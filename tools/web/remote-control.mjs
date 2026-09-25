#!/usr/bin/env node
/*
 * Host-side, machine-readable access to httpd's existing interfaces.
 * No code runs on the Amiga beyond the Shell commands the caller supplies.
 *
 * SPDX-License-Identifier: MIT
 */

import { randomBytes } from "node:crypto";
import { createReadStream, createWriteStream, statSync, writeFileSync } from "node:fs";
import { pipeline } from "node:stream/promises";
import { Readable } from "node:stream";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { inflateSync } from "node:zlib";

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, "..", "..");

export function endpoint(base, path) {
  const url = new URL(base);
  if (url.protocol !== "http:" && url.protocol !== "https:") {
    throw new Error("--url must use http:// or https://");
  }
  if (url.username || url.password || url.search || url.hash || url.pathname !== "/") {
    throw new Error("--url must be an origin, such as http://amiga.local:8080");
  }
  if (!path.startsWith("/") || path.startsWith("//") || path.includes("?") || path.includes("#")) {
    throw new Error("remote path must be an absolute HTTP path without a query");
  }
  return new URL(path, url).href;
}

function socketUrl(base, path, search = "") {
  const url = new URL(endpoint(base, path));
  url.protocol = url.protocol === "https:" ? "wss:" : "ws:";
  url.search = search;
  return url.href;
}

function timeout(ms, label) {
  return new Promise((_, reject) => {
    setTimeout(() => reject(new Error(`${label} timed out after ${ms} ms`)), ms).unref();
  });
}

async function openSocket(url, ms) {
  const ws = new WebSocket(url);
  ws.binaryType = "arraybuffer";
  try {
    await Promise.race([
      new Promise((resolve, reject) => {
        ws.addEventListener("open", resolve, { once: true });
        ws.addEventListener("error", (event) => {
          const detail = event.error?.message || event.message || "connection failed";
          reject(new Error(`WebSocket ${url}: ${detail}`));
        }, { once: true });
        ws.addEventListener("close", () => reject(new Error(`WebSocket closed before opening: ${url}`)), { once: true });
      }),
      timeout(ms, "WebSocket connection"),
    ]);
    return ws;
  } catch (e) {
    ws.close();
    throw e;
  }
}

/* A real Shell echoes keystrokes as well as command output. The marker must
 * occupy its own output line; the echoed Echo command cannot satisfy this. */
export function markerLine(text, marker, kind) {
  const line = kind === "begin" ? marker + " BEGIN" : marker + " RC=";
  let start = 0;
  while (start < text.length) {
    const end = text.indexOf("\n", start);
    if (end < 0) return null;
    const value = text.slice(start, end).replace(/\r$/, "");
    /* AmigaDOS prints its form-feed-prefixed prompt on the same line as the
     * next command's output.  Match only the actual Echo result, not the
     * echoed Echo command or an arbitrary line containing our token. */
    const markerAt = value.indexOf(line);
    const prefix = markerAt >= 0 ? value.slice(0, markerAt) : "";
    if (markerAt >= 0 && (prefix === "" || /^\x0f[^\n]*> $/.test(prefix))) {
      const result = value.slice(markerAt);
      if (kind === "begin" && result === line) return { start, end: end + 1 };
      const m = kind === "end" && result.match(new RegExp("^" + line + "(-?[0-9]+)$"));
      if (m) return { start, end: end + 1, rc: Number(m[1]) };
    }
    start = end + 1;
  }
  return null;
}

function latin1(text) {
  if (/[\x00-\x1f\x7f-\x9f]/.test(text)) {
    throw new Error("command must be one printable line");
  }
  for (const ch of text) if (ch.codePointAt(0) > 255) {
    throw new Error("command must be Latin-1, as the AmigaDOS Shell is");
  }
  if (text.length > 512) throw new Error("command is too long (512 characters maximum)");
  return Buffer.from(text, "latin1");
}

