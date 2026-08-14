/*
 * A viewer for an AmigaOS Workbench screen, with a capture player in front of
 * it.
 *
 * The player came first and is not a debugging aid that got left in.  The
 * planar unpack, the palette, the aspect correction and the scaling are the
 * parts most likely to be subtly wrong, and every one of them can be got
 * right against a file with no Amiga, no emulator and no server in the loop
 * -- which is a two-second edit-reload cycle instead of a two-minute one.
 * When the live half then draws something wrong, the same page opens the
 * capture beside it.
 *
 * Both halves share everything below the transport: one View, one decoder,
 * one palette path.  The only difference is where the planar bytes came from,
 * which is the property that makes the player evidence about the live viewer
 * rather than a second implementation that agrees with it by luck.
 *
 * The live half's tile format is a PLACEHOLDER and says so on the page as
 * well as here; see tiles.ts, which is the one file that has to change when
 * the encoder lands.
 *
 * SPDX-License-Identifier: MIT
 */

import { attachInput } from "./input";
import { frameAt, parsePfs, type Capture } from "./pfs";
import { frameBytes, palette32, pixelAspect } from "./planar";
import {
  applyUpdate,
  geometryFromWord,
  paletteFromWord,
  scratchBytes,
  type Geometry,
} from "./tiles";
import { View, type Aspect, type Zoom } from "./view";
import { Wire, defaultEndpoint, type WireState } from "./wire";

const $ = (id: string) => document.getElementById(id)!;

const view = new View($("box"));

/* --------------------------------------------------------------- the bar -- */

const srcEl = $("src");
const stateEl = $("state");
const wordEl = $("word");
const geomEl = $("geom");
const perfEl = $("perf");
const palEl = $("pal");
const countEl = $("count");

function say(kind: "" | "up" | "down", text: string): void {
  stateEl.className = kind;
  wordEl.textContent = text;
}

/* A ring, not a growing list: a session left open logs a word per mouse move
   and the interesting one is always the last. */
const LOG_LINES = 200;
const logEl = $("log");
const lines: string[] = [];

function log(s: string): void {
  lines.push(s);
  if (lines.length > LOG_LINES) lines.splice(0, lines.length - LOG_LINES);
  logEl.textContent = lines.join("\n");
  logEl.scrollTop = logEl.scrollHeight;
}

function showGeometry(): void {
  const s = view.geometry;
  if (s.width === 0) { geomEl.textContent = "-"; return; }
  const a = pixelAspect(s);
  geomEl.textContent =
    s.width + "x" + s.height + "x" + s.depth +
    "  " + (1 << s.depth) + " colours" +
    "  bpr " + s.bytesPerRow +
    "  " + frameBytes(s) + " B/frame" +
    "  displayed " + s.width * a.x + "x" + s.height * a.y;
}

function showPalette(rgb: Uint8Array, depth: number): void {
  palEl.replaceChildren();
  for (let i = 0; i < 1 << depth; i++) {
    const sw = document.createElement("i");
    sw.style.background =
      "rgb(" + rgb[i * 3] + "," + rgb[i * 3 + 1] + "," + rgb[i * 3 + 2] + ")";
    sw.title = i + ": " +
      [rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]]
        .map((v) => v.toString(16).padStart(2, "0")).join("");
    palEl.append(sw);
  }
}

/* ------------------------------------------------------------ the player -- */

let cap: Capture | null = null;
let frame = 0;
let playing = false;
let fps = 25;
let lastTick = 0;
let raf = 0;

const playEl = $("play") as HTMLButtonElement;
const prevEl = $("prev") as HTMLButtonElement;
const nextEl = $("next") as HTMLButtonElement;
const scrubEl = $("scrub") as HTMLInputElement;
const fpsEl = $("fps") as HTMLInputElement;

function showFrame(i: number): void {
  if (cap === null) return;
  frame = ((i % cap.frameCount) + cap.frameCount) % cap.frameCount;
  view.paint(cap.frames, frameAt(cap, frame));
  scrubEl.value = String(frame);
  countEl.textContent = (frame + 1) + " / " + cap.frameCount;
  perfEl.textContent = "decode " + view.lastDecodeMs.toFixed(2) + " ms";
}

/*
 * rAF with an accumulator rather than setInterval.  A capture has a rate and
 * the display has a rate and they are not the same number: stepping on the
 * display's clock and dropping or repeating a frame when the two disagree is
 * what keeps 25 fps looking like 25 fps on a 120 Hz panel.
 */
function tick(now: number): void {
  raf = 0;
  if (!playing || cap === null) return;

  if (now - lastTick >= 1000 / fps) {
    lastTick = now;
    showFrame(frame + 1);
  }
  raf = requestAnimationFrame(tick);
}

