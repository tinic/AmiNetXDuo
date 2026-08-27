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
 * The live half's wire format is the frame encoder's, and tiles.ts is the
 * only file that reads a byte of it -- which is what let the rest of this be
 * built and measured against a placeholder framing before the encoder
 * existed, and then swapped for the real one without touching anything else.
 *
 * SPDX-License-Identifier: MIT
 */

import { Apng } from "./apng";
import { attachInput } from "./input";
import {
  PFS_MAX_FRAMES,
  buildPfs,
  frameAt,
  parsePfs,
  pfsDuration,
  type Capture,
  type PointerAt,
  type Pointer,
} from "./pfs";
import {
  FMT_RGB565,
  frameBytes,
  palColours,
  pixelAspect,
  renderPalette,
  screenColours,
  screenFormat,
  type Screen,
} from "./planar";
import {
  applyUpdate,
  geometryFromWord,
  paletteFromWord,
  scratchBytes,
  type Geometry,
} from "./tiles";
import { defaultPointer, pointerImage } from "./pointer";
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
  /* Colours the screen can show, which is not always how many arrive in the
     `pal` word: a HAM screen sends sixteen or sixty-four base entries and puts
     up thousands.  screenColours() is the one that answers the question a
     person is asking. */
  geomEl.textContent =
    s.width + "x" + s.height + "x" + s.depth +
    "  " + screenColours(s) + " colours" +
    "  bpr " + s.bytesPerRow +
    "  " + frameBytes(s) + " B/frame" +
    "  displayed " + s.width * a.x + "x" + s.height * a.y;
}

/* ------------------------------------------------------------ the player -- */

let cap: Capture | null = null;
let frame = 0;
let playing = false;
let raf = 0;

/*
 * WHERE THE PLAYHEAD IS, IN THE RECORDING'S OWN MILLISECONDS.
 *
 * `startedAt` is the document clock reading that corresponds to `startedFrom`
 * on the file's timeline, so the playhead is (now - startedAt) + startedFrom.
 * Re-anchored on every play, pause and scrub, which is what keeps a seek from
 * making the next frame jump.
 */
let startedAt = 0;
let startedFrom = 0;

/* Which pointer image the player last expanded, so a scrub does not rebuild
   it on every frame.  Reset when a capture is loaded. */
let shownImage = -1;

const playEl = $("play") as HTMLButtonElement;
const prevEl = $("prev") as HTMLButtonElement;
const nextEl = $("next") as HTMLButtonElement;
const scrubEl = $("scrub") as HTMLInputElement;

/* Seconds and a decimal, which is what a duration under a minute reads as
   and what a scrub position has to be compared against. */
function secs(ms: number): string {
  return (ms / 1000).toFixed(1) + "s";
}

function showFrame(i: number): void {
  if (cap === null) return;
  frame = ((i % cap.frameCount) + cap.frameCount) % cap.frameCount;
  view.paint(cap.frames, frameAt(cap, frame));

  /*
   * The pointer where the recording says it was.  A capture written before
   * anything tracked it has image 0 at 0,0 on every frame, which is what the
   * `image === 0 && x === 0 && y === 0` test reads as "nothing recorded" --
   * a pointer genuinely parked in the top left corner for a whole recording
   * is the one case this gets wrong, and drawing nothing there is better than
   * drawing an arrow on a file that never had one.
   */
  {
    const p = cap.pointerAt[frame];
    if (p === undefined || (p.image === 0 && p.x === 0 && p.y === 0)) {
      view.clearPointer();
    } else {
      /* The image the file names, expanded once per capture and not per
         frame: a recording carries one pointer and names it on every frame. */
      if (p.image !== shownImage) {
        shownImage = p.image;
        const q = cap.pointers[p.image - 1];
        view.setPointer(q === undefined
          ? defaultPointer()
          : pointerImage(q.width, q.height, q.depth, q.xScale, q.yScale,
                         q.hotX, q.hotY, q.rgb, q.bits));
      }
      view.movePointer(p.x, p.y);
    }
  }

  scrubEl.value = String(frame);
  countEl.textContent = (frame + 1) + " / " + cap.frameCount + "  " +
                        secs(cap.times[frame]) + " / " + secs(pfsDuration(cap));
  perfEl.textContent = "decode " + view.lastDecodeMs.toFixed(2) + " ms";
}

