/* SPDX-License-Identifier: MIT */

import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { createServer } from "node:http";
import { mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

import { endpoint, getFile, markerLine, putFile, runCommand, screenshot } from "./remote-control.mjs";
import { encodeFrame, makeGeometry, palette, synth } from "./console-host.mjs";

function sendFrame(socket, op, data) {
  const payload = Buffer.isBuffer(data) ? data : Buffer.from(data);
  const head = payload.length < 126 ? Buffer.from([0x80 | op, payload.length])
    : Buffer.from([0x80 | op, 126, payload.length >> 8, payload.length & 255]);
  socket.write(Buffer.concat([head, payload]));
}

function onFrames(socket, take) {
  let pending = Buffer.alloc(0);
  socket.on("data", (chunk) => {
    pending = Buffer.concat([pending, chunk]);
    for (;;) {
      if (pending.length < 2) return;
      const sizeCode = pending[1] & 0x7f;
      const head = sizeCode === 126 ? 4 : 2;
      if (pending.length < head) return;
      const size = sizeCode === 126 ? pending.readUInt16BE(2) : sizeCode;
      const masked = (pending[1] & 0x80) !== 0;
      const fullHead = head + (masked ? 4 : 0);
      if (pending.length < fullHead + size) return;
      const payload = Buffer.from(pending.subarray(fullHead, fullHead + size));
      if (masked) for (let i = 0; i < size; i++) payload[i] ^= pending[head + (i & 3)];
      const op = pending[0] & 0x0f;
      pending = pending.subarray(fullHead + size);
      if (op !== 8) take(op, payload);
    }
  });
}

async function fixture(options = {}) {
  const files = new Map([["/DH0/hello.txt", Buffer.from("hello\n")]]);
  const received = [];
  const upgrades = [];
  const sockets = new Set();
  const sample = synth(64, 64, 3, 1);
  const geom = makeGeometry(sample.screen, 4, 4);
  const picture = encodeFrame(geom, new Uint8Array(sample.stride),
                              sample.frames.subarray(0, sample.stride), 1);
  const pictureAfterAction = encodeFrame(geom, sample.frames.slice(0, sample.stride),
                                         sample.frames.subarray(0, sample.stride), 2);
  const server = createServer((req, res) => {
    if (req.method === "GET") {
      const data = files.get(req.url);
      res.writeHead(data ? 200 : 404);
      res.end(data);
    } else if (req.method === "PUT") {
      const parts = [];
      req.on("data", (part) => parts.push(part));
      req.on("end", () => {
        files.set(req.url, Buffer.concat(parts));
        res.writeHead(201);
        res.end();
      });
    } else { res.writeHead(405); res.end(); }
  });
  server.on("upgrade", (req, socket) => {
    upgrades.push(req.url);
    sockets.add(socket);
    socket.on("close", () => sockets.delete(socket));
    const key = req.headers["sec-websocket-key"];
    const accept = createHash("sha1").update(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")
      .digest("base64");
    socket.write("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n" +
      "Connection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n");
    if (req.url === "/shell?session=1" || req.url === "/shell?session=0") {
      let marker = "";
      onFrames(socket, (op, data) => {
        assert.equal(op, 2);
        const s = data.toString("latin1");
        received.push(s);
        const start = s.match(/Echo "(ANXD[A-F0-9]+) BEGIN"/);
        if (start) {
          marker = start[1];
          sendFrame(socket, 2, `Echo "${marker} BEGIN"\r\n${marker} BEGIN\r\n`);
        } else if (marker && s.includes("RC=$RC")) {
          const failing = received.includes("False");
          sendFrame(socket, 2, `${failing ? "False\r\nFailed" : "Version\r\nAmigaOS 3.1"}\r\n` +
            `Echo "${marker} RC=$RC"\r\n${marker} RC=${failing ? 5 : 0}\r\n`);
        }
      });
    } else if (req.url === "/console") {
      onFrames(socket, (op, data) => {
        assert.equal(op, 1);
        const word = data.toString("latin1");
        received.push(word);
        if (options.replyAfterAction !== false && word.endsWith(" 0") &&
            (word.startsWith("m ") || word.startsWith("ku "))) {
          setTimeout(() => sendFrame(socket, 2, pictureAfterAction), 25);
        }
      });
      setTimeout(() => {
        sendFrame(socket, 1, "geom 64 64 3 8 4 4 0");
        sendFrame(socket, 2, picture);
        sendFrame(socket, 1, "pal " + palette(3).toString("hex"));
      }, 20);
    }
  });
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  return { base: `http://127.0.0.1:${server.address().port}`, files, received, upgrades,
           close: () => new Promise((resolve) => {
             for (const socket of sockets) socket.destroy();
             server.closeAllConnections();
             server.close(resolve);
           }) };
}

test("origin/path checks and marker parsing", () => {
  assert.equal(endpoint("http://amiga.local:8080", "/DH0/a b"), "http://amiga.local:8080/DH0/a%20b");
  assert.throws(() => endpoint("ftp://amiga", "/DH0/a"));
  assert.throws(() => endpoint("http://amiga", "//elsewhere/a"));
  assert.deepEqual(markerLine("\nANXD123 RC=7\r\n", "ANXD123", "end"),
                   { start: 1, end: 15, rc: 7 });
  assert.deepEqual(markerLine("\x0f5.Workbench:> ANXD123 RC=7\n", "ANXD123", "end"),
                   { start: 0, end: 28, rc: 7 });
  assert.equal(markerLine('Echo "ANXD123 RC=$RC"\n', "ANXD123", "end"), null);
});

test("graphical action never returns a stale pre-action screenshot", async () => {
  const f = await fixture({ replyAfterAction: false });
  const dir = mkdtempSync(join(tmpdir(), "anxd-remote-stale-"));
  try {
    const png = join(dir, "stale.png");
    await assert.rejects(() => screenshot(f.base, png, 300,
                                          { kind: "click", x: 1, y: 1 }), /timed out/);
    assert.throws(() => readFileSync(png));
  } finally {
    rmSync(dir, { recursive: true, force: true });
    await f.close();
  }
});

test("Shell produces one bounded result with an exit code", async () => {
  const f = await fixture();
  try {
    const r = await runCommand(f.base, "Version", { timeoutMs: 3000 });
    assert.equal(r.exitCode, 0);
    assert.equal(r.output, "AmigaOS 3.1\n");
    const slot0 = await runCommand(f.base, "Version", { timeoutMs: 3000, session: 0 });
    assert.equal(slot0.exitCode, 0);
    assert.deepEqual(f.upgrades.slice(0, 2), ["/shell?session=1", "/shell?session=0"]);
    assert.ok(f.received.some((s) => s === "Version"));
    const failed = await runCommand(f.base, "False", { timeoutMs: 3000 });
    assert.equal(failed.exitCode, 5);
    assert.equal(failed.output, "Failed\n");
    await assert.rejects(() => runCommand(f.base, "Version\nDelete ALL"));
  } finally { await f.close(); }
});

test("WebDAV GET/PUT and console PNG with click", async () => {
  const f = await fixture();
  const dir = mkdtempSync(join(tmpdir(), "anxd-remote-test-"));
  try {
    const local = join(dir, "file.txt");
    await getFile(f.base, "/DH0/hello.txt", local);
    assert.equal(readFileSync(local, "utf8"), "hello\n");
    await putFile(f.base, local, "/DH0/new.txt");
    assert.equal(f.files.get("/DH0/new.txt").toString(), "hello\n");
    const png = join(dir, "screen.png");
    const r = await screenshot(f.base, png, 3000, { kind: "click", x: 10, y: 20 });
    assert.equal(r.width, 64);
    const image = readFileSync(png);
    assert.equal(image.toString("hex", 0, 8), "89504e470d0a1a0a");
    assert.equal(image.readUInt32BE(16), 64);
    assert.equal(image.readUInt32BE(20), 64);
    assert.ok(f.received.includes("m 10 20 1"));
    assert.ok(f.received.includes("m 10 20 0"));
    await screenshot(f.base, join(dir, "key.png"), 3000, { kind: "key", code: "Enter" });
    assert.ok(f.received.includes("kd 68 0"));
    assert.ok(f.received.includes("ku 68 0"));
  } finally {
    rmSync(dir, { recursive: true, force: true });
    await f.close();
  }
});
