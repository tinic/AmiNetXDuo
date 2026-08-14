/*
 * A Workbench screen that is not an Amiga.
 *
 *   node tools/web/console-mock.mjs [PORT] [W H DEPTH]     default 8098
 *
 * Then open http://127.0.0.1:8098/console.
 *
 * The same argument as tools/web/mock.mjs makes for the Shell: everything the
 * page does that is hard -- the tile apply, the XOR chain, the damage
 * rectangle, what the palette does when it changes mid-session -- is testable
 * without a 68020, and booting an emulator to find out whether a redraw is
 * one tile short is a minute a change.
 *
 * It speaks the PLACEHOLDER framing in client/console/tiles.ts and is
 * therefore provisional in exactly the same way.  It is a DEVELOPMENT AID and
 * asserts nothing; tools/web/console-selftest.mjs is where the checks are.
 *
 * /sample.pfs is served as well, so ?pfs=/sample.pfs exercises the player
 * half against a real HTTP fetch.
 *
 * SPDX-License-Identifier: MIT
 */

import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";
import { createServer } from "node:http";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import {
  encodeUpdate,
  makeGeometry,
  palette,
  synth,
  toPlanes,
  drawFrame,
  wordAligned,
  writePfs,
} from "./console-host.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, "..", "..");
const PAGE = process.env.AMINETXDUO_CONSOLE_PAGE ||
  join(ROOT, "build", "web", "console.html");

const PORT = Number(process.argv[2] || 8098);
const W = Number(process.argv[3] || 640);
const H = Number(process.argv[4] || 256);
const DEPTH = Number(process.argv[5] || 3);
const TILE_W = 32;
const TILE_H = 16;
const FPS = 12;
const GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

const screen = {
  width: W, height: H, depth: DEPTH, bytesPerRow: wordAligned(W),
};
const geom = makeGeometry(screen, TILE_W, TILE_H);
const rgb = palette(DEPTH);
const stride = screen.bytesPerRow * H * DEPTH;

/* -------------------------------------------------------------- framing -- */

function frame(op, payload) {
  const n = payload.length;
  const head = n < 126 ? Buffer.from([0x80 | op, n])
    : n < 65536 ? Buffer.concat([Buffer.from([0x80 | op, 126]),
                                 u16(n)])
      : Buffer.concat([Buffer.from([0x80 | op, 127]), u64(n)]);
  return Buffer.concat([head, payload]);
}

function u16(n) { const b = Buffer.alloc(2); b.writeUInt16BE(n); return b; }
function u64(n) { const b = Buffer.alloc(8); b.writeBigUInt64BE(BigInt(n)); return b; }

/* --------------------------------------------------------------- the run -- */

function makeSession(sock) {
  let prev = new Uint8Array(stride);
  let t = 0;
  let seq = 0;
  let keyframe = true;

  const word = (s) => sock.write(frame(0x1, Buffer.from(s, "latin1")));
  const binary = (b) => sock.write(frame(0x2, b));

  word("geom " + W + " " + H + " " + DEPTH + " " + screen.bytesPerRow +
       " " + TILE_W + " " + TILE_H);
  word("pal " + Buffer.from(rgb).toString("hex"));

  const timer = setInterval(() => {
    const next = toPlanes(drawFrame(W, H, DEPTH, t), W, H, DEPTH,
                          screen.bytesPerRow);
    const u = encodeUpdate(geom, prev, next, seq, keyframe);
    binary(u);
    prev = next;
    seq = (seq + 1) & 0xffff;
    keyframe = false;
    t++;
  }, Math.round(1000 / FPS));

  return {
    heard(w) {
      /* `refresh` is the only word with a consequence: the viewer asks for
         one when a sequence number skipped, which is the state the XOR chain
         cannot be recovered from. */
      if (w === "refresh") {
        keyframe = true;
        prev = new Uint8Array(stride);
        return;
      }
      /* Everything else is input, which nothing here consumes -- the point
         of printing it is that it can be seen arriving. */
      console.log("  %s", w);
    },
    stop() { clearInterval(timer); },
  };
}

/* ------------------------------------------------------------- the server -- */

const server = createServer((req, res) => {
  const path = (req.url || "/").split("?")[0];

  if (path === "/sample.pfs") {
    const body = writePfs(synth(W, H, DEPTH, 60));
    res.writeHead(200, {
      "content-type": "application/octet-stream",
      "content-length": body.length,
    });
    res.end(body);
    return;
  }

  if (path !== "/console" && path !== "/") {
    res.writeHead(404).end("no");
    return;
  }

  let body;
  try {
    body = readFileSync(PAGE);
  } catch {
    res.writeHead(500).end("build/web/console.html is not there -- run " +
                           "node tools/web/build-console.mjs");
    return;
  }
  res.writeHead(200, {
    "content-type": "text/html; charset=utf-8",
    "content-length": body.length,
  });
  res.end(body);
});

server.on("upgrade", (req, sock) => {
  const key = req.headers["sec-websocket-key"];
  sock.write("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n" +
             "Connection: Upgrade\r\nSec-WebSocket-Accept: " +
             createHash("sha1").update(key + GUID).digest("base64") + "\r\n\r\n");

  const session = makeSession(sock);
  let buf = Buffer.alloc(0);

  sock.on("data", (d) => {
    buf = Buffer.concat([buf, d]);
    for (;;) {
      if (buf.length < 2) return;
      const op = buf[0] & 0x0f;
      const masked = (buf[1] & 0x80) !== 0;
      let n = buf[1] & 0x7f;
      let at = 2;
      if (n === 126) { if (buf.length < 4) return; n = buf.readUInt16BE(2); at = 4; }
      else if (n === 127) { if (buf.length < 10) return; n = Number(buf.readBigUInt64BE(2)); at = 10; }
      const mask = masked ? buf.subarray(at, at + 4) : null;
      if (masked) at += 4;
      if (buf.length < at + n) return;

      const payload = Buffer.from(buf.subarray(at, at + n));
      if (mask !== null) for (let i = 0; i < n; i++) payload[i] ^= mask[i % 4];
      buf = buf.subarray(at + n);

      if (op === 0x1) session.heard(payload.toString("latin1"));
      else if (op === 0x9) sock.write(frame(0xa, payload));
      else if (op === 0x8) { session.stop(); sock.end(); }
    }
  });

  sock.on("close", () => session.stop());
  sock.on("error", () => session.stop());
});

server.listen(PORT, () => {
  console.log("mock Workbench on http://127.0.0.1:%d/console", PORT);
  console.log("%dx%dx%d, %dx%d tiles, %d fps, /sample.pfs for the player",
              W, H, DEPTH, TILE_W, TILE_H, FPS);
});