function withoutTerminalEcho(text, command, marker) {
  text = text.replace(/\x0f[^\n]*?> /g, "");
  const lines = text.split("\n");
  const first = lines[0]?.replace(/\r$/, "");
  if (first === command || first?.endsWith("> " + command)) lines.shift();
  const echoedEnd = `Echo "${marker} RC=$RC"`;
  for (let i = lines.length - 1; i >= 0; i--) {
    const line = lines[i].replace(/\r$/, "");
    if (line === echoedEnd || line.endsWith("> " + echoedEnd)) {
      lines.splice(i, 1);
      break;
    }
  }
  return lines.join("\n");
}

export async function runCommand(base, command, options = {}) {
  const ms = options.timeoutMs ?? 30000;
  const maxOutput = options.maxOutput ?? 1024 * 1024;
  const session = options.session ?? 1;
  if (session !== 0 && session !== 1) throw new Error("session must be 0 or 1");
  const bytes = latin1(command);
  if (bytes.length === 0) throw new Error("command is empty");
  const marker = "ANXD" + randomBytes(12).toString("hex").toUpperCase();
  const ws = await openSocket(socketUrl(base, "/shell", `?session=${session}`), Math.min(ms, 10000));
  const beganAt = Date.now();
  let stream = "";
  let begin = null;
  let finished = false;
  let oversized = false;
  let result;
  try {
    result = await Promise.race([
      new Promise((resolve, reject) => {
        ws.addEventListener("message", async (event) => {
          if (typeof event.data === "string" || finished) return;
          const chunk = event.data instanceof ArrayBuffer
            ? Buffer.from(event.data).toString("latin1")
            : Buffer.from(await event.data.arrayBuffer()).toString("latin1");
          stream += chunk.replace(/\r\n/g, "\n");
          if (stream.length > maxOutput + 8192) {
            oversized = true;
            reject(new Error(`Shell output exceeded ${maxOutput} bytes`));
            return;
          }
          if (begin === null) {
            begin = markerLine(stream, marker, "begin");
            if (begin !== null) {
              ws.send(bytes);
              ws.send(Buffer.from("\nEcho \"" + marker + " RC=$RC\"\n", "latin1"));
            }
          }
          if (begin !== null) {
            const end = markerLine(stream, marker, "end");
            if (end !== null && end.start >= begin.end) {
              finished = true;
              resolve({
                command,
                exitCode: end.rc,
                output: withoutTerminalEcho(stream.slice(begin.end, end.start), command, marker),
                durationMs: Date.now() - beganAt,
              });
            }
          }
        });
        ws.addEventListener("close", () => reject(new Error("Shell closed before its completion marker")), { once: true });
        ws.addEventListener("error", () => reject(new Error("Shell WebSocket failed")), { once: true });
        ws.send(Buffer.from(`FailAt 9999\nEcho "${marker} BEGIN"\n`, "latin1"));
      }),
      timeout(ms, "Shell command"),
    ]);
  } catch (e) {
    if (!oversized && ws.readyState === WebSocket.OPEN) ws.send("break");
    throw e;
  } finally {
    ws.close();
  }
  return result;
}

export async function getFile(base, remote, local, ms = 30000) {
  const response = await fetch(endpoint(base, remote), { signal: AbortSignal.timeout(ms) });
  if (!response.ok || !response.body) throw new Error(`GET ${remote}: HTTP ${response.status}`);
  await pipeline(Readable.fromWeb(response.body), createWriteStream(local, { flags: "w" }));
  return { remote, local, bytes: statSync(local).size, status: response.status };
}

export async function putFile(base, local, remote, ms = 30000) {
  const size = statSync(local).size;
  const response = await fetch(endpoint(base, remote), {
    method: "PUT",
    headers: { "Content-Length": String(size) },
    body: createReadStream(local),
    duplex: "half",
    signal: AbortSignal.timeout(ms),
  });
  if (!response.ok) throw new Error(`PUT ${remote}: HTTP ${response.status}`);
  await response.arrayBuffer();
  return { remote, local, bytes: size, status: response.status };
}