function setPlaying(on: boolean): void {
  playing = on && cap !== null;
  playEl.textContent = playing ? "Pause" : "Play";
  if (playing && raf === 0) {
    lastTick = 0;
    raf = requestAnimationFrame(tick);
  }
}

function loadCapture(name: string, buf: ArrayBuffer): void {
  let c: Capture;
  try {
    c = parsePfs(buf);
  } catch (e) {
    say("down", "not loaded: " + (e instanceof Error ? e.message : String(e)));
    return;
  }

  live.disconnect();
  cap = c;
  frame = 0;

  view.setScreen(c.screen, c.palette);
  showGeometry();
  showPalette(c.rgb, c.screen.depth);

  scrubEl.max = String(c.frameCount - 1);
  scrubEl.disabled = c.frameCount < 2;
  playEl.disabled = c.frameCount < 2;
  prevEl.disabled = c.frameCount < 2;
  nextEl.disabled = c.frameCount < 2;

  srcEl.textContent = name;
  say("up", "capture, " + c.frameCount + " frames");
  showFrame(0);
  setPlaying(c.frameCount > 1);
}

playEl.onclick = () => setPlaying(!playing);
prevEl.onclick = () => { setPlaying(false); showFrame(frame - 1); };
nextEl.onclick = () => { setPlaying(false); showFrame(frame + 1); };
scrubEl.oninput = () => { setPlaying(false); showFrame(Number(scrubEl.value)); };
fpsEl.oninput = () => {
  const v = Number(fpsEl.value);
  if (v >= 1 && v <= 60) fps = v;
};

/* ------------------------------------------------------------ the source -- */

const fileEl = $("file") as HTMLInputElement;

($("open") as HTMLButtonElement).onclick = () => fileEl.click();

fileEl.onchange = () => {
  const f = fileEl.files && fileEl.files[0];
  if (f) void take(f);
};

async function take(f: File): Promise<void> {
  say("", "reading " + f.name);
  loadCapture(f.name, await f.arrayBuffer());
}

/* Drop anywhere.  A drop target the size of a strip is a target people miss,
   and there is nothing else on this page a file could mean. */
addEventListener("dragover", (e) => {
  e.preventDefault();
  document.body.classList.add("drop");
});
addEventListener("dragleave", () => document.body.classList.remove("drop"));
addEventListener("drop", (e) => {
  e.preventDefault();
  document.body.classList.remove("drop");
  const f = e.dataTransfer && e.dataTransfer.files[0];
  if (f) void take(f);
});

/* -------------------------------------------------------------- the wire -- */

let geom: Geometry | null = null;
let planes: Uint8Array | null = null;
let scratch = new Uint8Array(0);
let expectSeq = -1;

/* Frames arrive before the palette does, so there is one to start with: grey
   is what an Amiga screen is before LoadRGB4 runs, and a black one reads as
   the viewer having failed. */
function greyPalette(depth: number): Uint8Array {
  const n = 1 << depth;
  const rgb = new Uint8Array(n * 3);
  for (let i = 0; i < n; i++) {
    const v = Math.round((i * 255) / Math.max(1, n - 1));
    rgb[i * 3] = v; rgb[i * 3 + 1] = v; rgb[i * 3 + 2] = v;
  }
  return rgb;
}

let inBytes = 0;
let inFrames = 0;
let statAt = 0;

const live = new Wire({
  onWord: (w) => heard(w),
  onFrame: (buf) => update(buf),
  onState: (state, detail) => connection(state, detail),
});

/*
 * A word this does not know is ignored and logged.  Same rule as the Shell's
 * page: it is what lets either end learn a word without the other having to
 * be rebuilt on the same afternoon.
 */
function heard(w: string): void {
  log("< " + (w.length > 96 ? w.slice(0, 93) + "..." : w));

  try {
    if (w.startsWith("geom ")) {
      const g = geometryFromWord(w);
      geom = g;
      planes = new Uint8Array(frameBytes(g.screen));
      scratch = new Uint8Array(scratchBytes(g));
      expectSeq = -1;
      cap = null;
      const rgb = greyPalette(g.screen.depth);
      view.setScreen(g.screen, palette32(rgb, g.screen.depth));
      showGeometry();
      showPalette(rgb, g.screen.depth);
      live.word("refresh");
      return;
    }

    if (w.startsWith("pal ")) {
      if (geom === null) return;
      const rgb = paletteFromWord(w, geom.screen.depth);
      view.setPalette(palette32(rgb, geom.screen.depth));
      showPalette(rgb, geom.screen.depth);
      if (planes !== null) view.paint(planes, 0);
      return;
    }
  } catch (e) {
    say("down", e instanceof Error ? e.message : String(e));
  }
}