/* Put the playhead at a frame and anchor the clock to it, so playing on from
   a scrub or a step carries on from where the picture actually is. */
function seek(i: number): void {
  if (cap === null) return;
  showFrame(i);
  startedFrom = cap.times[frame];
  startedAt = performance.now();
}

/*
 * rAF against the RECORDING'S clock, not the display's and not a rate.
 *
 * A live capture has no frame rate: an idle screen produces a frame every few
 * seconds and a window opening produces a burst, so the file carries the
 * milliseconds each frame was taken at and this walks that timeline in real
 * time.  The accumulator idea survives the change -- the display's rate and
 * the recording's still do not line up -- but what it does now is HOLD a frame
 * whose successor is not due yet and SKIP past any that fell behind, rather
 * than step one frame per interval.
 */
function tick(now: number): void {
  raf = 0;
  if (!playing || cap === null) return;

  const at = startedFrom + (now - startedAt);

  if (at >= pfsDuration(cap)) {
    /* Round again from the top, on the same clock. */
    showFrame(0);
    startedFrom = 0;
    startedAt = now;
  } else {
    let next = frame;
    while (next + 1 < cap.frameCount && cap.times[next + 1] <= at) next++;
    if (next !== frame) showFrame(next);
  }

  raf = requestAnimationFrame(tick);
}

function setPlaying(on: boolean): void {
  playing = on && cap !== null;
  playEl.textContent = playing ? "Pause" : "Play";
  if (playing && cap !== null) {
    /* From wherever the picture is, and from the top if it is at the end. */
    startedFrom = frame + 1 >= cap.frameCount ? 0 : cap.times[frame];
    if (startedFrom === 0 && frame + 1 >= cap.frameCount) showFrame(0);
    startedAt = performance.now();
    if (raf === 0) raf = requestAnimationFrame(tick);
  }
}

/*
 * The transport gadgets belong to a capture.  A live session has no frame to
 * step to and no end to scrub towards, so they are dead there rather than
 * merely useless: leaving them lit invites a click that cannot do anything.
 * Record is not one of them -- it is the gadget that works only when live.
 */
function setTransport(on: boolean): void {
  playEl.disabled = !on;
  prevEl.disabled = !on;
  nextEl.disabled = !on;
  scrubEl.disabled = !on;
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
  shownImage = -1;

  view.setScreen(c.screen, c.palette);
  showGeometry();

  scrubEl.max = String(c.frameCount - 1);
  setTransport(c.frameCount >= 2);
  /* Not one of the transport gadgets: a one-frame capture has nothing to step
     to and still exports, as an APNG of one frame. */
  expEl.disabled = false;

  srcEl.textContent = name;
  say("up", "capture, " + c.frameCount + " frames over " +
             secs(pfsDuration(c)));
  seek(0);
  setPlaying(c.frameCount > 1);
}

playEl.onclick = () => setPlaying(!playing);
prevEl.onclick = () => { setPlaying(false); seek(frame - 1); };
nextEl.onclick = () => { setPlaying(false); seek(frame + 1); };
scrubEl.oninput = () => { setPlaying(false); seek(Number(scrubEl.value)); };

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

/* The live palette in RGB8.  The View keeps the 32-bit form it paints with;
   a .pfs header wants the bytes that came off the wire, and the recorder is
   the only thing that asks for them. */
let liveRgb: Uint8Array = new Uint8Array(0);

/* Two hex digits a byte, which is how both `pal` and `ptr` carry theirs. */
function hexBytes(hex: string): Uint8Array {
  const out = new Uint8Array(hex.length >> 1);
  for (let i = 0; i < out.length; i++) {
    out[i] = parseInt(hex.substr(i * 2, 2), 16);
  }
  return out;
}

/* What the Amiga said its pointer is, in SPRITE pixels and unscaled, or null
   before it has said.  Kept in that form because it is what a recording
   stores: the file carries the truth and the scale, not the expansion. */
let livePointer: {
  w: number; h: number; depth: number; xScale: number; yScale: number;
  hotX: number; hotY: number; rgb: Uint8Array; bits: Uint8Array;
} | null = null;

