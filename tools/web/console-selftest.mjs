/*
 * Decode with the browser's own modules, outside a browser, and check the
 * pixels.
 *
 *   node tools/web/console-selftest.mjs [--png] [--slow]
 *
 * A viewer that draws garbage typechecks fine, so "it compiled" is not
 * evidence about a planar unpack: the bit order, the plane order and the
 * padding in bytesPerRow are four ways to be wrong that all produce a picture
 * rather than an error.  What is checked here is every pixel of a decoded
 * frame against a reference built the other way round -- chunky indices
 * straight through the palette -- so a transposed plane fails on frame one.
 *
 * The modules under test are the REAL ones.  esbuild bundles
 * client/console/{planar,pfs,tiles}.ts into build/web/ and this imports that,
 * rather than a copy of the algorithm that would agree with itself.
 *
 * --png writes the decoded frames out beside the bundle, because the last
 * check on a picture is a person looking at it.
 *
 * SPDX-License-Identifier: MIT
 */

import { mkdirSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

import * as esbuild from "esbuild";

import {
  drawFrame,
  encodeUpdate,
  makeGeometry,
  packBits,
  palette,
  synth,
  writePfs,
  writePng,
} from "./console-host.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, "..", "..");
const SRC = join(ROOT, "src", "tools", "web", "client", "console");
const OUTDIR = join(ROOT, "build", "web");

const wantPng = process.argv.includes("--png");
const slow = process.argv.includes("--slow");

/* --------------------------------------------------------- the modules -- */

mkdirSync(OUTDIR, { recursive: true });

const core = join(OUTDIR, "console-core.mjs");
await esbuild.build({
  stdin: {
    contents: 'export * from "./planar";\n' +
              'export * from "./pfs";\n' +
              'export * from "./tiles";\n',
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

/* The reference: chunky indices through the palette, which shares no code
   with the planar path it is checking. */
function referenceRGBA(w, h, depth, t, rgb) {
  const px = drawFrame(w, h, depth, t);
  const out = new Uint8Array(w * h * 4);
  for (let i = 0; i < w * h; i++) {
    const v = px[i];
    out[i * 4] = rgb[v * 3];
    out[i * 4 + 1] = rgb[v * 3 + 1];
    out[i * 4 + 2] = rgb[v * 3 + 2];
    out[i * 4 + 3] = 255;
  }
  return out;
}

function firstDifference(a, b, w) {
  for (let i = 0; i < a.length; i++) {
    if (a[i] !== b[i]) {
      const px = (i / 4) | 0;
      return "byte " + i + ", pixel " + (px % w) + "," + ((px / w) | 0) +
             ": " + a[i] + " vs " + b[i];
    }
  }
  return "";
}

function decodeFrame(cap, i) {
  const words = new Uint32Array(cap.screen.width * cap.screen.height);
  M.decodeInto(cap.screen, cap.frames, M.frameAt(cap, i), cap.palette, words);
  return new Uint8Array(words.buffer);
}

/* ------------------------------------------------ the picture is right -- */

/*
 * The padded case is not a curiosity.  A 634-wide screen has bytesPerRow 80
 * and 6 pixels of a row that are not on it; a decoder that computes the
 * stride from the width shears the picture one byte further left every row,
 * and that is the single most likely bug in this file.
 */
const SHAPES = [
  ["640x256x3 hires PAL", 640, 256, 3, undefined],
  ["320x200x5 lores NTSC", 320, 200, 5, undefined],
  ["634x242x4 padded bpr", 634, 242, 4, 80],
  ["800x600x8 big", 800, 600, 8, undefined],
  ["640x480x3", 640, 480, 3, undefined],
];

for (const [name, w, h, depth, bpr] of SHAPES) {
  const cap = M.parsePfs(bufferToArrayBuffer(writePfs(synth(w, h, depth, 3, bpr))));

  ok(name + ": header survives the round trip",
     cap.screen.width === w && cap.screen.height === h &&
     cap.screen.depth === depth && cap.frameCount === 3,
     JSON.stringify(cap.screen));

  let same = true;
  let why = "";
  for (let t = 0; t < 3; t++) {
    const got = decodeFrame(cap, t);
    const want = referenceRGBA(w, h, depth, t, palette(depth));
    if (Buffer.compare(Buffer.from(got), Buffer.from(want)) !== 0) {
      same = false;
      why = "frame " + t + ", " + firstDifference(got, want, w);
      break;
    }
  }
  ok(name + ": every pixel of every frame matches the reference", same, why);

  if (wantPng) {
    const file = join(OUTDIR, "console-" + w + "x" + h + "x" + depth + ".png");
    writeFileSync(file, writePng(decodeFrame(cap, 0), w, h));
    console.log("      %s", file);
  }
}

/* ---------------------------------------------- the container complains -- */

{
  const good = writePfs(synth(64, 32, 2, 2));

  const bad = Buffer.from(good);
  bad.write("PFSX", 0, "latin1");
  ok("a wrong magic is refused", throws(() => M.parsePfs(bufferToArrayBuffer(bad))));

  const short = Buffer.from(good.subarray(0, good.length - 100));
  const msg = message(() => M.parsePfs(bufferToArrayBuffer(short)));
  ok("a truncated capture says so, in frames", /truncated: 1 of 2 frames/.test(msg), msg);

  const deep = Buffer.from(good);
  deep.writeUInt8(9, 8);
  ok("depth 9 is refused", throws(() => M.parsePfs(bufferToArrayBuffer(deep))));

  const narrow = Buffer.from(good);
  narrow.writeUInt16BE(4, 10);            /* bytesPerRow 4, width 64 */
  ok("a bytesPerRow too small for the width is refused",
     throws(() => M.parsePfs(bufferToArrayBuffer(narrow))));
}

/* --------------------------------------------------------- the packing -- */

{
  const cases = [
    Buffer.alloc(0),
    Buffer.from([1]),
    Buffer.alloc(300),
    Buffer.from(Array.from({ length: 500 }, (_, i) => (i * 37) & 0xff)),
    Buffer.concat([Buffer.alloc(200, 7), Buffer.from([1, 2, 3]), Buffer.alloc(129, 9)]),
  ];

  let all = true;
  let why = "";
  for (const c of cases) {
    const packed = packBits(c);
    const out = new Uint8Array(c.length);
    M.unpackBits(packed, 0, packed.length, out, c.length);
    if (Buffer.compare(Buffer.from(out), c) !== 0) {
      all = false;
      why = "length " + c.length;
      break;
    }
  }
  ok("PackBits round trips", all, why);

  ok("PackBits refuses to run off the end",
     throws(() => M.unpackBits(Buffer.from([0x00]), 0, 1, new Uint8Array(4), 4)));
}

/* ----------------------------------------------------------- the tiles -- */

/*
 * The placeholder framing, both sides.  What is being checked is not the
 * format -- it is provisional -- but that the XOR chain reconstructs the
 * capture exactly: an update path that is one byte out anywhere accumulates,
 * so frame 20 is the test and frame 1 is not.
 */
for (const [name, w, h, depth, tw, th] of [
  ["640x480x3 16x16 tiles", 640, 480, 3, 16, 16],
  ["800x600x8 32x16 tiles", 800, 600, 8, 32, 16],
  ["634x242x4 ragged grid", 634, 242, 4, 32, 32],
]) {
  const cap = synth(w, h, depth, 20);
  const g = makeGeometry(cap.screen, tw, th);
  const cg = M.makeGeometry(cap.screen, tw, th);
  const fb = new Uint8Array(cap.stride);
  const scratch = new Uint8Array(M.scratchBytes(cg));

  let exact = true;
  let why = "";
  let bytes = 0;

  for (let t = 0; t < cap.frameCount; t++) {
    const prev = t === 0 ? new Uint8Array(cap.stride)
                         : cap.frames.subarray((t - 1) * cap.stride, t * cap.stride);
    const next = cap.frames.subarray(t * cap.stride, (t + 1) * cap.stride);
    const u = encodeUpdate(g, prev, next, t, t === 0);
    bytes += u.length;

    M.applyUpdate(cg, u, fb, scratch);

    if (Buffer.compare(Buffer.from(fb), Buffer.from(next)) !== 0) {
      exact = false;
      why = "frame " + t;
      break;
    }
  }

  ok(name + ": 20 updates reconstruct the capture byte for byte", exact, why);
  console.log("      " + bytes + " bytes of updates for " + cap.frameCount +
              " frames, " +
              ((bytes * 100) / (cap.stride * cap.frameCount)).toFixed(2) +
              "% of raw");

  const word = "geom " + w + " " + h + " " + depth + " " +
               cap.screen.bytesPerRow + " " + tw + " " + th;
  const parsed = M.geometryFromWord(word);
  ok(name + ": the geom word parses to the same grid",
     parsed.across === cg.across && parsed.down === cg.down);
}

{
  const rgb = palette(3);
  const hex = Buffer.from(rgb).toString("hex");
  const back = M.paletteFromWord("pal " + hex, 3);
  ok("the pal word round trips", Buffer.compare(Buffer.from(back), rgb) === 0);
  ok("a pal word of the wrong length is refused",
     throws(() => M.paletteFromWord("pal " + hex.slice(0, 10), 3)));
  ok("a tile width that is not a multiple of 8 is refused",
     throws(() => M.makeGeometry({ width: 64, height: 64, depth: 2, bytesPerRow: 8 }, 12, 16)));
}

/* ------------------------------------------------------------ the clock -- */

/*
 * Timed against a rotation of frames rather than one frame over and over:
 * decoding the same 230 KB twenty thousand times measures a cache that a
 * viewer never gets.
 */
function timeDecode(w, h, depth, label) {
  const cap = M.parsePfs(bufferToArrayBuffer(writePfs(synth(w, h, depth, 8))));
  const words = new Uint32Array(w * h);

  const runs = slow ? 400 : 120;
  /* Warm, so the first measurement is not the JIT compiling the loop. */
  for (let i = 0; i < 20; i++) {
    M.decodeInto(cap.screen, cap.frames, M.frameAt(cap, i % 8), cap.palette, words);
  }

  const t0 = process.hrtime.bigint();
  for (let i = 0; i < runs; i++) {
    M.decodeInto(cap.screen, cap.frames, M.frameAt(cap, i % 8), cap.palette, words);
  }
  const ms = Number(process.hrtime.bigint() - t0) / 1e6 / runs;

  /* Built by hand: node's console.log has no field widths, and a column
     that does not line up is a column nobody reads. */
  console.log("time  " + label.padEnd(13) +
              col(ms.toFixed(3) + " ms/frame") +
              col((1000 / ms).toFixed(0) + " fps") +
              col(((w * h) / ms / 1000).toFixed(0) + " Mpx/s"));
  return ms;
}

console.log("");
timeDecode(640, 256, 3, "640x256x3");
timeDecode(640, 480, 3, "640x480x3");
timeDecode(800, 600, 8, "800x600x8");
timeDecode(1280, 1024, 8, "1280x1024x8");

/* And the live path end to end: unpack the update, XOR it in, decode the
   damage.  This is the number that matters for a session, and it is not the
   whole-screen decode above -- a Workbench that is mostly still touches a
   handful of tiles. */
function timeLive(w, h, depth, tw, th, label) {
  const cap = synth(w, h, depth, 8);
  const g = makeGeometry(cap.screen, tw, th);
  const cg = M.makeGeometry(cap.screen, tw, th);
  const fb = new Uint8Array(cap.stride);
  const scratch = new Uint8Array(M.scratchBytes(cg));
  const words = new Uint32Array(w * h);
  const pal = M.palette32(cap.rgb, depth);

  const updates = [];
  for (let t = 0; t < cap.frameCount; t++) {
    const prev = t === 0 ? new Uint8Array(cap.stride)
                         : cap.frames.subarray((t - 1) * cap.stride, t * cap.stride);
    const next = cap.frames.subarray(t * cap.stride, (t + 1) * cap.stride);
    updates.push(encodeUpdate(g, prev, next, t, t === 0));
  }

  const runs = slow ? 400 : 120;
  let bytes = 0;
  let area = 0;

  /* The keyframe is applied once and then only the deltas are timed: a
     session sends one keyframe and then runs. */
  M.applyUpdate(cg, updates[0], fb, scratch);

  const t0 = process.hrtime.bigint();
  for (let i = 0; i < runs; i++) {
    const u = updates[1 + (i % (updates.length - 1))];
    const d = M.applyUpdate(cg, u, fb, scratch);
    M.decodeRectInto(cap.screen, fb, 0, pal, words, d.x0, d.y0, d.x1, d.y1);
    bytes += u.length;
    area += (d.x1 - d.x0) * (d.y1 - d.y0);
  }
  const ms = Number(process.hrtime.bigint() - t0) / 1e6 / runs;

  console.log("live  " + label.padEnd(13) +
              col(ms.toFixed(3) + " ms/update") +
              col((1000 / ms).toFixed(0) + " fps") +
              col(Math.round(bytes / runs) + " B/update") +
              col(Math.round(area / runs) + " px damaged"));
}

console.log("");
timeLive(640, 480, 3, 16, 16, "640x480x3");
timeLive(800, 600, 8, 32, 16, "800x600x8");

console.log("");
if (failures > 0) {
  console.log("selftest: " + failures + " checks failed");
  process.exit(1);
}
console.log("selftest: all checks passed");

/* ---------------------------------------------------------------- bits -- */

function col(s) {
  return s.padStart(18) + "  ";
}

function bufferToArrayBuffer(b) {
  return b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength);
}

function throws(fn) {
  try { fn(); return false; } catch { return true; }
}

function message(fn) {
  try { fn(); return ""; } catch (e) { return e instanceof Error ? e.message : String(e); }
}