function update(buf: ArrayBuffer): void {
  if (geom === null || planes === null) return;

  const t0 = performance.now();
  let d;
  try {
    d = applyUpdate(geom, new Uint8Array(buf), planes, scratch);
  } catch (e) {
    /* A malformed update is not survivable by carrying on: the XOR chain is
       now wrong for every tile after it.  Ask for a keyframe and say so. */
    say("down", e instanceof Error ? e.message : String(e));
    live.word("refresh");
    expectSeq = -1;
    return;
  }

  /*
   * A gap in the sequence means an update was lost, and every XOR after it
   * is applied to bytes that are not what the encoder thought they were.
   * There is nothing to reconstruct from, so the only correct move is to ask
   * for the screen again.
   */
  if (expectSeq >= 0 && d.seq !== expectSeq && !d.keyframe) {
    log("! seq " + d.seq + ", expected " + expectSeq + " -- asking for a keyframe");
    live.word("refresh");
  }
  expectSeq = (d.seq + 1) & 0xffff;

  view.paint(planes, 0, d);

  inBytes += d.bytes;
  inFrames++;

  const now = performance.now();
  if (now - statAt >= 500) {
    const secs = (now - statAt) / 1000;
    perfEl.textContent =
      inFrames === 0 ? "" :
      (inFrames / secs).toFixed(1) + " fps  " +
      (inBytes / secs / 1024).toFixed(1) + " KB/s  " +
      "apply " + (now - t0).toFixed(2) + " ms  " +
      "decode " + view.lastDecodeMs.toFixed(2) + " ms";
    statAt = now;
    inBytes = 0;
    inFrames = 0;
  }
}

const urlEl = $("url") as HTMLInputElement;
const liveEl = $("live") as HTMLButtonElement;

const WORDS: Record<WireState, string> = {
  idle: "idle",
  connecting: "connecting",
  open: "live",
  closed: "closed",
  refused: "refused",
};

function connection(state: WireState, detail: string): void {
  const up = state === "open";
  say(up ? "up" : state === "connecting" ? "" : "down",
      detail && !up ? WORDS[state] + ": " + detail : WORDS[state]);

  liveEl.textContent = up || state === "connecting" ? "Disconnect" : "Connect";
  if (up) {
    srcEl.textContent = urlEl.value;
    /* Cleared here rather than on the geom word: a reconnect that never gets
       one should not be showing the last session's statistics. */
    inBytes = 0;
    inFrames = 0;
    statAt = performance.now();
  }
  if (state === "closed" || state === "refused") expectSeq = -1;
}

liveEl.onclick = () => {
  if (live.open) { live.disconnect(); return; }
  setPlaying(false);
  live.connect(urlEl.value);
};

urlEl.value = defaultEndpoint();

/* ------------------------------------------------------------- the input -- */

const inputs = attachInput(view, $("box"), {
  send: (w) => live.word(w),
  log: (w) => log((live.open ? "> " : "· ") + w),
});

/*
 * The pointer word rate, which is the number the coalescing exists to hold
 * down.  Shown next to the input log because that is where it is evidence:
 * moves is what the mouse produced and the log is what was sent.
 */
setInterval(() => {
  const el = $("count");
  if (cap === null && geom !== null) {
    el.textContent = inputs.moves() + " moves  " +
                     live.wordsOut + " words out";
  }
}, 500);

/* -------------------------------------------------------------- the view -- */

const aspectEl = $("aspect") as HTMLSelectElement;
const zoomEl = $("zoom") as HTMLSelectElement;

aspectEl.onchange = () => view.setAspect(aspectEl.value as Aspect);
zoomEl.onchange = () => {
  const v = zoomEl.value;
  view.setZoom((v === "fit" ? "fit" : Number(v)) as Zoom);
};

addEventListener("keydown", (e) => {
  if (document.activeElement === $("box")) return;   /* typing at the Amiga */
  if (e.key === " ") { e.preventDefault(); setPlaying(!playing); }
  else if (e.key === "ArrowLeft") { setPlaying(false); showFrame(frame - 1); }
  else if (e.key === "ArrowRight") { setPlaying(false); showFrame(frame + 1); }
});

/* -------------------------------------------------------------- the start -- */

say("", "idle -- open a .pfs capture, or connect");
log("tile updates use the PLACEHOLDER framing in tiles.ts; the encoder's");
log("real one is not written yet (branch proto/rfb-encoder)");