/* Frames arrive before the palette does, so there is one to start with: grey
   is what an Amiga screen is before LoadRGB4 runs, and a black one reads as
   the viewer having failed.  `colours` is entries, so a format that has none
   asks for none and gets an empty one. */
function greyPalette(colours: number): Uint8Array {
  const n = colours;
  const rgb = new Uint8Array(n * 3);
  for (let i = 0; i < n; i++) {
    const v = Math.round((i * 255) / Math.max(1, n - 1));
    rgb[i * 3] = v; rgb[i * 3 + 1] = v; rgb[i * 3 + 2] = v;
  }
  return rgb;
}

let inBytes = 0;
let inFrames = 0;
let inMs = 0;
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
      setPlaying(false);
      setTransport(false);
      expEl.disabled = true;
      /*
       * NOTHING TO WAIT FOR ON A TRUECOLOUR SCREEN.  A 16-bit pixel carries
       * its own colour, so no `pal` follows the geom and none ever will; a
       * viewer that sat on a placeholder until one arrived would sit there
       * for the whole session.  palColours() says none, which makes both of
       * these empty and neither of them read -- 1 << 16 grey entries would
       * be a palette nothing ever looks at.
       */
      const rgb = greyPalette(palColours(g.screen));
      liveRgb = rgb;
      view.setScreen(g.screen, renderPalette(g.screen, rgb));
      showGeometry();
      /* Before the pal that follows, so the file that closes here keeps the
         palette its frames were drawn with. */
      recRoll();
      /*
       * NO `refresh` HERE.  A geom already means both sides are at zero: the
       * server clears its shadow whenever it queues one -- at the start of a
       * session and again whenever it re-takes its buffers -- and the planes
       * above have just been reallocated.  Asking as well made the server
       * clear a shadow that was ALREADY clear and send a SECOND full frame,
       * and because the tiles are XOR against the shadow the viewer XORed the
       * picture it had just drawn back to zero.  On a reconnect, where the
       * server is already producing before the word can reach it, that landed
       * one frame after the desktop appeared and stayed: an idle Workbench
       * has nothing more to send.
       */
      return;
    }

    /*
     * The Amiga's own pointer.  It is a hardware sprite, so it is in no frame
     * this ever receives and has to come as its own word; the POSITION does
     * not, because this viewer is what puts the Amiga's pointer where the
     * browser's is and therefore already knows where it is.
     */
    if (w.startsWith("ptr ")) {
      const f = w.split(" ");
      if (f.length !== 10) return;

      const n = f.slice(1, 8).map(Number);
      if (n.some((v) => !Number.isFinite(v))) return;
      const [pw, ph, pd, xs, ys, hx, hy] = n;
      if (pw < 1 || ph < 1 || pd < 1 || pd > 8 || xs < 1 || ys < 1) return;

      const rgb = hexBytes(f[8]);
      const bits = hexBytes(f[9]);
      if (rgb.length !== 3 * ((1 << pd) - 1)) return;
      if (bits.length !== pd * (Math.floor((pw + 15) / 16) * 2) * ph) return;

      livePointer = { w: pw, h: ph, depth: pd, xScale: xs, yScale: ys,
                      hotX: hx, hotY: hy, rgb, bits };
      view.setPointer(pointerImage(pw, ph, pd, xs, ys, hx, hy, rgb, bits));
      return;
    }

    if (w.startsWith("pal ")) {
      if (geom === null) return;
      /* A truecolour screen has none, so one here is a server that has got
         the format wrong -- and reading it at depth 16 would ask for 393216
         hex digits.  Dropped rather than thrown: the picture is correct
         without it and the word is already in the log. */
      if (screenFormat(geom.screen) === FMT_RGB565) return;
      const rgb = paletteFromWord(w, palColours(geom.screen));
      liveRgb = rgb;
      view.setPalette(renderPalette(geom.screen, rgb));
      if (planes !== null) view.paint(planes, 0);
      recRoll();
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
    /* A malformed frame is not survivable by carrying on: what is in the
       buffer is now half a frame and every delta after it builds on that. */
    say("down", e instanceof Error ? e.message : String(e));
    planes.fill(0);
    live.word("refresh");
    expectSeq = -1;
    return;
  }

  /*
   * A gap in the sequence means a frame was lost, and every XOR and every
   * copy after it is applied to bytes that are not what the encoder thought
   * were there.  There is nothing to reconstruct from, so the only correct
   * move is to ask for the screen again -- `refresh` makes the far side
   * forget its shadow, which is what turns the next frame into a full one.
   */
  if (expectSeq >= 0 && d.seq !== expectSeq) {
    log("! seq " + d.seq + ", expected " + expectSeq + " -- asking for a resend");
    live.word("refresh");
    planes.fill(0);
  }
  expectSeq = (d.seq + 1) & 0xffff;

  view.paint(planes, 0, d);
  recTake(planes);

  inBytes += d.bytes;
  inFrames++;

  const now = performance.now();
  inMs += now - t0;

  /*
   * Milliseconds a second, not milliseconds a frame.
   *
   * A browser clamps performance.now() to 100 microseconds for a page that
   * is not cross-origin isolated, and one tile update is under that -- shown
   * per frame it reads 0.00 ms, which is a number nobody can do anything
   * with.  Accumulated over half a second it is the share of a core the
   * viewer is costing, which is the question actually being asked.
   */
  if (now - statAt >= 500) {
    const secs = (now - statAt) / 1000;
    perfEl.textContent =
      inFrames === 0 ? "" :
      (inFrames / secs).toFixed(1) + " fps  " +
      (inBytes / secs / 1024).toFixed(1) + " KB/s  " +
      (inMs / secs).toFixed(1) + " ms/s in decode";
    statAt = now;
    inBytes = 0;
    inFrames = 0;
    inMs = 0;
  }
}

