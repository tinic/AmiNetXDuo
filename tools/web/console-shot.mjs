/*
 * Load the built page in a real browser, read the canvas back, and say what
 * is on it.
 *
 *   node tools/web/console-shot.mjs URL [OUT.png] [--wait MS] [--input]
 *
 * A screenshot alone proves the page painted something; what this also does
 * is pull the framebuffer canvas back out through getImageData and count what
 * is in it, so "the viewer connected and drew the screen" is a number and not
 * an impression.  A blank canvas and a correct one are the same PNG to a
 * program and the same glance to a person in a hurry.
 *
 * Output is key=value and the exit code, so it can be read by something other
 * than a person.  Non-zero means a check failed, not that the browser did.
 *
 * Chrome is driven over CDP directly.  Node has had a WebSocket client since
 * 22 and the four commands needed here are four JSON messages; a devtools
 * driver would be a dependency for that.
 *
 * SPDX-License-Identifier: MIT
 */

import { spawn } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const args = process.argv.slice(2);
const url = args[0];
const out = args[1] && !args[1].startsWith("--") ? args[1] : null;
const waitMs = Number(pick("--wait") ?? 2500);
const wantInput = args.includes("--input");

if (!url) {
  console.error("usage: console-shot.mjs URL [OUT.png] [--wait MS] [--input]");
  process.exit(2);
}

function pick(flag) {
  const i = args.indexOf(flag);
  return i < 0 ? undefined : args[i + 1];
}

const CHROME = process.env.CHROME ||
  "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome";

/*
 * Read back the canvas the page decoded into.  Counting distinct colours and
 * the share that are not the background pen is what tells a drawn Workbench
 * from a cleared canvas; the palette bands the synthesiser puts along the
 * bottom are what makes the count meaningful.
 */
const PROBE = `(() => {
  const fb = document.getElementById("fb");
  const el = (id) => (document.getElementById(id) || {}).textContent || "";
  const r = {
    state: el("word").replace(/[= ]/g, "_"),
    geom: el("geom").replace(/[= ]/g, "_"),
    perf: el("perf").replace(/[= ]/g, "_"),
    log_lines: (el("log").match(/\\n/g) || []).length + 1,
    canvas_w: fb ? fb.width : 0,
    canvas_h: fb ? fb.height : 0,
    css_w: fb ? Math.round(fb.getBoundingClientRect().width) : 0,
    css_h: fb ? Math.round(fb.getBoundingClientRect().height) : 0,
    colours: 0,
    non_background: 0,
  };
  if (!fb || fb.width === 0) return r;
  const px = fb.getContext("2d").getImageData(0, 0, fb.width, fb.height).data;
  const seen = new Set();
  let other = 0;
  for (let i = 0; i < px.length; i += 4) {
    const v = (px[i] << 16) | (px[i + 1] << 8) | px[i + 2];
    seen.add(v);
    if (v !== 0xaaaaaa) other++;
  }
  r.colours = seen.size;
  r.non_background = Math.round((other * 1000) / (px.length / 4)) / 10;
  return r;
})()`;

const profile = mkdtempSync(join(tmpdir(), "console-shot-"));

const chrome = spawn(CHROME, [
  "--headless=new",
  "--disable-gpu",
  "--hide-scrollbars",
  "--no-first-run",
  "--no-default-browser-check",
  "--window-size=1200,900",
  "--remote-debugging-port=0",
  "--user-data-dir=" + profile,
  url,
], { stdio: ["ignore", "ignore", "ignore"] });

let code = 0;

try {
  const port = await waitFor(() => {
    /* Chrome writes the port it chose here once it is listening, which is the
       only way to know it with --remote-debugging-port=0.  Asking for a fixed
       port instead is how two of these running at once collide. */
    const s = readFileSync(join(profile, "DevToolsActivePort"), "utf8");
    return Number(s.split("\n")[0]);
  }, 15000, "chrome did not open a debugging port");

  const target = await waitFor(async () => {
    const list = await (await fetch("http://127.0.0.1:" + port + "/json/list")).json();
    const page = list.find((t) => t.type === "page" && t.webSocketDebuggerUrl);
    if (!page) throw new Error("no page target");
    return page;
  }, 15000, "chrome opened no page");

  const cdp = await connect(target.webSocketDebuggerUrl);

  await cdp.send("Page.enable");
  await cdp.send("Runtime.enable");
  await cdp.send("Page.navigate", { url });
  await sleep(waitMs);

  if (wantInput) {
    /* A mouse move and a click, so the input path is exercised by something
       other than a person: what the page does with them is visible in the
       log element and in the word count. */
    for (let i = 0; i < 12; i++) {
      await cdp.send("Input.dispatchMouseEvent", {
        type: "mouseMoved", x: 300 + i * 8, y: 300 + i * 4, buttons: 0,
      });
    }
    await cdp.send("Input.dispatchMouseEvent", {
      type: "mousePressed", x: 380, y: 340, button: "left", buttons: 1,
      clickCount: 1,
    });
    await cdp.send("Input.dispatchMouseEvent", {
      type: "mouseReleased", x: 380, y: 340, button: "left", buttons: 0,
      clickCount: 1,
    });
    await sleep(300);
  }

  const probe = await cdp.send("Runtime.evaluate", {
    expression: PROBE,
    returnByValue: true,
  });

  if (probe.exceptionDetails) {
    throw new Error("the probe threw: " +
                    JSON.stringify(probe.exceptionDetails.exception));
  }

  const r = probe.result.value;
  for (const [k, v] of Object.entries(r)) console.log(k + "=" + v);

  if (out !== null) {
    const shot = await cdp.send("Page.captureScreenshot", { format: "png" });
    writeFileSync(out, Buffer.from(shot.data, "base64"));
    console.log("screenshot=" + out);
  }

  /* What makes this a check rather than a report: a canvas that is one colour
     is a canvas nothing decoded into. */
  if (r.canvas_w === 0) { console.log("fail=no canvas"); code = 1; }
  else if (r.colours < 4) { console.log("fail=canvas has " + r.colours + " colours"); code = 1; }

  cdp.close();
} catch (e) {
  console.log("fail=" + (e instanceof Error ? e.message : String(e)));
  code = 1;
} finally {
  chrome.kill("SIGKILL");
  rmSync(profile, { recursive: true, force: true });
}

process.exit(code);

/* ---------------------------------------------------------------- bits -- */

function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

async function waitFor(fn, ms, why) {
  const until = Date.now() + ms;
  for (;;) {
    try { return await fn(); } catch { /* not yet */ }
    if (Date.now() > until) throw new Error(why);
    await sleep(100);
  }
}

function connect(wsUrl) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(wsUrl);
    const waiting = new Map();
    let id = 0;

    ws.onopen = () => resolve({
      send(method, params) {
        const n = ++id;
        ws.send(JSON.stringify({ id: n, method, params: params || {} }));
        return new Promise((ok, no) => waiting.set(n, { ok, no }));
      },
      close() { ws.close(); },
    });

    ws.onmessage = (e) => {
      const m = JSON.parse(e.data);
      const w = waiting.get(m.id);
      if (w === undefined) return;
      waiting.delete(m.id);
      if (m.error) w.no(new Error(m.error.message));
      else w.ok(m.result);
    };

    ws.onerror = () => reject(new Error("could not reach chrome over CDP"));
  });
}
