/*
 * A Workbench screen that is not an Amiga.
 *
 *   node tools/web/console-mock.mjs [PORT] [CAPTURE.pfs]
 *   node tools/web/console-mock.mjs [PORT] [W H DEPTH]      synthesised
 *
 * Default port 8098; then open http://127.0.0.1:8098/console.
 *
 * A real capture is the better argument now that there are some: it replays
 * the frames an A1200 actually produced, through the same encoder the Amiga
 * will run, so what the page draws is what a session will look like.
 *
 * The same argument as tools/web/mock.mjs makes for the Shell: everything the
 * page does that is hard -- the tile apply, the XOR chain, the damage
 * rectangle, what the palette does when it changes mid-session -- is testable
 * without a 68020, and booting an emulator to find out whether a redraw is
 * one tile short is a minute a change.
 *
 * The frames go out in the encoder's own wire format -- see
 * include/aminetxduo/rfb_encode.h -- coded by console-host.mjs, which is a
 * second implementation of it and not the one that ships.  A DEVELOPMENT AID
 * that asserts nothing; console-selftest.mjs is where the checks are.
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
  encodeFrame,
  makeGeometry,
  synth,
  writePfs,
} from "./console-host.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, "..", "..");
const PAGE = process.env.AMINETXDUO_CONSOLE_PAGE ||
  join(ROOT, "src", "tools", "web", "console.html");

const PORT = Number(process.argv[2] || 8098);
const CAPTURE = (process.argv[3] || "").endsWith(".pfs") ? process.argv[3] : null;

/* 640x256 and two planes deep, because that is what stock Workbench 3.1
   comes up as on any machine. */
const W = Number(CAPTURE ? 0 : process.argv[3] || 640);
const H = Number(CAPTURE ? 0 : process.argv[4] || 256);
const DEPTH = Number(CAPTURE ? 0 : process.argv[5] || 2);
/* Bytes and rows, matching rfb_geom.  16x8 is the middle of rfbbench's
   sweep. */
const TILE_W = 16;
const TILE_H = 8;
const FPS = 12;
const GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

const cap = CAPTURE !== null ? readCapture(CAPTURE)
                             : synth(W, H, DEPTH, 60);
const screen = cap.screen;
const geom = makeGeometry(screen, TILE_W, TILE_H);
const rgb = cap.rgb;
const stride = cap.stride;

/* The .pfs reader, which is the one the page has in TypeScript; kept short
   here rather than shared, because sharing it would mean the mock and the
   page agreeing with each other about a file neither of them wrote. */
function readCapture(path) {
  const b = readFileSync(path);
  if (b.toString("latin1", 0, 4) !== "PFS2") throw new Error(path + " is not a .pfs");
  const s = {
    width: b.readUInt16BE(4),
    height: b.readUInt16BE(6),
    depth: b[8],
    bytesPerRow: b.readUInt16BE(10),
  };
  const frameCount = b.readUInt16BE(12);
  const palBytes = 3 * (1 << s.depth);
  const st = s.bytesPerRow * s.height * s.depth;
  return {
    screen: s,
    rgb: b.subarray(16, 16 + palBytes),
    frames: b.subarray(16 + palBytes, 16 + palBytes + frameCount * st),
    frameCount,
    stride: st,
    file: b,
  };
}

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
  /* The shadow, which the encoder keeps and the receiver mirrors.  Zeroed,
     so the first frame codes as a delta from a blank screen -- which is what
     a viewer that has just connected has. */
  let shadow = new Uint8Array(stride);
  let t = 0;
  let seq = 0;

  const word = (s) => sock.write(frame(0x1, Buffer.from(s, "latin1")));
  const binary = (b) => sock.write(frame(0x2, b));

  word("geom " + screen.width + " " + screen.height + " " + screen.depth +
       " " + screen.bytesPerRow + " " + TILE_W + " " + TILE_H);
  word("pal " + Buffer.from(rgb).toString("hex"));

  const timer = setInterval(() => {
    const at = (t % cap.frameCount) * stride;
    const next = cap.frames.subarray(at, at + stride);
    binary(encodeFrame(geom, shadow, next, seq));
    seq = (seq + 1) & 0xffff;
    t++;
  }, Math.round(1000 / FPS));

  return {
    heard(w) {
      /* `refresh` is the only word with a consequence: the viewer asks for
         one when a sequence number skipped, which is the state the XOR chain
         cannot be recovered from. */
      if (w === "refresh") {
        shadow = new Uint8Array(stride);
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
    const body = cap.file ?? writePfs(cap);
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
    res.writeHead(500).end("src/tools/web/console.html is not there -- run " +
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
  console.log("%s: %dx%dx%d, %d frames, %dx%d tiles, %d fps, " +
              "/sample.pfs for the player",
              CAPTURE ?? "synthesised", screen.width, screen.height,
              screen.depth, cap.frameCount, TILE_W, TILE_H, FPS);
});
