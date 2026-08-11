/*
 * THE PASSWORD, AS THE SCREEN HAS IT.
 *
 *   node tests/tools/wsconsole-page.mjs [--url URL] [--sshd PORT] [--user U]
 *
 * WHY THIS EXISTS AND tests/tools/wsterm-console.py IS NOT ENOUGH
 *
 *   The echo lives in the PAGE.  A drill that speaks the protocol and does not
 *   echo proves the server's half -- `mode raw` arrives, nothing typed comes
 *   back -- and cannot prove the thing anybody actually cares about, which is
 *   that the characters are not drawn.  So this drives the real built
 *   terminal.html in a real browser and reads the letters off the screen.
 *
 *   And it proves the test can SEE typing before it claims not to: a command
 *   typed in cooked mode must appear, in the same buffer, read the same way.
 *   Without that control, "the password is not on the screen" is a sentence a
 *   broken reader also produces.
 *
 * NO DEPENDENCIES, WHICH IS THE HOUSE RULE
 *
 *   No playwright, no puppeteer, nothing in package.json.  Chrome's own
 *   DevTools Protocol over the WebSocket and fetch that Node has had built in
 *   since 22, and a browser binary that is already on the machine.  The whole
 *   client is the sixty lines under "the protocol".
 *
 * SPDX-License-Identifier: MIT
 */