const urlEl = $("url") as HTMLInputElement;
const liveEl = $("live") as HTMLButtonElement;
const shotEl = $("shot") as HTMLButtonElement;
const expEl = $("export") as HTMLButtonElement;

/*
 * There is one Workbench screen and one input.device, so the server takes one
 * viewer and refuses the rest; the HTTP status behind a refused upgrade is not
 * exposed to script (see socket.ts), so the ONE refusal a correct client can
 * provoke here is named rather than left as the bare word.  A server that is
 * not there does not refuse -- it fails to connect, and that is the
 * "connecting" that never resolves.
 */
const WORDS: Record<WireState, string> = {
  idle: "idle",
  connecting: "connecting",
  open: "live",
  closed: "closed",
  refused: "refused -- somebody else has the screen",
};

let wireState: WireState = "idle";

/*
 * A REBOOT IS NOT A FAULT, AND THE STATES IT PASSES THROUGH LOOK LIKE ONE
 *
 * `reset` is answered with a close frame and then the machine goes, so the
 * socket closes, every attempt to reopen it fails while the Amiga is in
 * Kickstart, and the browser reports each of those as a socket that never
 * opened -- which is the same thing it reports when somebody else has the
 * screen.  So the session is marked as rebooting before the word goes out and
 * every close is read against that: the page says "rebooting" and keeps
 * asking until httpd answers again, instead of saying it was refused and
 * stopping.
 */
let rebooting = false;
let rebootPoll = 0;

function connection(state: WireState, detail: string): void {
  const up = state === "open";
  wireState = state;

  if (rebooting && !up) {
    say("", "rebooting -- waiting for the Amiga to come back");
    liveEl.textContent = "Disconnect";
    if (rebootPoll === 0) {
      rebootPoll = setInterval(() => {
        if (!rebooting) return;
        if (!live.open && wireState !== "connecting") live.connect(urlEl.value);
      }, 2000) as unknown as number;
    }
    return;
  }

  if (rebooting && up) {
    rebooting = false;
    clearInterval(rebootPoll);
    rebootPoll = 0;
  }

  say(up ? "up" : state === "connecting" ? "" : "down",
      detail && !up ? WORDS[state] + ": " + detail : WORDS[state]);

  liveEl.textContent = up || state === "connecting" ? "Disconnect" : "Connect";

  /* Record takes what arrives on the wire, so it is dead without a session --
     the mirror of the transport gadgets, which are dead with one. */
  recEl.disabled = !up;
  if (!up && rec !== null) recStop();

  if (up) {
    srcEl.textContent = urlEl.value;
    /* Cleared here rather than on the geom word: a reconnect that never gets
       one should not be showing the last session's statistics. */
    inBytes = 0;
    inFrames = 0;
    inMs = 0;
    statAt = performance.now();
  }
  if (state === "closed" || state === "refused") expectSeq = -1;
}

