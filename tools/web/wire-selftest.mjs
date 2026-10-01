/*
 * Drive the real Wire class outside a browser and check the one decision the
 * /shell prompt replay rests on: whether `?fresh=1` is on a given upgrade.
 *
 *   node tools/web/wire-selftest.mjs
 *
 * A client that sends `fresh=1` when it should not will hand the server a
 * second copy of the prompt on a same-page reconnect -- the exact duplicate
 * the gate exists to stop -- and a client that forgets it on a first load
 * leaves the reattached terminal blank.  "tsc says it typechecks" proves
 * neither of those: the bug is in which branch runs, not in the types.  What
 * is checked here is the branch selection, against the two client-side facts
 * the fix is built on:
 *
 *   - `everOpened` flips true ONLY in onopen, so a first load sends `fresh=1`
 *     and a reconnect -- open, then closed, then opened again -- does not;
 *   - onclose does NOT flip it, so a refused first upgrade (onclose with no
 *     onopen) can retry and still carry `fresh=1`.
 *
 * The module under test is the REAL one.  esbuild bundles client/wire.ts into
 * build/web/ and this imports that, rather than a copy of the branch that
 * would agree with itself.  `WebSocket` and `location` are the two browser
 * globals it touches, stubbed here so the class can run headless.
 *
 * SPDX-License-Identifier: MIT
 */

import { mkdirSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

import * as esbuild from "esbuild";

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, "..", "..");
const SRC = join(ROOT, "src", "tools", "web", "client");
const OUTDIR = join(ROOT, "build", "web");

/* --------------------------------------------------------- the module --- */

mkdirSync(OUTDIR, { recursive: true });

const core = join(OUTDIR, "wire-selftest-core.mjs");
await esbuild.build({
  stdin: {
    contents: 'export * from "./wire";\n',
    resolveDir: SRC,
    loader: "ts",
  },
  bundle: true,
  format: "esm",
  target: "es2020",
  outfile: core,
});

const M = await import(pathToFileURL(core).href);

/* ------------------------------------------------------------- the rig -- */

let failures = 0;

function ok(name, cond, detail) {
  if (cond) {
    console.log("ok    %s", name);
  } else {
    failures++;
    console.log("FAIL  %s%s", name, detail ? " -- " + detail : "");
  }
}

/* The browser globals wire.ts reaches for, made headless.  URLSearchParams is
   already a Node global; the other two are not, so they are stood up here. */
class FakeWebSocket {
  static CONNECTING = 0;
  static OPEN = 1;
  static CLOSING = 2;
  static CLOSED = 3;
  static instances = [];

  constructor(url) {
    this.url = url;
    this.readyState = FakeWebSocket.CONNECTING;
    this.binaryType = "";
    this.onopen = null;
    this.onmessage = null;
    this.onclose = null;
    this.onerror = null;
    FakeWebSocket.instances.push(this);
  }

  send(_data) {}
  close(_code, _reason) { this.readyState = FakeWebSocket.CLOSED; }
}

globalThis.WebSocket = FakeWebSocket;
globalThis.location = {
  protocol: "http:",
  host: "a1200",
  pathname: "/shell",
  search: "",
};

/* Drive a socket through the states a browser reports, in the order the real
   one fires the events: readyState moves first, then the handler runs. */
function open(ws) {
  ws.readyState = FakeWebSocket.OPEN;
  ws.onopen();
}
function closeOpened(ws) {
  ws.readyState = FakeWebSocket.CLOSED;
  ws.onclose({ code: 1000, reason: "" });
}
function closeRefused(ws) {
  ws.readyState = FakeWebSocket.CLOSED;
  ws.onclose({ code: 1006, reason: "" });
}

const handlers = { onText() {}, onWord() {}, onState() {} };
const last = () => FakeWebSocket.instances[FakeWebSocket.instances.length - 1];

/* ---------------------------------------------------------- the checks -- */

/* First load: a fresh page has an empty terminal, so fresh=1 rides along. */
{
  const w = new M.Wire(handlers);
  w.connect();
  ok("first_load_sends_fresh",
     last().url === "ws://a1200/shell?fresh=1", last().url);
}

/* Same-page reconnect: open, close, open again.  The retained terminal still
   shows the prompt, so the second upgrade must NOT carry fresh=1. */
{
  const w = new M.Wire(handlers);
  w.connect();
  const s = last();
  open(s);
  closeOpened(s);
  w.connect();
  ok("reconnect_after_open_omits_fresh",
     last().url === "ws://a1200/shell", last().url);
}

/* Refused first upgrade: onclose with no onopen is a refusal, not a close, so
   everOpened must stay false and the retry must still send fresh=1. */
{
  const w = new M.Wire(handlers);
  w.connect();
  ok("refused_first_upgrade_sends_fresh",
     last().url === "ws://a1200/shell?fresh=1", last().url);
  closeRefused(last());
  w.connect();
  ok("refused_retry_still_sends_fresh",
     last().url === "ws://a1200/shell?fresh=1", last().url);
}

/* fresh=1 is added alongside the session, not instead of it, so a first load
   into a chosen slot still names the slot and the replay. */
{
  globalThis.location.search = "?session=1";
  const w = new M.Wire(handlers);
  w.connect();
  ok("first_load_keeps_session_and_fresh",
     last().url === "ws://a1200/shell?session=1&fresh=1", last().url);
  globalThis.location.search = "";
}

/* A socket that is still open (readyState OPEN, ws non-null) is guarded in
   connect(): a second connect() must not create a second upgrade. */
{
  const w = new M.Wire(handlers);
  w.connect();
  open(last());
  const count = FakeWebSocket.instances.length;
  w.connect();
  ok("open_socket_is_not_reupgraded",
     FakeWebSocket.instances.length === count,
     "grew by " + (FakeWebSocket.instances.length - count));
}

if (failures) {
  console.log("wire_selftest_failed=%d", failures);
  process.exit(1);
}
console.log("wire_selftest_failed=0 (all checks)");