async function imageCodec() {
  const esbuild = await import("esbuild");
  const source = join(ROOT, "src", "tools", "web", "client", "console");
  const build = await esbuild.build({
    stdin: {
      contents: 'export * from "./planar"; export * from "./tiles"; export * from "./rawkey";',
      resolveDir: source,
      loader: "ts",
    },
    bundle: true, format: "esm", platform: "node", target: "es2020", write: false,
  });
  return import("data:text/javascript;base64," + Buffer.from(build.outputFiles[0].contents).toString("base64"));
}

export async function screenshot(base, local, ms = 30000, action = null) {
  const [codec, { writePng }] = await Promise.all([
    imageCodec(), import("./console-host.mjs"),
  ]);
  const ws = await openSocket(socketUrl(base, "/console"), Math.min(ms, 10000));
  let geom = null;
  let planes = null;
  let scratch = null;
  let palette = null;
  let frameSeen = false;
  let expectedSeq = -1;
  let settled = false;
  let actionSent = false;
  let actionReady = false;
  try {
    return await Promise.race([
      new Promise((resolve, reject) => {
        const finish = () => {
          if (settled || !geom || !frameSeen || !palette) return;
          if (action && !actionReady) {
            if (!actionSent) {
              actionSent = true;
              try {
                if (action.kind === "click") {
                  const { x, y } = action;
                  if (!Number.isInteger(x) || !Number.isInteger(y) ||
                      x < 0 || y < 0 || x >= geom.screen.width || y >= geom.screen.height) {
                    throw new Error(`click is outside ${geom.screen.width}x${geom.screen.height}`);
                  }
                  ws.send(`m ${x} ${y} 1`);
                  setTimeout(() => {
                    if (ws.readyState !== WebSocket.OPEN) return;
                    ws.send(`m ${x} ${y} 0`);
                    frameSeen = false;
                    actionReady = true;
                    setTimeout(() => {
                      if (!settled && !frameSeen && ws.readyState === WebSocket.OPEN) ws.send("refresh");
                    }, 500);
                  }, 80);
                } else if (action.kind === "key") {
                  const raw = codec.RAWKEY[action.code];
                  if (raw === undefined) throw new Error(`unknown physical key ${action.code}`);
                  ws.send(`kd ${raw} 0`);
                  setTimeout(() => {
                    if (ws.readyState !== WebSocket.OPEN) return;
                    ws.send(`ku ${raw} 0`);
                    frameSeen = false;
                    actionReady = true;
                    setTimeout(() => {
                      if (!settled && !frameSeen && ws.readyState === WebSocket.OPEN) ws.send("refresh");
                    }, 500);
                  }, 80);
                } else throw new Error(`unknown action ${action.kind}`);
              } catch (e) { reject(e); }
            }
            return;
          }
          settled = true;
          const { width, height } = geom.screen;
          const pixels = new Uint32Array(width * height);
          codec.decodeInto(geom.screen, planes, 0, codec.renderPalette(geom.screen, palette), pixels);
          writeFileSync(local, writePng(new Uint8Array(pixels.buffer), width, height));
          resolve({ local, width, height, format: "png", pixels: "native",
                    ...(action ? { action } : {}) });
        };
        ws.addEventListener("message", async (event) => {
          try {
            if (settled) return;
            if (typeof event.data === "string") {
              if (event.data.startsWith("geom ")) {
                geom = codec.geometryFromWord(event.data);
                planes = new Uint8Array(codec.frameBytes(geom.screen));
                scratch = new Uint8Array(codec.scratchBytes(geom));
                palette = codec.palColours(geom.screen) === 0 ? new Uint8Array(0) : null;
                frameSeen = false;
                expectedSeq = -1;
              } else if (event.data.startsWith("pal ") && geom) {
                palette = codec.paletteFromWord(event.data, codec.palColours(geom.screen));
              }
              finish();
              return;
            }
            if (!geom) return;
            let frame = Buffer.from(event.data instanceof ArrayBuffer
              ? event.data : await event.data.arrayBuffer());
            if (frame.length >= 4 && (frame[1] & 1)) {
              frame = Buffer.concat([frame.subarray(0, 4), inflateSync(frame.subarray(4))]);
              frame[1] &= ~1;
            }
            const decoded = codec.applyUpdate(geom, frame, planes, scratch);
            if (expectedSeq >= 0 && decoded.seq !== expectedSeq) {
              planes.fill(0);
              frameSeen = false;
              expectedSeq = -1;
              ws.send("refresh");
              return;
            }
            expectedSeq = (decoded.seq + 1) & 0xffff;
            frameSeen = true;
            finish();
          } catch (e) { reject(e); }
        });
        ws.addEventListener("close", () => reject(new Error("console closed before a complete frame")), { once: true });
        ws.addEventListener("error", () => reject(new Error("console WebSocket failed")), { once: true });
      }),
      timeout(ms, "console screenshot"),
    ]);
  } finally {
    settled = true;
    ws.close();
  }
}