/*
 * THE SCREENSHOT.
 *
 * Two digits of month and day and a 24-hour clock, and the geometry, so a
 * folder of these sorts into the order they were taken and says what each one
 * is without opening it.  `native` is in the name too, because a 640x256 file
 * and a 640x512 file of the same screen are both correct and telling them
 * apart afterwards is otherwise guesswork.
 */
function stamp(): string {
  const d = new Date();
  const p = (n: number, w = 2) => String(n).padStart(w, "0");
  return d.getFullYear() + p(d.getMonth() + 1) + p(d.getDate()) + "-" +
         p(d.getHours()) + p(d.getMinutes()) + p(d.getSeconds());
}

/* The object URL is the only thing here that would otherwise be held for the
   life of the page, one per file saved. */
function download(blob: Blob, name: string): void {
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(url), 10000);
}

function saveShot(native: boolean): void {
  const shot = view.snapshot(native);
  if (shot === null) { say("down", "there is no screen to save yet"); return; }

  shot.canvas.toBlob((blob) => {
    if (blob === null) { say("down", "the screenshot would not encode"); return; }
    const name = "amiga-" + shot.w + "x" + shot.h +
                 (native ? "-native" : "") + "-" + stamp() + ".png";
    download(blob, name);
    say("up", "saved " + name);
  }, "image/png");
}

shotEl.onclick = (e: MouseEvent) => saveShot(e.shiftKey);

/*
 * THE EXPORT.
 *
 * The same recording, in a file something else can open.  A .pfs is what the
 * capture side wrote and is the right thing to keep; an APNG is the copy that
 * can be attached to a bug report, and apng.ts says why it is that and not an
 * MP4.  Both, not one instead of the other -- the recorder is unchanged.
 *
 * IT WALKS THE PLAYER AND NOT THE FILE.  Each frame is shown and then
 * composed, which is the same pair of calls a screenshot makes, so the export
 * has the palette, the aspect and the pointer that were on the screen and
 * cannot drift from what was being watched.  It also means the picture on the
 * page is the progress bar.
 *
 * The transport is dead while it runs: a Play during the walk would be a
 * second thing calling showFrame(), and the frames would come out interleaved.
 */
let exporting = false;

async function saveExport(native: boolean): Promise<void> {
  const c = cap;
  if (c === null) { say("down", "there is no capture to export"); return; }
  if (exporting) return;

  exporting = true;
  expEl.disabled = true;
  setPlaying(false);
  setTransport(false);

  const was = frame;

  try {
    showFrame(0);
    const first = view.compose(native);
    if (first === null) throw new Error("there is no screen to export");

    const png = new Apng(first.w, first.h);

    /* The last frame has no successor to be timed against.  The gap before it
       is the only honest guess, and a single-frame capture gets a tenth of a
       second so that the file is not one with a zero delay in it. */
    const tail = c.frameCount >= 2
      ? c.times[c.frameCount - 1] - c.times[c.frameCount - 2] : 100;

    for (let i = 0; i < c.frameCount; i++) {
      if (i > 0) showFrame(i);
      const f = i === 0 ? first : view.compose(native);
      if (f === null) throw new Error("the screen went away mid-export");
      await png.add(f.rgba,
                    i + 1 < c.frameCount ? c.times[i + 1] - c.times[i] : tail);
      if ((i & 15) === 0) {
        say("", "exporting " + (i + 1) + " / " + c.frameCount);
      }
    }

    const out = png.finish();
    const name = "amiga-" + first.w + "x" + first.h +
                 (native ? "-native" : "") + "-" + stamp() + ".png";
    download(new Blob([out], { type: "image/apng" }), name);
    say("up", "saved " + name + ", " + png.frames + " frames, " +
              (out.length / 1024).toFixed(0) + " KB");
  } catch (e) {
    say("down", "the export would not write: " +
                (e instanceof Error ? e.message : String(e)));
  } finally {
    exporting = false;
    expEl.disabled = cap === null;
    setTransport(cap !== null && cap.frameCount >= 2);
    seek(was);
  }
}

expEl.onclick = (e: MouseEvent) => void saveExport(e.shiftKey);

