/*
 * An AmigaDOS Shell that is not an Amiga.
 *
 *   node tools/web/mock.mjs [PORT]        default 8099, then open /shell
 *
 * WHY THIS EXISTS
 *
 *   Everything the page does that is hard -- the line editor's anchor
 *   arithmetic, the 0x9B rewrite, what a wrapped line does when output
 *   arrives in the middle of it -- is testable without a 68020, and booting
 *   an emulator to see whether a redraw is one column out is a minute a
 *   change.  This serves the same file over the same protocol and answers
 *   with the same bytes an A1200 sends, 0x9B and all.
 *
 *   It is a DEVELOPMENT AID and asserts nothing.  tests/tools/run-wsterm.sh
 *   and tests/tools/httpd-drill.py --terminal are the contract; this is the
 *   thing you look at while writing the code they then check.
 *
 * SPDX-License-Identifier: MIT
 */

import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";
import { createServer } from "node:http";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
/* AMINETXDUO_MOCK_PAGE serves a different file, which is how the page it
   replaced was put side by side with it. */
const PAGE = process.env.AMINETXDUO_MOCK_PAGE ||
  join(HERE, "..", "..", "src", "tools", "web", "terminal.html");
const PORT = Number(process.argv[2] || 8099);
const GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

const CSI = "\u009B";                       /* what AmigaDOS actually writes */

/* ------------------------------------------------------------- the Shell -- */

function makeShell(send, close) {
  let dir = "Work:";
  let busy = null;
  let line = "";

  const prompt = () => send("1." + dir + "> ");

  const FILES = [
    ["Docs", "Dir"],
    ["Devs", "Dir"],
    ["readme.txt", 21],
    ["Caf\u00E9 notes.txt", 44],      /* Latin-1, and the point of Latin-1 */
    ["startup-sequence", 1130],
  ];

  const commands = {
    dir() {
      for (const [name, size] of FILES) {
        send("  " + name.padEnd(24) +
             (size === "Dir" ? "  Dir" : String(size).padStart(5)) + "\n");
      }
    },
    list() {
      send("Directory \"" + dir + "\" on " + new Date().toDateString() + "\n");
      for (const [name, size] of FILES) {
        send(name.padEnd(26) +
             (size === "Dir" ? "     Dir" : String(size).padStart(8)) +
             " ----rwed Today     11:04:12\n");
      }
      send(FILES.length + " files - 40 blocks used\n");
    },
    type() {
      send("The Amiga Shell reads its commands from a pipe here, and a pipe\n" +
           "has no console handler behind it, so nothing on this side echoes\n" +
           "what you type.  Every character you can see was drawn by the\n" +
           "browser.  Caf\u00E9, na\u00EFve, \u00FCber -- Latin-1, one byte each.\n");
    },
    echo(rest) { send(rest.replace(/\*e/g, "\u001B") + "\n"); },
    cd(rest) { if (rest) dir = rest.replace(/\/$/, ""); },
    version() { send("Kickstart 40.68, Workbench 40.42\n"); },
    colours() {
      for (let i = 0; i < 8; i++) {
        send(CSI + "3" + i + "m pen " + i + " " + CSI + "0m");
      }
      send("\n");
      send(CSI + "1mbold" + CSI + "0m " + CSI + "3mitalic" + CSI + "0m " +
           CSI + "4munderline" + CSI + "0m " + CSI + "7minverse" + CSI + "0m\n");
    },
    clear() { send(CSI + "0;0H" + CSI + "J"); },
    /* Something to interrupt.  A break has to land while this is running. */
    ping() {
      let n = 0;
      busy = setInterval(() => {
        send("PING 10.0.0.1: seq=" + (++n) + " time=" + (2 + n % 5) + " ms\n");
      }, 400);
    },
    wide() {
      send("x".repeat(200) + "\n");
    },
    endcli() { send("\n"); close(1000, "the Shell exited"); },
  };

  function run(text) {
    const [word, ...rest] = text.trim().split(/\s+/);
    const fn = commands[(word || "").toLowerCase()];

    if (!word) { prompt(); return; }
    if (!fn) {
      send(text.trim() + ": Unknown command\n");
      prompt();
      return;
    }

    fn(rest.join(" "));
    if (busy === null) prompt();
  }

  setTimeout(() => {
    send("New Shell process 4\n");
    prompt();
  }, 40);

  return {
    keys(bytes) {
      /* Byte for byte, exactly as the Shell's Read() would see it. */
      for (const b of bytes) {
        const c = String.fromCharCode(b);
        if (c === "\n" || c === "\r") { const t = line; line = ""; run(t); }
        else line += c;
      }
    },
    word(w) {
      if (w === "break") {
        if (busy !== null) { clearInterval(busy); busy = null; send("***BREAK\n"); }
        line = "";
        prompt();
      } else if (w === "eof") {
        send("\n");
        close(1000, "the Shell exited");
      }
    },
    stop() { if (busy !== null) clearInterval(busy); },
  };
}

/* ---------------------------------------------------------- the framing -- */

function frame(op, payload) {
  const n = payload.length;
  const head = n < 126 ? Buffer.from([0x80 | op, n])
    : n < 65536 ? Buffer.concat([Buffer.from([0x80 | op, 126]),
                                 (() => { const b = Buffer.alloc(2); b.writeUInt16BE(n); return b; })()])
      : Buffer.concat([Buffer.from([0x80 | op, 127]),
                       (() => { const b = Buffer.alloc(8); b.writeBigUInt64BE(BigInt(n)); return b; })()]);
  return Buffer.concat([head, payload]);
}

function latin1(s) {
  const b = Buffer.alloc(s.length);
  for (let i = 0; i < s.length; i++) b[i] = s.charCodeAt(i) & 0xff;
  return b;
}

const server = createServer((req, res) => {
  /* Without the query dropped, /shell?input=char is a 404 and the page
     never loads at all, which reads as the page having crashed. */
  const path = (req.url || "/").split("?")[0];
  if (path !== "/shell" && path !== "/") {
    res.writeHead(404).end("no");
    return;
  }
  const body = readFileSync(PAGE);
  res.writeHead(200, {
    "content-type": "text/html; charset=iso-8859-1",
    "content-length": body.length,
  });
  res.end(body);
});

server.on("upgrade", (req, sock) => {
  const key = req.headers["sec-websocket-key"];
  sock.write("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n" +
             "Connection: Upgrade\r\nSec-WebSocket-Accept: " +
             createHash("sha1").update(key + GUID).digest("base64") + "\r\n\r\n");

  const shell = makeShell(
    (text) => sock.write(frame(0x2, latin1(text))),
    (code, why) => {
      const b = Buffer.alloc(2 + why.length);
      b.writeUInt16BE(code);
      b.write(why, 2, "latin1");
      sock.write(frame(0x8, b));
      sock.end();
    });

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

      if (op === 0x2) shell.keys(payload);
      else if (op === 0x1) shell.word(payload.toString("latin1"));
      else if (op === 0x9) sock.write(frame(0xa, payload));
      else if (op === 0x8) { shell.stop(); sock.end(); }
    }
  });

  sock.on("close", () => shell.stop());
  sock.on("error", () => shell.stop());
});

server.listen(PORT, () => {
  console.log("mock AmigaDOS on http://127.0.0.1:%d/shell", PORT);
  console.log("commands: Dir List Type Echo Cd Version Colours Clear Ping Wide EndCLI");
});