function usage() {
  return `Usage: node tools/web/remote-control.mjs [--url http://amiga.local:8080] [--session 0|1] [--timeout SECONDS] [--max-output BYTES] COMMAND ...

Set ANXD_URL instead of repeating --url.
Shell commands use session 1 by default; /shell in a browser uses session 0.

Commands:
  run 'AmigaDOS command'            JSON: exitCode, terminal output, duration
  get /DH0/file local-file         Download by WebDAV/HTTP GET
  put local-file /DH0/file         Upload by WebDAV PUT (overwrites)
  screenshot local.png             Capture /console to a native-size PNG
  click X Y local.png              Left-click native screen coordinates, then PNG
  key PHYSICAL-CODE local.png      Press a key (e.g. Enter, F1), then PNG

The Amiga's httpd has no access control. This tool does not add any.
`;
}

async function main(args) {
  let base = process.env.ANXD_URL ?? "";
  let ms = 30000;
  let maxOutput = 1024 * 1024;
  let session = 1;
  while (args.length && args[0].startsWith("--")) {
    const flag = args.shift();
    if (flag === "--url") base = args.shift() ?? "";
    else if (flag === "--session") {
      session = Number(args.shift());
      if (session !== 0 && session !== 1) throw new Error("--session must be 0 or 1");
    }
    else if (flag === "--timeout") {
      const seconds = Number(args.shift());
      if (!Number.isFinite(seconds) || seconds <= 0 || seconds > 3600) throw new Error("--timeout must be greater than 0 and at most 3600 seconds");
      ms = Math.round(seconds * 1000);
    } else if (flag === "--max-output") {
      maxOutput = Number(args.shift());
      if (!Number.isInteger(maxOutput) || maxOutput < 1024 || maxOutput > 64 * 1024 * 1024) {
        throw new Error("--max-output must be 1024..67108864 bytes");
      }
    } else if (flag === "--help") { process.stdout.write(usage()); return; }
    else throw new Error(`unknown option ${flag}`);
  }
  if (!base) throw new Error("--url or ANXD_URL is required");
  const [verb, ...operands] = args;
  let result;
  if (verb === "run" && operands.length === 1) result = await runCommand(base, operands[0], { timeoutMs: ms, maxOutput, session });
  else if (verb === "get" && operands.length === 2) result = await getFile(base, ...operands, ms);
  else if (verb === "put" && operands.length === 2) result = await putFile(base, ...operands, ms);
  else if (verb === "screenshot" && operands.length === 1) result = await screenshot(base, operands[0], ms);
  else if (verb === "click" && operands.length === 3) {
    result = await screenshot(base, operands[2], ms,
                              { kind: "click", x: Number(operands[0]), y: Number(operands[1]) });
  } else if (verb === "key" && operands.length === 2) {
    result = await screenshot(base, operands[1], ms, { kind: "key", code: operands[0] });
  }
  else throw new Error("bad command or operands\n" + usage());
  process.stdout.write(JSON.stringify(result) + "\n");
  if (verb === "run" && result.exitCode !== 0) process.exitCode = 1;
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) {
  main(process.argv.slice(2)).catch((e) => {
    process.stderr.write(JSON.stringify({ error: e.message }) + "\n");
    process.exitCode = 2;
  });
}