/* ---------------------------------------------------------- the recorder -- */

/*
 * THE LIVE SESSION INTO A .pfs THE PLAYER ON THIS PAGE CAN OPEN
 *
 * The planar frames, as the decoder wrote them and the canvas was painted
 * from them -- not a re-planarisation of the RGB, which would be a second
 * encoder to be wrong in a second way.
 *
 * ONE FILE HOLDS ONE GEOMETRY AND ONE PALETTE, so a `geom` or a `pal` ends
 * the file and starts another.  Following the frontmost screen makes a swap
 * ordinary rather than rare -- opening Palette changes size, depth, stride and
 * colours at once -- and the alternative to rolling is either stopping at that
 * moment, which loses the part somebody was recording FOR, or appending frames
 * of one shape into a header that describes another, which is a file that
 * parses and decodes to rubbish.
 *
 * A `pal` with no frames yet is not a roll: a geom is always followed by one,
 * and the new file has not started.  The case that does roll on a palette
 * alone is something animating the ColorMap -- dragging a slider in Palette
 * Preferences is the one that produces a file a second, and it is the price of
 * every file being decodable on its own.
 *
 * No cap, no ring, no duration limit.  The only bound is the format's
 * frameCount field, and hitting it rolls the file the same way a geom does.
 */
interface Rec {
  /* The whole Screen and not the four numbers it used to be: the FORMAT is
     what decides how long the file's palette is, and a recorder that dropped
     it would write a truecolour capture with a header that says planar. */
  screen: Screen;
  rgb: Uint8Array;
  frames: Uint8Array<ArrayBuffer>[];
  /* When each frame arrived, on the page's clock.  buildPfs rebases them on
     the first, so what goes in the file is milliseconds from frame zero. */
  times: number[];
  /*
   * Where the pointer was on each frame, and which image it was.
   *
   * Both are real.  Absolute pointer positions are what this viewer SENDS --
   * the Amiga's pointer is put where the browser's is -- so the position it is
   * drawing is where the Amiga's pointer was.  The image is whatever the
   * `ptr` word last carried, stored UNSCALED in sprite pixels with its scale,
   * which is what the file wants; a session that never received one records
   * image 0 rather than the arrow this page draws for itself.
   */
  pointerAt: PointerAt[];
  pointers: Pointer[];
  bytes: number;
  part: number;
}

let rec: Rec | null = null;

const recEl = $("rec") as HTMLButtonElement;

function recName(r: Rec): string {
  return "amiga-" + r.screen.width + "x" + r.screen.height + "x" +
         r.screen.depth + (r.part > 1 ? "-" + r.part : "") + "-" +
         stamp() + ".pfs";
}

/* What there is so far, downloaded.  Returns whether anything went out: a
   roll that happens before the first frame has nothing to write. */
function recFlush(): boolean {
  if (rec === null || rec.frames.length === 0) return false;

  const name = recName(rec);
  try {
    download(new Blob([buildPfs(rec.screen, rec.rgb, rec.frames, rec.times,
                                rec.pointerAt, rec.pointers)],
                      { type: "application/octet-stream" }), name);
  } catch (e) {
    say("down", "the recording would not write: " +
                (e instanceof Error ? e.message : String(e)));
    return false;
  }
  say("up", "saved " + name + ", " + rec.frames.length + " frames over " +
            secs(rec.times[rec.times.length - 1] - rec.times[0]));
  return true;
}

function recShow(): void {
  if (rec === null) return;
  const ran = rec.times.length === 0
    ? 0 : rec.times[rec.times.length - 1] - rec.times[0];
  countEl.textContent = "REC " + rec.frames.length + " frames  " +
                        secs(ran) + "  " +
                        (rec.bytes / 1048576).toFixed(1) + " MB";
}

function recStart(): void {
  if (geom === null) { say("down", "there is no screen to record yet"); return; }
  rec = {
    screen: geom.screen,
    rgb: liveRgb.slice(),
    frames: [],
    times: [],
    pointerAt: [],
    pointers: [],
    bytes: 0,
    part: 1,
  };
  recEl.setAttribute("aria-pressed", "true");
  recShow();
}

function recStop(): void {
  if (rec === null) return;
  recFlush();
  rec = null;
  recEl.setAttribute("aria-pressed", "false");
}

