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

import { existsSync, mkdirSync, readFileSync, readdirSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

import * as esbuild from "esbuild";

import {
  drawFrame,
  encodeFrame,
  makeGeometry,
  packBits,
  palette,
  readApng,
  readPng,
  synth,
  synthChunky,
  synthRgb565,
  quantise565,
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
              'export * from "./tiles";\n' +
              'export * from "./apng";\n',
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

/* -------------------------------------------- the recorder writes it back -- */

/*
 * buildPfs() is what the Record button hands to the browser, and the player
 * on the same page opens it with parsePfs().  A file the player cannot read
 * is the one failure that makes the button pointless, so the check is byte
 * equality against a file parsePfs already accepted: parse, hand the frames
 * straight back, and the result has to be the same bytes.
 */
function pfsFrames(cap) {
  const out = [];
  for (let i = 0; i < cap.frameCount; i++) {
    const at = M.frameAt(cap, i);
    out.push(cap.frames.subarray(at, at + cap.stride));
  }
  return out;
}

function pfsTimes(cap) {
  return Array.from(cap.times);
}

/* ------------------------------------------------ the picture is right -- */

/*
 * The padded case is not a curiosity.  A 634-wide screen has bytesPerRow 80
 * and 6 pixels of a row that are not on it; a decoder that computes the
 * stride from the width shears the picture one byte further left every row,
 * and that is the single most likely bug in this file.
 */
/* The three source layouts, spelled here so the tables below read as a format
   and not as a boolean.  A boolean is what this used to carry and it is
   exactly what a third format does not fit into. */
const FMT_PLANAR = 0;
const FMT_CLUT8  = 1;
const FMT_RGB565 = 2;

/* One capture of the shape asked for, whichever format it is. */
function synthOf(fmt, w, h, depth, frames, bpr) {
  if (fmt === FMT_RGB565) return synthRgb565(w, h, frames, bpr);
  if (fmt === FMT_CLUT8)  return synthChunky(w, h, frames, bpr);
  return synth(w, h, depth, frames, bpr);
}

/* And the colours a decode of it has to produce.  A truecolour capture has no
   palette on the wire, so the reference uses the one the picture was drawn
   from, put through the format's own loss. */
function referenceOf(fmt, depth) {
  if (fmt === FMT_RGB565) return quantise565(palette(8));
  if (fmt === FMT_CLUT8)  return palette(8);
  return palette(depth);
}

const SHAPES = [
  ["640x256x3 hires PAL", 640, 256, 3, undefined, FMT_PLANAR],
  ["320x200x5 lores NTSC", 320, 200, 5, undefined, FMT_PLANAR],
  ["634x242x4 padded bpr", 634, 242, 4, 80, FMT_PLANAR],
  ["800x600x8 big", 800, 600, 8, undefined, FMT_PLANAR],
  ["640x480x3", 640, 480, 3, undefined, FMT_PLANAR],
  /* The RTG shapes.  The reference is built from drawFrame's indices straight
     through the palette and knows nothing about any of the layouts, so a
     chunky decode that read the bytes as bitplanes fails here on frame 0 --
     which is the check the planar path has had and the one a new format needs
     most.
     The truecolour ones carry the same picture with no palette at all, and
     their reference goes through quantise565() because that is the loss the
     format applies: 640 wide is 1280 bytes to a row and a whole number of
     16-byte tiles, 404 wide is 808 and is not. */
  ["640x480 chunky", 640, 480, 8, undefined, FMT_CLUT8],
  ["804x300 chunky padded bpr", 804, 300, 8, 808, FMT_CLUT8],
  ["640x480 rgb565", 640, 480, 16, undefined, FMT_RGB565],
  ["404x200 rgb565 ragged bpr", 404, 200, 16, 808, FMT_RGB565],
];


for (const [name, w, h, depth, bpr, fmt] of SHAPES) {
  const cap = M.parsePfs(bufferToArrayBuffer(writePfs(
    synthOf(fmt, w, h, depth, 3, bpr))));

  ok(name + ": header survives the round trip",
     cap.screen.width === w && cap.screen.height === h &&
     cap.screen.depth === depth && cap.frameCount === 3 &&
     M.screenFormat(cap.screen) === fmt,
     JSON.stringify(cap.screen));

  let same = true;
  let why = "";
  for (let t = 0; t < 3; t++) {
    const got = decodeFrame(cap, t);
    /* drawFrame draws at the capture's own depth: eight for both card
       formats, since a card screen is drawn from the same indices. */
    const want = referenceRGBA(w, h, fmt === FMT_PLANAR ? depth : 8, t,
                               referenceOf(fmt, depth));
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

for (const [name, w, h, depth, bpr, fmt] of SHAPES) {
  const src = writePfs(synthOf(fmt, w, h, depth, 3, bpr));
  const cap = M.parsePfs(bufferToArrayBuffer(src));
  const built = M.buildPfs(cap.screen, cap.rgb, pfsFrames(cap),
                           pfsTimes(cap), cap.pointerAt, cap.pointers);

  ok(name + ": buildPfs writes back the bytes parsePfs read",
     built.length === src.length && built.every((v, i) => v === src[i]),
     built.length + " vs " + src.length + " bytes");
}

const SCR2 = { width: 640, height: 256, depth: 2, bytesPerRow: 80 };
const F2 = () => new Uint8Array(2 * 80 * 256);

ok("buildPfs refuses a frame of the wrong length",
   throws(() => M.buildPfs(SCR2, new Uint8Array(12), [new Uint8Array(17)], [0])));

ok("buildPfs refuses no frames at all",
   throws(() => M.buildPfs(SCR2, new Uint8Array(12), [], [])));

ok("buildPfs refuses a timestamp count that is not the frame count",
   throws(() => M.buildPfs(SCR2, new Uint8Array(12), [F2(), F2()], [0])));

/* --------------------------------------------- the clock and the pointer -- */

/*
 * A recording has no rate, so what is checked is that IRREGULAR intervals
 * survive exactly: 0, 17, 900, 901 is an idle screen with a burst in it,
 * which is what a frames-per-second gadget could never have represented.
 */
{
  const cap = M.parsePfs(bufferToArrayBuffer(Buffer.from(
    M.buildPfs(SCR2, new Uint8Array(12), [F2(), F2(), F2(), F2()],
               [1000.4, 1017.6, 1900.2, 1901.0]))));

  ok("the timeline is rebased on the first frame and kept exactly",
     Array.from(cap.times).join(",") === "0,17,900,901",
     Array.from(cap.times).join(","));

  ok("the duration is the last timestamp", M.pfsDuration(cap) === 901,
     String(M.pfsDuration(cap)));

  ok("a capture with no pointer names image 0 on every frame",
     cap.pointers.length === 0 && cap.pointerAt.every((p) => p.image === 0));
}

/*
 * The pointer half.  A 16x16x2 sprite with a hotspot away from the corner and
 * a 2x1 scale is the ordinary hires case; what is checked is that every field
 * comes back, the colours included -- they come from the SPRITE palette and
 * not the screen's, so a reader that took them from the screen would draw the
 * wrong pointer and no other check here would notice.
 */
{
  const ptr = {
    width: 16, height: 16, depth: 2, xScale: 2, yScale: 1,
    hotX: 1, hotY: 2,
    rgb: Uint8Array.from([0xee, 0x22, 0x22, 0x11, 0x33, 0xff, 0xff, 0xff, 0xff]),
    bits: Uint8Array.from({ length: 2 * 2 * 16 }, (_v, i) => i & 0xff),
  };
  const src = M.buildPfs(SCR2, new Uint8Array(12), [F2(), F2()], [0, 40],
                         [{ x: 12, y: 34, image: 1 },
                          { x: -3, y: 300, image: 0 }], [ptr]);
  const cap = M.parsePfs(bufferToArrayBuffer(Buffer.from(src)));
  const got = cap.pointers[0];

  ok("a pointer image survives the round trip",
     cap.pointers.length === 1 && got.width === 16 && got.height === 16 &&
     got.depth === 2 && got.xScale === 2 && got.yScale === 1 &&
     got.hotX === 1 && got.hotY === 2,
     JSON.stringify(cap.pointers.length ? {
       w: got.width, h: got.height, d: got.depth,
       sx: got.xScale, sy: got.yScale, hx: got.hotX, hy: got.hotY } : null));

  ok("the sprite carries its own colours, three of them at depth 2",
     got.rgb.length === 9 &&
     Array.from(got.rgb).join(",") === Array.from(ptr.rgb).join(","));

  ok("the sprite bits come back byte for byte",
     got.bits.length === ptr.bits.length &&
     got.bits.every((v, i) => v === ptr.bits[i]));

  ok("a negative position survives, which an overscan screen produces",
     cap.pointerAt[1].x === -3 && cap.pointerAt[1].y === 300 &&
     cap.pointerAt[1].image === 0,
     JSON.stringify(cap.pointerAt[1]));

  ok("the writer reproduces a file with a pointer in it",
     (() => {
       const back = M.buildPfs(cap.screen, cap.rgb, pfsFrames(cap),
                               pfsTimes(cap), cap.pointerAt, cap.pointers);
       return back.length === src.length && back.every((v, i) => v === src[i]);
     })());

  ok("a frame naming an image that is not in the file is refused",
     throws(() => M.parsePfs(bufferToArrayBuffer(Buffer.from(
       M.buildPfs(SCR2, new Uint8Array(12), [F2(), F2()], [0, 40],
                  [{ x: 0, y: 0, image: 3 }, { x: 0, y: 0, image: 0 }],
                  [ptr]))))));
}

ok("a file with the old magic is refused rather than read as this one",
   (() => {
     const src = M.buildPfs(SCR2, new Uint8Array(12), [F2()], [0]);
     src[3] = 0x31;
     return throws(() => M.parsePfs(bufferToArrayBuffer(Buffer.from(src))));
   })());

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
 * The encoder's format, both sides.  What is checked is not the format --
 * that is the encoder's header -- but that a chain of deltas reconstructs the
 * capture exactly: an apply that is one byte out anywhere accumulates, so
 * frame 20 is the test and frame 1 is not.
 *
 * Tile widths are BYTES.  The sizes are rfbbench's own sweep, and the ragged
 * grid is there because bytesPerRow is not always a whole number of tiles.
 */
for (const [name, w, h, depth, tw, th, fmt] of [
  ["640x480x3 16x8 tiles", 640, 480, 3, 16, 8, FMT_PLANAR],
  ["800x600x8 32x16 tiles", 800, 600, 8, 32, 16, FMT_PLANAR],
  ["634x242x4 ragged grid", 634, 242, 4, 12, 10, FMT_PLANAR],
  /* And the RTG shapes: one plane of bytes, a tile grid over a stride that is
     bytes and not pixels, and a width that pads so the tile at the right
     edge is clipped.  The 11-byte tile on the last row is the case nothing
     else reaches: an odd tile width puts a tile boundary between the two
     bytes of a pixel, so the damage rectangle has to round its left edge
     down to a pixel and its right edge up to one. */
  ["640x480 chunky 16x8 tiles", 640, 480, 8, 16, 8, FMT_CLUT8],
  ["804x300 chunky ragged grid", 804, 300, 8, 32, 16, FMT_CLUT8],
  ["640x480 rgb565 16x8 tiles", 640, 480, 16, 16, 8, FMT_RGB565],
  ["404x200 rgb565 odd tile", 404, 200, 16, 11, 10, FMT_RGB565],
]) {
  const cap = synthOf(fmt, w, h, depth, 20);
  const g = makeGeometry(cap.screen, tw, th);
  const cg = M.makeGeometry(cap.screen, tw, th);
  const fb = new Uint8Array(cap.stride);
  const shadow = new Uint8Array(cap.stride);
  const scratch = new Uint8Array(M.scratchBytes(cg));

  let exact = true;
  let why = "";
  let bytes = 0;

  for (let t = 0; t < cap.frameCount; t++) {
    const next = cap.frames.subarray(t * cap.stride, (t + 1) * cap.stride);
    /* A scroll every fifth frame, so the COPY op is on the path rather than
       being a branch nothing ever takes. */
    const copy = t > 0 && t % 5 === 0
      ? { x0: 0, w: cap.screen.bytesPerRow, y0: 0, h: h - 16, dy: 16 }
      : undefined;
    const u = encodeFrame(g, shadow, next, t, copy);
    bytes += u.length;

    M.applyUpdate(cg, u, fb, scratch);

    if (Buffer.compare(Buffer.from(fb), Buffer.from(next)) !== 0) {
      exact = false;
      why = "frame " + t;
      break;
    }
  }

  ok(name + ": 20 frames, 3 of them scrolls, reconstruct byte for byte",
     exact, why);
  console.log("      " + bytes + " bytes for " + cap.frameCount +
              " frames, " +
              ((bytes * 100) / (cap.stride * cap.frameCount)).toFixed(2) +
              "% of raw");

  const word = "geom " + w + " " + h + " " + cap.screen.depth + " " +
               cap.screen.bytesPerRow + " " + tw + " " + th + " " +
               fmt;
  const parsed = M.geometryFromWord(word);
  ok(name + ": the geom word parses to the same grid",
     parsed.across === cg.across && parsed.down === cg.down &&
     M.screenFormat(parsed.screen) === fmt);
}

{
  /* The three ways a frame can be malformed that a decoder must not take a
     guess at.  A viewer that carries on from any of them draws a screen that
     is half of two different frames. */
  const cap = synth(64, 32, 2, 2);
  const cg = M.makeGeometry(cap.screen, 4, 8);
  const fb = new Uint8Array(cap.stride);
  const scratch = new Uint8Array(M.scratchBytes(cg));
  const good = encodeFrame(cg, new Uint8Array(cap.stride),
                           cap.frames.subarray(0, cap.stride), 0);

  ok("a frame with the wrong version is refused",
     throws(() => {
       const b = Buffer.from(good); b[0] = 9;
       M.applyUpdate(cg, b, fb, scratch);
     }));
  ok("a frame with no END op is refused",
     throws(() => M.applyUpdate(cg, good.subarray(0, good.length - 1), fb, scratch)));
  ok("an unknown op is refused",
     throws(() => {
       const b = Buffer.concat([good.subarray(0, 4), Buffer.from([0x7f])]);
       M.applyUpdate(cg, b, fb, scratch);
     }));
  ok("a copy that reads off the screen is refused",
     throws(() => {
       const c = Buffer.alloc(11);
       c.writeUInt8(1, 0);
       c.writeUInt16BE(0, 1); c.writeUInt16BE(8, 3);
       c.writeUInt16BE(0, 5); c.writeUInt16BE(32, 7);
       c.writeInt16BE(8, 9);
       M.applyUpdate(cg, Buffer.concat([good.subarray(0, 4), c,
                                        Buffer.from([0])]), fb, scratch);
     }));
}

{
  const rgb = palette(3);
  const hex = Buffer.from(rgb).toString("hex");
  const back = M.paletteFromWord("pal " + hex, 8);
  ok("the pal word round trips", Buffer.compare(Buffer.from(back), rgb) === 0);
  ok("a pal word of the wrong length is refused",
     throws(() => M.paletteFromWord("pal " + hex.slice(0, 10), 8)));
  ok("a tile wider than RFB_MAX_TILE_W is refused",
     throws(() => M.makeGeometry({ width: 64, height: 64, depth: 2, bytesPerRow: 8 }, 65, 16)));
  ok("a tile taller than RFB_MAX_TILE_H is refused",
     throws(() => M.makeGeometry({ width: 64, height: 64, depth: 2, bytesPerRow: 8 }, 8, 65)));
}

/* ------------------------------------------------- the real captures -- */

/*
 * Grabs off an A1200 running Kickstart 3.1 40.68 and Workbench 3.1 40.42,
 * with a .png beside each one decoded by the capture side's own host decoder.
 * Those PNGs are the ground truth this viewer has to agree with: they share
 * no code with anything here, so a match is evidence about the planar unpack
 * and the palette rather than about my arithmetic being self-consistent.
 *
 * Stock Workbench 3.1 comes up TWO planes deep whatever the machine is, so
 * the depth2 set is the real case and depth4 is what a person who changed it
 * in Prefs gets.  640x256, bytesPerRow 80.
 *
 * Skipped, not failed, when the captures are not on this machine: they live
 * outside the tree.
 */
const CONTENT = process.env.CONSOLE_CONTENT || "/Users/turo/rfb-proto-content";

if (!existsSync(CONTENT)) {
  console.log("skip  real captures: %s is not here", CONTENT);
} else {
  for (const dir of readdirSync(CONTENT)) {
    const at = join(CONTENT, dir);
    let files;
    try { files = readdirSync(at); } catch { continue; }

    for (const name of files.filter((f) => f.endsWith(".pfs")).sort()) {
      const raw = readFileSync(join(at, name));
      const cap = M.parsePfs(bufferToArrayBuffer(raw));

      const back = M.buildPfs(cap.screen, cap.rgb, pfsFrames(cap),
                              pfsTimes(cap), cap.pointerAt, cap.pointers);
      ok(dir + "/" + name + ": buildPfs reproduces the file exactly",
         back.length === raw.length && back.every((v, i) => v === raw[i]),
         back.length + " vs " + raw.length + " bytes");

      const stem = name.replace(/\.pfs$/, "");
      const w = cap.screen.width, h = cap.screen.height;

      const shots = files.filter((f) => f.endsWith(".png") &&
                                        f.replace(/^d\d+-/, "").startsWith(stem + "-"));

      for (const shot of shots.sort()) {
        const png = readPng(readFileSync(join(at, shot)));

        if (png.width !== w || png.height !== h) {
          ok(dir + "/" + shot + ": same geometry as the capture", false,
             png.width + "x" + png.height + " vs " + w + "x" + h);
          continue;
        }

        /* Which frame it is is not written down, so it is found: a match on
           exactly one frame is a stronger statement than a match on the one
           somebody said it would be. */
        const hits = [];
        for (let t = 0; t < cap.frameCount; t++) {
          if (sameRGB(decodeFrame(cap, t), png.rgba, w * h)) hits.push(t);
        }

        ok(dir + "/" + shot + ": a decoded frame matches it exactly",
           hits.length > 0,
           hits.length === 0
             ? firstDifference(decodeFrame(cap, 0), png.rgba, w) +
               " against frame 0 of " + cap.frameCount
             : "");
        if (hits.length > 0) {
          console.log("      %s %dx%dx%d, %d frames, matched at %s",
                      dir + "/" + name, w, h, cap.screen.depth, cap.frameCount,
                      hits.length > 4 ? hits.length + " frames (a still)"
                                      : hits.join(","));
        }
      }
    }
  }

  /* And the clock, on the frames a person will actually be looking at. */
  for (const [dir, name] of [["depth2", "windows.pfs"], ["depth4", "windows.pfs"]]) {
    const file = join(CONTENT, dir, name);
    if (!existsSync(file)) continue;
    const cap = M.parsePfs(bufferToArrayBuffer(readFileSync(file)));
    const words = new Uint32Array(cap.screen.width * cap.screen.height);
    const runs = slow ? 400 : 200;

    for (let i = 0; i < 20; i++) {
      M.decodeInto(cap.screen, cap.frames, M.frameAt(cap, i % cap.frameCount),
                   cap.palette, words);
    }
    const t0 = process.hrtime.bigint();
    for (let i = 0; i < runs; i++) {
      M.decodeInto(cap.screen, cap.frames, M.frameAt(cap, i % cap.frameCount),
                   cap.palette, words);
    }
    const ms = Number(process.hrtime.bigint() - t0) / 1e6 / runs;
    console.log("real  " + (dir + "/" + name).padEnd(20) +
                col(ms.toFixed(3) + " ms/frame") +
                col((1000 / ms).toFixed(0) + " fps"));
  }
}

/* ------------------------------------------------- the chipset modes -- */

/*
 * HAM6, HAM8 and extra half-brite, against the picture tests/perf/chipscreen.c
 * draws on the guest.
 *
 * Checked as PROPERTIES OF THE PICTURE and not against a second decoder.  A
 * reference implementation of hold-and-modify would be a third place to make
 * the same mistake, and the mistakes this format invites -- the control codes
 * permuted, the data field taken from the wrong end -- all survive being
 * decoded consistently.  What does not survive is the picture: a band written
 * with the code that means red has to come out red, with green and blue left
 * at what the row started from, climbing from dark to bright.
 *
 * chipscreen.c writes exactly these indices, so a change to either file that
 * the other does not follow lands here.
 */
const CHIP_W = 320;
const CHIP_H = 256;

/* chipscreen.c base_rgb(): a lattice of 17, exact in a 4-bit colour register,
   with 0 black so a HAM row that starts from it carries only the ramp. */
function chipPalette(colours) {
  const rgb = new Uint8Array(colours * 3);
  for (let i = 0; i < colours; i++) {
    rgb[i * 3] = 17 * (i & 15);
    rgb[i * 3 + 1] = 17 * (15 - (i & 15));
    rgb[i * 3 + 2] = 17 * (((i * 7) + (i >> 4)) & 15);
  }
  rgb[0] = 0; rgb[1] = 0; rgb[2] = 0;
  rgb[3] = 255; rgb[4] = 255; rgb[5] = 255;
  return rgb;
}

function chipPlanes(idxAt, depth) {
  const bpr = ((CHIP_W + 15) >> 4) * 2;
  const buf = new Uint8Array(bpr * CHIP_H * depth);
  for (let y = 0; y < CHIP_H; y++) {
    for (let x = 0; x < CHIP_W; x++) {
      const v = idxAt(x, y);
      for (let p = 0; p < depth; p++) {
        if ((v >> p) & 1) buf[p * bpr * CHIP_H + y * bpr + (x >> 3)] |= 0x80 >> (x & 7);
      }
    }
  }
  return { buf, bpr };
}

function chipDecode(format, depth, colours, idxAt) {
  const { buf, bpr } = chipPlanes(idxAt, depth);
  const screen = { width: CHIP_W, height: CHIP_H, depth, bytesPerRow: bpr, format };
  const rgb = chipPalette(colours);
  const words = new Uint32Array(CHIP_W * CHIP_H);
  M.decodeInto(screen, buf, 0, M.renderPalette(screen, rgb), words);
  const b = new Uint8Array(words.buffer);
  return {
    rgb,
    at: (x, y) => [b[(y * CHIP_W + x) * 4], b[(y * CHIP_W + x) * 4 + 1],
                   b[(y * CHIP_W + x) * 4 + 2]],
  };
}

/* chipscreen.c ham_row(): band 0 the base colours, then control 2 red, 3
   green and 1 blue.  That order is the hardware's and it is the one a decoder
   gets wrong. */
for (const [name, format, depth, shift, colours] of
     [["HAM6", M.FMT_HAM6, 6, 4, 16], ["HAM8", M.FMT_HAM8, 8, 6, 64]]) {
  const n = 1 << shift;
  const d = chipDecode(format, depth, colours, (x, y) => {
    const band = ((y * 4) / CHIP_H) | 0;
    const k = ((x * n) / CHIP_W) | 0;
    if (band === 0) return k;
    if (band === 1) return (2 << shift) | k;
    if (band === 2) return (3 << shift) | k;
    return (1 << shift) | k;
  });
  const rowOf = (band) => (((band * CHIP_H) / 4) | 0) + 4;

  let wrong = 0;
  for (let x = 0; x < CHIP_W; x++) {
    const k = ((x * n) / CHIP_W) | 0;
    const p = d.at(x, rowOf(0));
    if (p[0] !== d.rgb[k * 3] || p[1] !== d.rgb[k * 3 + 1] ||
        p[2] !== d.rgb[k * 3 + 2]) wrong++;
  }
  ok(name + ": a base-colour band is the palette", wrong === 0,
     wrong + " pixels differ");

  for (const [band, comp, cname] of [[1, 0, "red"], [2, 1, "green"], [3, 2, "blue"]]) {
    const y = rowOf(band);
    let rising = true, bled = 0, last = -1;
    for (let x = 0; x < CHIP_W; x++) {
      const p = d.at(x, y);
      if (p[comp] < last) rising = false;
      last = p[comp];
      for (let c = 0; c < 3; c++) if (c !== comp && p[c] !== 0) bled++;
    }
    ok(name + ": the " + cname + " band climbs and reaches full",
       rising && d.at(CHIP_W - 1, y)[comp] >= 250 && d.at(0, y)[comp] === 0,
       "left " + d.at(0, y)[comp] + ", right " + d.at(CHIP_W - 1, y)[comp]);
    /* The row started from black, so a modify of one component must leave the
       other two at zero.  This is the check a permuted control code fails. */
    ok(name + ": the " + cname + " band holds the other two at black",
       bled === 0, bled + " components not zero");
  }
}

/* chipscreen.c ehb_row(): band 0 is 0..31 and band 1 is 32..63 under it, so a
   correct decode puts each half-bright colour directly below the colour it is
   half of.  A viewer that never built the second half draws them identical. */
{
  const d = chipDecode(M.FMT_EHB, 6, 32, (x, y) => {
    const band = ((y * 4) / CHIP_H) | 0;
    const k32 = ((x * 32) / CHIP_W) | 0;
    if (band === 0) return k32;
    if (band === 1) return 32 + k32;
    if (band === 2) return ((x * 64) / CHIP_W) | 0;
    return (y + (((x * 64) / CHIP_W) | 0)) & 63;
  });

  const y0 = 4;
  const y1 = ((CHIP_H / 4) | 0) + 4;
  let wrong = 0, same = 0;
  for (let x = 0; x < CHIP_W; x++) {
    const a = d.at(x, y0), b = d.at(x, y1);
    for (let c = 0; c < 3; c++) if (b[c] !== (a[c] >> 1)) wrong++;
    if (a[0] === b[0] && a[1] === b[1] && a[2] === b[2]) same++;
  }
  ok("EHB: 32..63 are 0..31 with every component halved", wrong === 0,
     wrong + " components differ");
  /* Black halves to black, so a few equal pixels are the palette and not a
     viewer that skipped the second half. */
  ok("EHB: the half-bright band is visibly darker", same < CHIP_W / 4,
     same + " of " + CHIP_W + " pixels identical");
}

/*
 * A chipset screen whose width is not a whole number of bytes.
 *
 * 634 wide has bytesPerRow 80, which holds 640 pixels: the last byte of every
 * row carries six that are not on the display.  A decoder that draws them
 * writes past the end of the row and into the start of the next one, and on
 * HAM it would also carry a colour across a row boundary that the hardware
 * resets.  The padded planar case above is here for the same reason and it is
 * the likeliest mistake in the file.
 */
for (const [name, format, depth, colours] of [
  ["HAM6", M.FMT_HAM6, 6, 16],
  ["HAM8", M.FMT_HAM8, 8, 64],
  ["EHB", M.FMT_EHB, 6, 32],
]) {
  const W = 634, H = 8, bpr = 80;
  const screen = { width: W, height: H, depth, bytesPerRow: bpr, format };
  const rgb = chipPalette(colours);

  /* Every byte set, padding included, so anything drawn from off the display
     is loud rather than a plausible colour. */
  const planes = new Uint8Array(bpr * H * depth).fill(0xff);
  const words = new Uint32Array(W * H).fill(0xdeadbeef);
  M.decodeInto(screen, planes, 0, M.renderPalette(screen, rgb), words);

  let untouched = 0;
  for (let i = 0; i < W * H; i++) if (words[i] === 0xdeadbeef) untouched++;
  ok(name + ": a 634-wide row draws every pixel that is on the display",
     untouched === 0, untouched + " left untouched");

  /* Row 1 must begin from what row 0 began from and not from row 0's last
     pixel, padding or no padding. */
  const b = new Uint8Array(words.buffer);
  const at = (x, y) => "" + b[(y * W + x) * 4] + "," + b[(y * W + x) * 4 + 1] +
                       "," + b[(y * W + x) * 4 + 2];
  ok(name + ": a row starts where the row above it started",
     at(0, 0) === at(0, 1), at(0, 0) + " then " + at(0, 1));
}

/* The palette lengths both ends size a buffer from, which is the one number
   that has to agree before a frame has gone either way. */
for (const [name, format, depth, want] of [
  ["HAM6", M.FMT_HAM6, 6, 16],
  ["HAM8", M.FMT_HAM8, 8, 64],
  ["EHB", M.FMT_EHB, 6, 32],
]) {
  const screen = { width: CHIP_W, height: CHIP_H, depth, bytesPerRow: 40, format };
  ok(name + ": the `pal` word carries " + want + " colours",
     M.palColours(screen) === want, "says " + M.palColours(screen));
  /* And the chipset modes keep the display-modulo aspect rule, which the RTG
     formats do not: a 320x256 HAM picture drawn square is half its height. */
  const a = M.pixelAspect(screen);
  ok(name + ": a 320x256 screen is displayed 2:2",
     a.x === 2 && a.y === 2, a.x + ":" + a.y);
}

/* ----------------------------------------------------------- the export -- */

/*
 * THE RECORDING IN A FILE SOMETHING ELSE OPENS.
 *
 * .pfs is the capture's own layout and only this page reads it, so the export
 * is the answer to "I recorded the fault, here it is" -- and an export nobody
 * can open is the bug it was written to fix.  What is checked is therefore not
 * that the encoder ran: it is that a reader which shares no line with it walks
 * the chunks, follows the sequence numbers, paints each rectangle where the
 * file says, and lands on the pixels that went in.
 *
 * LOSSLESS IS THE WHOLE POINT and it is an exact comparison for that reason.
 * A screen of flat palette entries and one-pixel bevels is the material every
 * lossy codec destroys, so "close enough" is not a grade this can be given.
 */
async function exportRoundTrip(label, cap) {
  const w = cap.screen.width;
  const h = cap.screen.height;

  const source = [];
  for (let i = 0; i < cap.frameCount; i++) {
    const rgba = new Uint8ClampedArray(w * h * 4);
    M.decodeInto(cap.screen, cap.frames, M.frameAt(cap, i), cap.palette,
                 new Uint32Array(rgba.buffer));
    source.push(rgba);
  }

  const png = new M.Apng(w, h);
  let total = 0;
  for (let i = 0; i < source.length; i++) {
    const ms = i + 1 < cap.frameCount ? cap.times[i + 1] - cap.times[i] : 40;
    total += ms;
    await png.add(source[i], ms);
  }
  const bytes = Buffer.from(png.finish());

  let got;
  try {
    got = readApng(bytes);
  } catch (e) {
    ok(label + ": the export is a readable APNG", false, e.message);
    return bytes;
  }

  ok(label + ": the export is a readable APNG",
     got.width === w && got.height === h,
     got.width + "x" + got.height);
  ok(label + ": acTL counts the frames that are in it",
     got.declared === got.frames.length && got.declared === png.frames,
     got.declared + " declared, " + got.frames.length + " present, " +
     png.frames + " written");
  ok(label + ": it rounds for ever", got.plays === 0, "plays " + got.plays);

  /* The still picture a viewer that knows nothing about APNG shows, which the
     specification requires to be the whole canvas. */
  const f0 = got.frames[0];
  ok(label + ": the first frame is the whole canvas",
     f0.x === 0 && f0.y === 0 && f0.w === w && f0.h === h,
     f0.w + "x" + f0.h + " at " + f0.x + "," + f0.y);

  let bad = -1;
  for (let i = 0; i < source.length && bad < 0; i++) {
    if (!sameRGB(got.frames[i].rgba, source[i], w * h)) bad = i;
  }
  ok(label + ": every frame comes back pixel for pixel", bad < 0,
     bad < 0 ? "" : "frame " + bad + " differs");

  const ran = got.frames.reduce((n, f) => n + f.delayMs, 0);
  ok(label + ": the frame times are the recording's",
     Math.abs(ran - total) < 1, ran + " ms against " + total);

  /* The reason a recording of a mostly-still screen is not one picture per
     frame.  A synthesised capture moves a box, so the frames after the first
     have to be smaller than the screen or the diffing is not happening. */
  const area = got.frames.slice(1).reduce((n, f) => n + f.w * f.h, 0);
  ok(label + ": frames after the first carry only what changed",
     got.frames.length < 2 || area < (got.frames.length - 1) * w * h,
     Math.round(area / Math.max(1, got.frames.length - 1)) +
     " px a frame of " + w * h);

  console.log("export %s %d frames  %d bytes  %d B/frame  %s%% of the .pfs",
              label.padEnd(8), got.frames.length, bytes.length,
              Math.round(bytes.length / got.frames.length),
              (bytes.length * 100 / (cap.frameCount * cap.stride)).toFixed(1));

  return bytes;
}

console.log("");
const apngPlanar = await exportRoundTrip("planar",
  M.parsePfs(bufferToArrayBuffer(writePfs(synth(640, 256, 3, 24)))));
await exportRoundTrip("chunky",
  M.parsePfs(bufferToArrayBuffer(writePfs(synthChunky(320, 240, 12)))));
await exportRoundTrip("rgb565",
  M.parsePfs(bufferToArrayBuffer(writePfs(synthRgb565(320, 240, 12)))));
console.log("");

/*
 * That the reader above is checking anything.  Every "comes back pixel for
 * pixel" is worth exactly what its reader's refusals are worth, and a reader
 * that accepted a corrupted chunk would pass all of them.
 */
{
  const wrecked = Buffer.from(apngPlanar);
  wrecked[wrecked.length - 20] ^= 0x01;
  ok("export: a corrupted chunk is refused, so the reader is reading",
     throws(() => readApng(wrecked)));
}

/*
 * A frame that drew nothing is not a frame.  An idle Workbench sends the same
 * picture again and again, and a file with one picture per arrival is a file
 * that is mostly repeats -- so a repeat becomes time on the frame in front of
 * it instead, and the recording still runs for as long as it ran.
 */
{
  const w = 64, h = 48;
  const flat = new Uint8ClampedArray(w * h * 4).fill(0xff);
  const png = new M.Apng(w, h);
  await png.add(flat, 100);
  await png.add(Uint8ClampedArray.from(flat), 250);
  await png.add(Uint8ClampedArray.from(flat), 650);
  const got = readApng(Buffer.from(png.finish()));

  ok("export: a frame that changed nothing is not written",
     got.frames.length === 1, got.frames.length + " frames");
  ok("export: its time goes to the frame in front of it",
     got.frames[0].delayMs === 1000, got.frames[0].delayMs + " ms");
}

/* delay_num is sixteen bits, so a frame held longer than 65.5 seconds has to
   be timed in hundredths.  An idle screen left alone over lunch is that case,
   and the alternative is a frame that flashes past in a millisecond. */
{
  const w = 16, h = 16;
  const a = new Uint8ClampedArray(w * h * 4).fill(0x11);
  const b = new Uint8ClampedArray(w * h * 4).fill(0x22);
  const png = new M.Apng(w, h);
  await png.add(a, 70000);
  await png.add(b, 40);
  const got = readApng(Buffer.from(png.finish()));
  ok("export: a frame held over a minute keeps its time",
     got.frames[0].delayMs === 70000, got.frames[0].delayMs + " ms");
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
/* The real cases first: stock Workbench 3.1 is 640x256 and two planes deep
   whatever the machine is, and the larger geometries below are headroom. */
timeDecode(640, 256, 2, "640x256x2");
timeDecode(640, 256, 4, "640x256x4");
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
  const shadow = new Uint8Array(cap.stride);
  const scratch = new Uint8Array(M.scratchBytes(cg));
  const words = new Uint32Array(w * h);
  const pal = M.palette32(cap.rgb, M.palColours(cap.screen));

  const frames = [];
  for (let t = 0; t < cap.frameCount; t++) {
    frames.push(encodeFrame(g, shadow,
                            cap.frames.subarray(t * cap.stride, (t + 1) * cap.stride),
                            t));
  }

  const runs = slow ? 400 : 120;
  let bytes = 0;
  let area = 0;

  /* The first frame is applied once and then only the deltas are timed: a
     session sends one full screen and then runs. */
  M.applyUpdate(cg, frames[0], fb, scratch);

  const t0 = process.hrtime.bigint();
  for (let i = 0; i < runs; i++) {
    const u = frames[1 + (i % (frames.length - 1))];
    const d = M.applyUpdate(cg, u, fb, scratch);
    M.decodeRectInto(cap.screen, fb, 0, pal, words, d.x0, d.y0, d.x1, d.y1);
    bytes += u.length;
    area += (d.x1 - d.x0) * (d.y1 - d.y0);
  }
  const ms = Number(process.hrtime.bigint() - t0) / 1e6 / runs;

  console.log("live  " + label.padEnd(13) +
              col(ms.toFixed(3) + " ms/frame") +
              col((1000 / ms).toFixed(0) + " fps") +
              col(Math.round(bytes / runs) + " B/frame") +
              col(Math.round(area / runs) + " px damaged"));
}

console.log("");
timeLive(640, 480, 3, 16, 8, "640x480x3");
timeLive(800, 600, 8, 32, 16, "800x600x8");

console.log("");
if (failures > 0) {
  console.log("selftest: " + failures + " checks failed");
  process.exit(1);
}
console.log("selftest: all checks passed");

/* ---------------------------------------------------------------- bits -- */

/* Alpha is not compared: the reference PNG has none and the decoder writes
   255 everywhere by construction. */
function sameRGB(got, want, pixels) {
  for (let i = 0; i < pixels; i++) {
    const o = i * 4;
    if (got[o] !== want[o] || got[o + 1] !== want[o + 1] ||
        got[o + 2] !== want[o + 2]) return false;
  }
  return true;
}

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