import { spawn } from "node:child_process";
import { existsSync, mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { globSync } from "node:fs";

/* ------------------------------------------------------------ arguments -- */

const arg = (name, fallback) => {
  const i = process.argv.indexOf("--" + name);
  return i >= 0 && i + 1 < process.argv.length ? process.argv[i + 1] : fallback;
};

const URL_ = arg("url", "http://127.0.0.1:18081/terminal");
const SSHD = arg("sshd", "2224");
const USER = arg("user", process.env.USER ?? "nobody");
const HOST_FROM_GUEST = "10.0.2.2";

/* Exists nowhere else in the run.  If it is on the screen, it was drawn. */
const SECRET = "Zx9Qv-notthepassword-Kw3";
/* And the control: this one MUST be on the screen, or the reader is broken. */
const VISIBLE = "CANSEEME";

let checks = 0;
const failures = [];
const check = (ok, what) => {
  checks++;
  if (!ok) {
    failures.push(what);
    console.log("  FAIL " + what);
  }
};

/* -------------------------------------------------------------- browser -- */

function findBrowser() {
  if (process.env.AMINETXDUO_CHROME) return process.env.AMINETXDUO_CHROME;

  const patterns = [
    join(process.env.HOME ?? "", "Library/Caches/ms-playwright/chromium_headless_shell-*/chrome-headless-shell-*/chrome-headless-shell"),
    join(process.env.HOME ?? "", "Library/Caches/ms-playwright/chromium-*/chrome-*/Chromium.app/Contents/MacOS/Chromium"),
    join(process.env.HOME ?? "", ".cache/ms-playwright/chromium_headless_shell-*/chrome-headless-shell-*/chrome-headless-shell"),
    join(process.env.HOME ?? "", ".cache/ms-playwright/chromium-*/chrome-linux/chrome"),
    "/usr/bin/chromium",
    "/usr/bin/chromium-browser",
    "/usr/bin/google-chrome",
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
  ];

  for (const p of patterns) {
    if (p.includes("*")) {
      const hit = globSync(p);
      if (hit.length > 0) return hit.sort().at(-1);
    } else if (existsSync(p)) {
      return p;
    }
  }
  return null;
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/* ------------------------------------------------------------- the protocol */

class Cdp {
  #ws;
  #next = 1;
  #waiting = new Map();

  static async open(url) {
    const c = new Cdp();
    c.#ws = new WebSocket(url);
    await new Promise((ok, no) => {
      c.#ws.onopen = ok;
      c.#ws.onerror = () => no(new Error("cannot reach the browser"));
    });
    c.#ws.onmessage = (e) => {
      const m = JSON.parse(e.data);
      const w = c.#waiting.get(m.id);
      if (w !== undefined) {
        c.#waiting.delete(m.id);
        m.error ? w.no(new Error(JSON.stringify(m.error))) : w.ok(m.result);
      }
    };
    return c;
  }

  send(method, params = {}, sessionId) {
    const id = this.#next++;
    const msg = { id, method, params };
    if (sessionId !== undefined) msg.sessionId = sessionId;
    this.#ws.send(JSON.stringify(msg));
    return new Promise((ok, no) => this.#waiting.set(id, { ok, no }));
  }

  close() { this.#ws.close(); }
}

/* ---------------------------------------------------------------- the page */

async function main() {
  const browser = findBrowser();
  if (browser === null) {
    console.log("result=infra");
    console.log("no browser found; set AMINETXDUO_CHROME=<path>");
    return 2;
  }

  const profile = mkdtempSync(join(tmpdir(), "wsconsole-"));
  const proc = spawn(browser, [
    "--headless",
    "--disable-gpu",
    "--no-sandbox",
    "--no-first-run",
    "--remote-debugging-port=0",
    "--user-data-dir=" + profile,
    "about:blank",
  ], { stdio: ["ignore", "ignore", "pipe"] });

  let stderr = "";
  proc.stderr.on("data", (b) => { stderr += b.toString(); });

  /* The port is written into the profile once it is listening.  Polled, not
     slept for: a fixed sleep is either too short on a loaded machine or waste
     on an idle one. */
  let port = null;
  for (let i = 0; i < 100 && port === null; i++) {
    await sleep(100);
    try {
      port = readFileSync(join(profile, "DevToolsActivePort"), "utf8")
        .split("\n")[0].trim();
    } catch { /* not yet */ }
  }

  if (port === null) {
    proc.kill();
    rmSync(profile, { recursive: true, force: true });
    console.log("result=infra");
    console.log("the browser never listened: " + stderr.slice(0, 400));
    return 2;
  }

  const version = await (await fetch("http://127.0.0.1:" + port + "/json/version")).json();
  const cdp = await Cdp.open(version.webSocketDebuggerUrl);

  const { targetId } = await cdp.send("Target.createTarget", { url: "about:blank" });
  const { sessionId } = await cdp.send("Target.attachToTarget",
                                       { targetId, flatten: true });

  const evaluate = async (expression) => {
    const r = await cdp.send("Runtime.evaluate",
                             { expression, returnByValue: true }, sessionId);
    return r.result?.value;
  };

  /* What the terminal has on it.  xterm.js's DOM renderer puts the rows in
     the document, so this is the same text a person is looking at -- not the
     socket, not the buffer object, the screen. */
  const screen = () => evaluate(
    "(document.querySelector('.xterm-rows')||{}).innerText || ''");

  const focus = () => evaluate(
    "(document.querySelector('.xterm-helper-textarea')||{}).focus?.()");

  /* One character at a time, as key events, because that is what xterm.js
     listens for and what a person produces. */
  const type = async (text) => {
    for (const ch of text) {
      await cdp.send("Input.dispatchKeyEvent",
                     { type: "keyDown", text: ch, unmodifiedText: ch,
                       key: ch }, sessionId);
      await cdp.send("Input.dispatchKeyEvent", { type: "keyUp", key: ch },
                     sessionId);
      await sleep(12);
    }
  };

  const enter = async () => {
    for (const type_ of ["keyDown", "keyUp"]) {
      await cdp.send("Input.dispatchKeyEvent",
                     { type: type_, key: "Enter", code: "Enter",
                       windowsVirtualKeyCode: 13, nativeVirtualKeyCode: 13,
                       text: "\r", unmodifiedText: "\r" }, sessionId);
    }
    await sleep(50);
  };

  const until = async (want, seconds) => {
    const deadline = Date.now() + seconds * 1000;
    while (Date.now() < deadline) {
      const s = await screen();
      if (s.includes(want)) return s;
      await sleep(400);
    }
    return null;
  };

  let transcript = "";

  try {
    await cdp.send("Page.navigate", { url: URL_ }, sessionId);

    const prompt = await until(">", 40);
    check(prompt !== null, "the page connects and the Shell prompts");
    if (prompt === null) throw new Error("no prompt");

    await focus();

    /* THE CONTROL.  Cooked mode echoes, here, in the page -- so a command
       typed now must appear on the screen.  Everything after this is only
       meaningful because this held. */
    await type("Echo " + VISIBLE);
    await sleep(300);
    const typed = await screen();
    check(typed.includes(VISIBLE),
          "what is typed in cooked mode IS drawn (the reader works)");
    await enter();
    await until(VISIBLE, 20);

    /* And now the thing itself. */
    await type("stack 65536");
    await enter();
    await sleep(1500);

    await type("ssh -y -p " + SSHD + " " + USER + "@" + HOST_FROM_GUEST);
    await enter();

    const asked = await until("assword", 180);
    check(asked !== null, "ssh reaches a password prompt in the page");
    if (asked === null) throw new Error("no password prompt");

    /* Give the mode word the moment it needs: getpass() writes the prompt and
       then calls SetMode(), so the two are one frame apart. */
    await sleep(1500);

    const mode = await evaluate(
      "(document.getElementById('word')||{}).textContent || ''");

    await type(SECRET);
    await sleep(500);

    transcript = await screen();

    check(!transcript.includes(SECRET),
          "the password is NOT on the screen");
    check(transcript.includes("assword"),
          "and the prompt that asked for it still is");

    await enter();
    await sleep(1000);
    const after = await screen();
    check(!after.includes(SECRET),
          "and it is still not there after Return");

    console.log("state=" + JSON.stringify(mode));
    transcript = after;
  } catch (e) {
    check(false, "the page run stopped: " + e.message);
    try { transcript = await screen(); } catch { /* gone */ }
  } finally {
    cdp.close();
    proc.kill();
    rmSync(profile, { recursive: true, force: true });
  }

  console.log("");
  console.log("=================== the screen, as rendered ===================");
  console.log(transcript.replace(/\n{3,}/g, "\n\n").trimEnd());
  console.log("==============================================================");
  console.log("");
  console.log("secret=" + SECRET);
  console.log("secret_on_screen=" + (transcript.includes(SECRET) ? "yes" : "no"));
  console.log("checks=" + checks);
  console.log("failures=" + failures.length);
  for (const f of failures) console.log("  " + f);
  console.log("result=" + (failures.length === 0 ? "pass" : "fail"));

  return failures.length === 0 ? 0 : 1;
}

process.exit(await main());