/* A geom or a pal under a running recording: finish this file and open the
   next one on the shape that has just been announced. */
function recRoll(): void {
  if (rec === null || geom === null) return;

  const part = recFlush() ? rec.part + 1 : rec.part;

  rec = {
    screen: geom.screen,
    rgb: liveRgb.slice(),
    frames: [],
    times: [],
    pointerAt: [],
    pointers: [],
    bytes: 0,
    part,
  };
  recShow();
}

function recTake(planar: Uint8Array): void {
  if (rec === null) return;

  const p = view.pointerAt;

  /* The image goes in ONCE and every frame names it.  It changes about as
     often as somebody changes their pointer preference, so a table of one is
     the usual case and a copy per frame would be 2 KB a frame for nothing. */
  let image = 0;
  if (livePointer !== null) {
    if (rec.pointers.length === 0) {
      rec.pointers.push({
        width: livePointer.w, height: livePointer.h, depth: livePointer.depth,
        xScale: livePointer.xScale, yScale: livePointer.yScale,
        hotX: livePointer.hotX, hotY: livePointer.hotY,
        rgb: livePointer.rgb, bits: livePointer.bits,
      });
    }
    image = 1;
  }

  rec.frames.push(planar.slice());
  rec.times.push(performance.now());
  rec.pointerAt.push(p === null ? { x: 0, y: 0, image: 0 }
                                : { x: p.x, y: p.y, image });
  rec.bytes += planar.length;

  if (rec.frames.length >= PFS_MAX_FRAMES) recRoll();
}

recEl.onclick = () => { if (rec === null) recStart(); else recStop(); };

/* ------------------------------------------------------------- the reset -- */

const askEl = $("ask") as HTMLDialogElement;
const askWhatEl = $("askwhat");
const askYesEl = $("askyes") as HTMLButtonElement;
const askNoEl = $("askno") as HTMLButtonElement;

let askThen: (() => void) | null = null;

function ask(what: string, verb: string, then: () => void): void {
  askWhatEl.textContent = what;
  askYesEl.textContent = verb;
  askThen = then;
  askEl.showModal();
}

askNoEl.onclick = () => { askThen = null; askEl.close(); };

/* Escape closes a <dialog> without going through either gadget, and a pending
   action left behind would fire on the NEXT thing that asked. */
askEl.onclose = () => { askThen = null; };
askYesEl.onclick = () => {
  const go = askThen;
  askThen = null;
  askEl.close();
  if (go !== null) go();
};

/*
 * The truth about what a reset costs, because the button is one click away
 * from a machine somebody else may be using.  The server flushes every
 * mounted volume before it reboots, so files already written are safe; what
 * a program is still holding in memory is not, and nothing on an Amiga can
 * ask it to save.
 */
const RESET_WHAT =
  "Reboot the Amiga now?\n\n" +
  "Anything a program is holding unsaved is lost. Files already written are " +
  "flushed to disk first. This session drops and reconnects when the " +
  "machine comes back.";

const resetEl = $("reset") as HTMLButtonElement;

resetEl.onclick = () => {
  if (!live.open) { say("down", "there is no session to reset"); return; }
  ask(RESET_WHAT, "Reboot", () => {
    rebooting = true;
    live.word("reset");
    say("", "rebooting -- waiting for the Amiga to come back");
  });
};

liveEl.onclick = () => {
  /* Disconnect during a reboot wait means stop waiting: the poll is what
     would otherwise reopen the session a second later. */
  if (rebooting) {
    rebooting = false;
    clearInterval(rebootPoll);
    rebootPoll = 0;
    away = false;
    live.disconnect();
    return;
  }
  if (live.open) { away = false; live.disconnect(); return; }
  setPlaying(false);
  live.connect(urlEl.value);
};

/*
 * LEAVING THE PAGE HAS TO SAY SO, BECAUSE THE SCREEN IS A SINGLE-INSTANCE
 * RESOURCE
 *
 * The server allows one console session, so a viewer that walks away without
 * closing is a viewer still holding the screen as far as the server can tell.
 * It has a liveness bound underneath -- a peer that has gone quiet loses the
 * screen after ten seconds -- but ten seconds is the ceiling for the case
 * where NOBODY SAID ANYTHING, and navigating away is not that case: the
 * browser knows perfectly well that it is leaving.  Saying so turns a
 * ten-second wait into the sixty milliseconds it takes to ask again, which is
 * what "go away and come straight back" has to feel like.
 *
 * pagehide and not beforeunload: beforeunload does not fire reliably on
 * mobile and it makes the page ineligible for the back/forward cache, which
 * would trade this fix for a slower back button.  visibilitychange covers the
 * other half -- a tab switched away from is not being watched either, and a
 * screen nobody is looking at should not be one nobody else can have.
 *
 * `away` is what stops this from fighting the Connect button: only a session
 * THIS closed on the way out is reopened on the way back.
 */
let away = false;

const leaving = () => {
  if (!live.open && wireState !== "connecting") return;
  away = true;
  live.disconnect();
};

const returning = () => {
  if (!away) return;
  away = false;
  if (!live.open) live.connect(urlEl.value);
};

addEventListener("pagehide", leaving);
addEventListener("pageshow", returning);
document.addEventListener("visibilitychange", () => {
  if (document.visibilityState === "hidden") leaving();
  else returning();
});

urlEl.value = defaultEndpoint();

/* ------------------------------------------------------------- the input -- */

attachInput(view, $("box"), {
  send: (w) => live.word(w),
  log: (w) => log((live.open ? "> " : "· ") + w),
});

/* The recording's own counter, which is the only thing the deck's counter
   shows during a live session. */
setInterval(() => { if (rec !== null) recShow(); }, 500);

/* -------------------------------------------------------------- the view -- */

const aspectEl = $("aspect") as HTMLSelectElement;
const zoomEl = $("zoom") as HTMLSelectElement;
const logTogEl = $("logtog") as HTMLButtonElement;

aspectEl.onchange = () => view.setAspect(aspectEl.value as Aspect);
zoomEl.onchange = () => {
  const v = zoomEl.value;
  view.setZoom((v === "fit" ? "fit" : Number(v)) as Zoom);
};

/* Not remembered across reloads, which is what the aspect and zoom gadgets
   beside it do: one persistence rule for the deck, and it is "none". */
logTogEl.onclick = () => {
  const on = !document.body.classList.contains("log");
  document.body.classList.toggle("log", on);
  logTogEl.setAttribute("aria-pressed", String(on));
  if (on) logEl.scrollTop = logEl.scrollHeight;
};

addEventListener("keydown", (e) => {
  if (document.activeElement === $("box")) return;   /* typing at the Amiga */
  if (e.key === " ") { e.preventDefault(); setPlaying(!playing); }
  else if (e.key === "ArrowLeft") { setPlaying(false); showFrame(frame - 1); }
  else if (e.key === "ArrowRight") { setPlaying(false); showFrame(frame + 1); }
});

/* -------------------------------------------------------------- the start -- */

say("", "idle -- open a .pfs capture, or connect");
log("frames arrive in the encoder's format; input words go out and nothing");
log("on the Amiga reads them yet");

/*
 * ?pfs=URL loads a capture over HTTP.
 *
 * This is the ONE fetch the page can make and it does not weaken the
 * self-contained rule, which is about the page needing nothing to render:
 * with no query there is no request, and the guards in build-console.mjs
 * check exactly that -- no script, no font, no stylesheet, no source map.
 * A capture is a document somebody asked for by naming it, the same as the
 * file picker, and being able to send a colleague a link to one is the
 * difference between a capture being evidence and being an attachment.
 */
const wantPfs = new URLSearchParams(location.search).get("pfs");

if (wantPfs !== null && wantPfs !== "") {
  say("", "fetching " + wantPfs);
  fetch(wantPfs)
    .then((r) => {
      if (!r.ok) throw new Error(r.status + " " + r.statusText);
      return r.arrayBuffer();
    })
    .then((b) => loadCapture(wantPfs, b))
    .catch((e) => say("down", "not fetched: " +
                              (e instanceof Error ? e.message : String(e))));
} else if (location.protocol !== "file:") {
  /* Served over HTTP means something served it, and the thing that serves
     this page is the thing with the screen on it.  Opened off the
     filesystem it waits, because there is nothing to guess. */
  live.connect(urlEl.value);
}
