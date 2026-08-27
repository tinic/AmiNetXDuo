/*
 * The host half of the framebuffer prototype: everything the browser is not.
 *
 *   - a synthesiser, because the capture side is being written on another
 *     branch and the viewer could not wait for it.  What it draws is chosen
 *     to be checkable rather than pretty: a band of every palette index
 *     across the bottom is what turns "the picture looks right" into an
 *     assertion, since band k must come out as palette entry k and a planar
 *     unpack that has the bit order backwards fails it on the first frame.
 *
 *   - .pfs read and write, the format the player opens.
 *
 *   - a PackBits encoder and the PLACEHOLDER tile framing, so the live half
 *     had something to be built against.  Both sides of that framing are
 *     provisional and the browser's side is client/console/tiles.ts; when the
 *     encoder branch lands, these two change together and nothing else does.
 *
 *   - a PNG writer, because a viewer that draws garbage typechecks fine.  The
 *     selftest decodes with the browser's own module and writes the result
 *     out as a file somebody can look at.
 *
 *   - an APNG reader, which is the same argument about the export: the page
 *     writes one so that other software can open a recording, and the only
 *     way to know it did is to open it with something that is not the writer.
 *
 * No dependencies.  zlib and a 40-line CRC are the whole of what a PNG needs
 * and the tree vendors its dependencies deliberately.
 *
 * SPDX-License-Identifier: MIT
 */

import { deflateSync, inflateSync } from "node:zlib";

/* --------------------------------------------------------- the synthesis -- */

/* Workbench 3.1's first eight pens, then a ramp that makes an index visible
   as a colour: entry k is (k, k*5, k*13) mod 256, so a wrong index is a
   wrong colour rather than a nearby shade of the right one. */
export function palette(depth) {
  const n = 1 << depth;
  const rgb = Buffer.alloc(n * 3);
  const wb = [
    [0xaa, 0xaa, 0xaa], [0x00, 0x00, 0x00], [0xff, 0xff, 0xff],
    [0x66, 0x88, 0xbb], [0xee, 0x44, 0x44], [0x55, 0xdd, 0x55],
    [0x00, 0x44, 0xdd], [0xee, 0x99, 0x00],
  ];
  for (let i = 0; i < n; i++) {
    const c = i < 8 && n >= 8 ? wb[i] : [i & 0xff, (i * 5) & 0xff, (i * 13) & 0xff];
    rgb[i * 3] = c[0];
    rgb[i * 3 + 1] = c[1];
    rgb[i * 3 + 2] = c[2];
  }
  return rgb;
}

/* A chunky index buffer, one byte a pixel, which is not a format anything
   uses -- it is just the easiest thing to draw into before it is sliced. */
function chunky(w, h) {
  return { w, h, px: new Uint8Array(w * h) };
}

function box(c, x, y, bw, bh, idx) {
  for (let j = Math.max(0, y); j < Math.min(c.h, y + bh); j++) {
    const row = j * c.w;
    for (let i = Math.max(0, x); i < Math.min(c.w, x + bw); i++) c.px[row + i] = idx;
  }
}

function frameRect(c, x, y, bw, bh, top, bottom) {
  for (let i = 0; i < bw; i++) {
    if (y >= 0 && y < c.h && x + i < c.w) c.px[y * c.w + x + i] = top;
    if (y + bh - 1 < c.h && x + i < c.w) c.px[(y + bh - 1) * c.w + x + i] = bottom;
  }
  for (let j = 0; j < bh; j++) {
    if (y + j < c.h && x < c.w) c.px[(y + j) * c.w + x] = top;
    if (y + j < c.h && x + bw - 1 < c.w) c.px[(y + j) * c.w + x + bw - 1] = bottom;
  }
}

/*
 * One frame of something Workbench-shaped: grey desktop, a title bar, two
 * bevelled windows, a dotted pattern that gives the planes something other
 * than solid runs to carry, and a band of every palette index along the
 * bottom.  `t` moves one window, which is what makes consecutive frames
 * differ in a few tiles rather than everywhere.
 */
export function drawFrame(w, h, depth, t) {
  const n = 1 << depth;
  const c = chunky(w, h);

  box(c, 0, 0, w, h, 0);

  /* Screen title bar. */
  box(c, 0, 0, w, 11, 0);
  for (let x = 0; x < w; x++) c.px[10 * c.w + x] = 1;
  for (let x = 4; x < 160 && x < w; x += 8) box(c, x, 3, 4, 5, 1);

  /* A window, bevelled the Workbench way: white up and left, black down and
     right, one pixel each. */
  const wx = 40, wy = 30, ww = Math.min(300, w - 60), wh = Math.min(150, h - 60);
  box(c, wx, wy, ww, wh, 2);
  frameRect(c, wx, wy, ww, wh, 2, 1);
  box(c, wx + 1, wy + 1, ww - 2, 10, 3);
  for (let y = wy + 16; y < wy + wh - 4; y += 10) {
    for (let x = wx + 6; x < wx + ww - 6; x += 3) {
      if (((x + y) & 7) < 3) c.px[y * c.w + x] = 1;
    }
  }

  /* The one that moves. */
  const mx = 60 + ((t * 7) % Math.max(1, w - 200));
  const my = h - 120 + ((t * 3) % 40);
  box(c, mx, my, 90, 50, 0);
  frameRect(c, mx, my, 90, 50, 2, 1);
  box(c, mx + 1, my + 1, 88, 9, 3);
  box(c, mx + 10, my + 20, 30, 20, 4 % n);

  /* Every index, in order, across the bottom.  This is the assertion. */
  const bandH = Math.min(24, Math.floor(h / 8));
  const bandW = Math.max(1, Math.floor(w / n));
  for (let i = 0; i < n; i++) {
    box(c, i * bandW, h - bandH, bandW, bandH, i);
  }

  return c.px;
}

export function bandGeometry(w, h, depth) {
  const n = 1 << depth;
  return {
    n,
    bandH: Math.min(24, Math.floor(h / 8)),
    bandW: Math.max(1, Math.floor(w / n)),
  };
}

/* Chunky to plane-major, which is the layout everything downstream expects. */
export function toPlanes(px, w, h, depth, bytesPerRow) {
  const plane = bytesPerRow * h;
  const out = new Uint8Array(plane * depth);

  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const v = px[y * w + x];
      if (v === 0) continue;
      const at = y * bytesPerRow + (x >> 3);
      const bit = 0x80 >> (x & 7);
      for (let p = 0; p < depth; p++) {
        if (v & (1 << p)) out[p * plane + at] |= bit;
      }
    }
  }

  return out;
}

export function wordAligned(w) {
  return ((w + 15) >> 4) * 2;
}

/* The same pixels laid out the way an RTG card holds them: one byte a pixel,
   rows bytesPerRow apart, and nothing to unpack.  drawFrame() already speaks
   in indices, so this is the padding and the stride and no more. */
export function toChunky(px, w, h, bytesPerRow) {
  const out = new Uint8Array(bytesPerRow * h);
  for (let y = 0; y < h; y++) out.set(px.subarray(y * w, y * w + w), y * bytesPerRow);
  return out;
}

export function synth(w, h, depth, frames, bytesPerRow) {
  const bpr = bytesPerRow ?? wordAligned(w);
  const stride = bpr * h * depth;
  const out = new Uint8Array(stride * frames);

  for (let t = 0; t < frames; t++) {
    out.set(toPlanes(drawFrame(w, h, depth, t), w, h, depth, bpr), t * stride);
  }

  return {
    screen: { width: w, height: h, depth, bytesPerRow: bpr },
    rgb: palette(depth),
    frames: out,
    frameCount: frames,
    stride,
  };
}

/*
 * The 8-bit chunky sibling of synth(), which is what an RTG screen is.
 *
 * The same drawing, the same palette and the same frame count, so a chunky
 * arm and a planar arm of any test are showing the same picture in two
 * layouts -- which is what makes "the chunky decode is right" a comparison
 * and not an assertion about a picture nobody has seen.  Depth is 8 because
 * that is what sizes the palette; there is one plane.
 */
export function synthChunky(w, h, frames, bytesPerRow) {
  const bpr = bytesPerRow ?? w;
  const stride = bpr * h;
  const out = new Uint8Array(stride * frames);

  for (let t = 0; t < frames; t++) {
    out.set(toChunky(drawFrame(w, h, 8, t), w, h, bpr), t * stride);
  }

  return {
    screen: { width: w, height: h, depth: 8, bytesPerRow: bpr, chunky: true },
    rgb: palette(8),
    frames: out,
    frameCount: frames,
    stride,
  };
}

/* --------------------------------------------------------- truecolour -- */

/*
 * The colours a truecolour capture can actually show.
 *
 * A synthesised RGB565 frame is drawFrame's indices put through the palette
 * and then through the format, so five bits of red and blue and six of green
 * are what survive.  A reference built straight from the palette would differ
 * from a correct decode in the low bits of nearly every pixel, so the
 * reference is built from this instead: the same palette, through the same
 * loss.
 */
export function quantise565(rgb) {
  const out = Buffer.alloc(rgb.length);
  for (let i = 0; i < rgb.length; i += 3) {
    const r = rgb[i] >> 3, g = rgb[i + 1] >> 2, b = rgb[i + 2] >> 3;
    out[i]     = (r << 3) | (r >> 2);
    out[i + 1] = (g << 2) | (g >> 4);
    out[i + 2] = (b << 3) | (b >> 2);
  }
  return out;
}

/* Two bytes a pixel, big-endian, at the row stride, and the padding past the
   width left as it was allocated.  That is what a card's framebuffer looks
   like once the Amiga has converted it, and the encoder codes every byte of
   it, the padding included. */
export function toRgb565(px, w, h, bytesPerRow, rgb) {
  const out = new Uint8Array(bytesPerRow * h);
  for (let y = 0; y < h; y++) {
    let o = y * bytesPerRow;
    for (let x = 0; x < w; x++) {
      const i = px[y * w + x] * 3;
      const v = ((rgb[i] >> 3) << 11) | ((rgb[i + 1] >> 2) << 5) |
                (rgb[i + 2] >> 3);
      out[o++] = v >> 8;
      out[o++] = v & 0xff;
    }
  }
  return out;
}

export function synthRgb565(w, h, frames, bytesPerRow) {
  /* Rounded up to a longword, which is what the server does with the width
     the card gives it. */
  const bpr = bytesPerRow ?? ((w * 2 + 3) & ~3);
  const stride = bpr * h;
  const out = new Uint8Array(stride * frames);
  const rgb = palette(8);

  for (let t = 0; t < frames; t++) {
    out.set(toRgb565(drawFrame(w, h, 8, t), w, h, bpr, rgb), t * stride);
  }

  return {
    /* depth 16 is bits a pixel and sizes nothing: there is no palette here,
       and rgb is empty rather than absent so a writer can concatenate it
       without asking what the format is. */
    screen: { width: w, height: h, depth: 16, bytesPerRow: bpr, format: 2 },
    rgb: Buffer.alloc(0),
    frames: out,
    frameCount: frames,
    stride,
  };
}

/* --------------------------------------------------------------- the file -- */

/* The cadence a synthesised capture claims.  It was not recorded, so there is
   no real timing to carry; 40 ms is what wbgrab's DELAY 2 produces and is what
   makes a synthetic file play at the speed a lab capture does. */
const SYNTH_FRAME_MS = 40;

export function writePfs(cap) {
  const head = Buffer.alloc(16);
  head.write("PFS2", 0, "latin1");
  head.writeUInt16BE(cap.screen.width, 4);
  head.writeUInt16BE(cap.screen.height, 6);
  head.writeUInt8(cap.screen.depth, 8);
  /* Byte 9 is rfb_geom.format.  It was documented as flags and only ever
     written as 0 or 1, and those two still mean planar and one eight-bit
     plane, so every file written before this reads as the capture it is. */
  head.writeUInt8(cap.screen.format ?? (cap.screen.chunky ? 1 : 0), 9);
  head.writeUInt16BE(cap.screen.bytesPerRow, 10);
  head.writeUInt16BE(cap.frameCount, 12);
  head.writeUInt16BE(0, 14);          /* no pointer images */

  /* Twelve bytes a frame: when, where the pointer was, which image it was.
     A synthesised capture has no pointer, so everything past the time is 0. */
  const recs = Buffer.alloc(cap.frameCount * 12);
  for (let i = 0; i < cap.frameCount; i++) {
    recs.writeUInt32BE(i * SYNTH_FRAME_MS, i * 12);
  }

  return Buffer.concat([head, cap.rgb, Buffer.from(cap.frames), recs]);
}

/* ---------------------------------------------------------- the placeholder -- */

/*
 * PackBits, the Amiga's own run coder -- cmpByteRun1, what a compressed IFF
 * ILBM is made of.  Chosen because a 68020 encoding with it is running
 * something it has shipped since 1985, so whatever the encoder branch settles
 * on for the framing, this part of it is a fair bet.
 */
export function packBits(src) {
  const out = [];
  let i = 0;

  while (i < src.length) {
    /* A run is worth coding at three, not two: two identical bytes cost two
       bytes literal and two bytes as a run, and a run breaks a literal that
       could have continued. */
    let run = 1;
    while (i + run < src.length && src[i + run] === src[i] && run < 128) run++;

    if (run >= 3) {
      out.push(257 - run, src[i]);
      i += run;
      continue;
    }

    let lit = 0;
    while (i + lit < src.length && lit < 128) {
      let same = 1;
      while (i + lit + same < src.length &&
             src[i + lit + same] === src[i + lit] && same < 3) same++;
      if (same >= 3) break;
      lit++;
    }
    out.push(lit - 1);
    for (let k = 0; k < lit; k++) out.push(src[i + k]);
    i += lit;
  }

  return Buffer.from(out);
}

export function makeGeometry(screen, tileW, tileH) {
  return {
    screen,
    tileW,                       /* BYTES, matching rfb_geom.tile_w */
    tileH,
    across: Math.ceil(screen.bytesPerRow / tileW),
    down: Math.ceil(screen.height / tileH),
  };
}

const OP_END = 0x00;
const OP_COPY = 0x01;
const OP_TILE = 0x02;
const OP_TILE8 = 0x03;

const CODE_RAW = 0;
const CODE_PB_RAW = 1;
const CODE_PB_XOR = 2;

/*
 * One frame in the encoder's wire format, as documented in
 * include/aminetxduo/rfb_encode.h on branch proto/rfb-encoder.
 *
 * This is NOT that encoder.  It is a second implementation of the same
 * format, written from the header, and it exists so the browser's decoder can
 * be exercised without a 68020 in the room -- and so that a disagreement
 * between the two shows up here rather than as a smeared Workbench.  The C
 * encoder is the one that ships; anything this does that it does not is a bug
 * in the mock and not in the format.
 *
 * `shadow` is the previous frame and is updated in place to be `next`.
 * `copy` is an optional { x0, w, y0, h, dy } in byte columns and rows, which
 * is how the COPY op gets exercised: the real encoder finds these with a
 * scroll detector and the caller here just says so.
 */
export function encodeFrame(g, shadow, next, seq, copy) {
  const bpr = g.screen.bytesPerRow;
  const h = g.screen.height;
  /* Planes, not depth: both card formats are one plane of bytes whatever
     their depth says, and every loop here wants the second number.  Which of
     the two it is does not reach this function at all -- a tile is a
     rectangle of bytes either way. */
  const fmt = g.screen.format ?? (g.screen.chunky === true ? 1 : 0);
  const chunky = fmt === 1 || fmt === 2;
  const depth = chunky ? 1 : g.screen.depth;
  const plane = bpr * h;

  const head = Buffer.alloc(4);
  head.writeUInt8(1, 0);
  head.writeUInt8(0, 1);
  head.writeUInt16BE(seq & 0xffff, 2);
  const parts = [head];

  if (copy !== undefined) {
    const c = Buffer.alloc(11);
    c.writeUInt8(OP_COPY, 0);
    c.writeUInt16BE(copy.x0, 1);
    c.writeUInt16BE(copy.w, 3);
    c.writeUInt16BE(copy.y0, 5);
    c.writeUInt16BE(copy.h, 7);
    c.writeInt16BE(copy.dy, 9);
    parts.push(c);

    /* Applied to the shadow here, so the tiles below are coded against what
       the receiver will actually have after it runs the same op. */
    for (let p = 0; p < depth; p++) {
      const base = p * plane;
      if (copy.dy > 0) {
        for (let r = 0; r < copy.h; r++) {
          const to = base + (copy.y0 + r) * bpr + copy.x0;
          const from = base + (copy.y0 + r + copy.dy) * bpr + copy.x0;
          shadow.copyWithin(to, from, from + copy.w);
        }
      } else {
        for (let r = copy.h; r > 0; r--) {
          const to = base + (copy.y0 + r - 1) * bpr + copy.x0;
          const from = base + (copy.y0 + r - 1 + copy.dy) * bpr + copy.x0;
          shadow.copyWithin(to, from, from + copy.w);
        }
      }
    }
  }

  const raw = Buffer.alloc(g.tileW * g.tileH);
  const xor = Buffer.alloc(g.tileW * g.tileH);
  let tiles = 0;

  for (let ty = 0; ty < g.down; ty++) {
    for (let tx = 0; tx < g.across; tx++) {
      const x0 = tx * g.tileW;
      const y0 = ty * g.tileH;
      const tw = Math.min(g.tileW, bpr - x0);
      const th = Math.min(g.tileH, h - y0);
      const want = tw * th;

      let mask = 0;
      const bodies = [];

      for (let p = 0; p < depth; p++) {
        const base = p * plane;
        let differs = false;

        for (let r = 0; r < th; r++) {
          const at = base + (y0 + r) * bpr + x0;
          for (let c = 0; c < tw; c++) {
            const v = next[at + c];
            const o = shadow[at + c];
            raw[r * tw + c] = v;
            xor[r * tw + c] = v ^ o;
            if (v !== o) differs = true;
          }
        }
        if (!differs) continue;

        mask |= 1 << p;

        const pbRaw = packBits(raw.subarray(0, want));
        const pbXor = packBits(xor.subarray(0, want));

        /* Smallest of the three, counting the two length bytes the packed
           forms carry and the raw form does not. */
        let best = Buffer.concat([Buffer.from([CODE_RAW]), raw.subarray(0, want)]);
        if (pbRaw.length + 3 < best.length) {
          best = Buffer.concat([Buffer.from([CODE_PB_RAW]), u16be(pbRaw.length), pbRaw]);
        }
        if (pbXor.length + 3 < best.length) {
          best = Buffer.concat([Buffer.from([CODE_PB_XOR]), u16be(pbXor.length), pbXor]);
        }
        bodies.push(best);

        for (let r = 0; r < th; r++) {
          const at = base + (y0 + r) * bpr + x0;
          for (let c = 0; c < tw; c++) shadow[at + c] = next[at + c];
        }
      }

      if (mask === 0) continue;

      /* One plane means one possible mask, so the chunky op does not carry
         one.  Same index, same codes, same payloads. */
      const th3 = Buffer.alloc(chunky ? 3 : 4);
      th3.writeUInt8(chunky ? OP_TILE8 : OP_TILE, 0);
      th3.writeUInt16BE(ty * g.across + tx, 1);
      if (!chunky) th3.writeUInt8(mask, 3);
      parts.push(th3, ...bodies);
      tiles++;
    }
  }

  parts.push(Buffer.from([OP_END]));
  const out = Buffer.concat(parts);
  out.tiles = tiles;
  return out;
}

function u16be(n) {
  const b = Buffer.alloc(2);
  b.writeUInt16BE(n);
  return b;
}

/* ---------------------------------------------------------------- the PNG -- */

const CRC = (() => {
  const t = new Int32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c;
  }
  return t;
})();

function crc32(buf) {
  let c = ~0;
  for (let i = 0; i < buf.length; i++) c = CRC[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
  return ~c >>> 0;
}

function chunk(type, data) {
  const len = Buffer.alloc(4);
  len.writeUInt32BE(data.length);
  const body = Buffer.concat([Buffer.from(type, "latin1"), data]);
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(body));
  return Buffer.concat([len, body, crc]);
}

/* RGBA in, an 8-bit truecolour-with-alpha PNG out.  Filter 0 on every row:
   the file is looked at once and deflate is doing the work. */
export function writePng(rgba, w, h) {
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0);
  ihdr.writeUInt32BE(h, 4);
  ihdr[8] = 8;
  ihdr[9] = 6;

  const raw = Buffer.alloc(h * (1 + w * 4));
  for (let y = 0; y < h; y++) {
    raw[y * (1 + w * 4)] = 0;
    Buffer.from(rgba.buffer, rgba.byteOffset + y * w * 4, w * 4)
      .copy(raw, y * (1 + w * 4) + 1);
  }

  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk("IHDR", ihdr),
    chunk("IDAT", deflateSync(raw, { level: 6 })),
    chunk("IEND", Buffer.alloc(0)),
  ]);
}

/*
 * PNG in, RGBA out.  Colour type 2 at 8 bits, which is what the capture
 * side's decoder writes, and the four filters a PNG encoder actually uses.
 *
 * Here so the viewer can be checked against a picture produced by an
 * implementation that shares no code with it: the .png files beside the real
 * captures are ground truth, and a comparison against them is worth more
 * than any number of round trips through my own encoder.
 */
export function readPng(buf) {
  if (buf.length < 8 || buf.readUInt32BE(0) !== 0x89504e47) {
    throw new Error("not a PNG");
  }

  let at = 8;
  let w = 0, h = 0, bits = 0, type = 0;
  const idat = [];

  while (at + 8 <= buf.length) {
    const len = buf.readUInt32BE(at);
    const kind = buf.toString("latin1", at + 4, at + 8);
    const body = buf.subarray(at + 8, at + 8 + len);
    at += 12 + len;

    if (kind === "IHDR") {
      w = body.readUInt32BE(0);
      h = body.readUInt32BE(4);
      bits = body[8];
      type = body[9];
      if (bits !== 8 || type !== 2) {
        throw new Error("PNG is " + bits + "-bit type " + type +
                        ", this reads 8-bit truecolour only");
      }
      if (body[12] !== 0) throw new Error("an interlaced PNG");
    } else if (kind === "IDAT") {
      idat.push(body);
    } else if (kind === "IEND") {
      break;
    }
  }

  const raw = inflateSync(Buffer.concat(idat));
  const out = Buffer.alloc(w * h * 4);
  const stride = w * 3;
  let prev = Buffer.alloc(stride);

  for (let y = 0; y < h; y++) {
    const filter = raw[y * (stride + 1)];
    const line = Buffer.from(raw.subarray(y * (stride + 1) + 1,
                                          y * (stride + 1) + 1 + stride));

    for (let i = 0; i < stride; i++) {
      const a = i >= 3 ? line[i - 3] : 0;
      const b = prev[i];
      const c = i >= 3 ? prev[i - 3] : 0;
      if (filter === 1) line[i] = (line[i] + a) & 0xff;
      else if (filter === 2) line[i] = (line[i] + b) & 0xff;
      else if (filter === 3) line[i] = (line[i] + ((a + b) >> 1)) & 0xff;
      else if (filter === 4) line[i] = (line[i] + paeth(a, b, c)) & 0xff;
      else if (filter !== 0) throw new Error("PNG filter " + filter);
    }

    for (let x = 0; x < w; x++) {
      out[(y * w + x) * 4] = line[x * 3];
      out[(y * w + x) * 4 + 1] = line[x * 3 + 1];
      out[(y * w + x) * 4 + 2] = line[x * 3 + 2];
      out[(y * w + x) * 4 + 3] = 255;
    }
    prev = line;
  }

  return { width: w, height: h, rgba: out };
}

/*
 * APNG in, every frame composited, out.
 *
 * A reference reader, and it shares no line with the encoder it checks:
 * client/console/apng.ts writes rectangles, chooses a filter per row and
 * folds a repeated frame into its predecessor's delay, and every one of those
 * is a way to produce a file that is structurally a PNG and shows the wrong
 * picture.  This walks the chunks the way the specification says a decoder
 * does -- CRC every one, follow the sequence numbers, paint each frame into
 * its own rectangle over what was already there -- so a comparison against
 * the frames that went in is a comparison against the format and not against
 * my own idea of it.
 *
 * Only what the encoder emits is accepted: 8-bit truecolour, dispose NONE,
 * blend SOURCE.  A file with anything else in it is a file this was not
 * written to read, and saying so is more use than drawing it wrong.
 */
export function readApng(buf) {
  if (buf.length < 8 || buf.readUInt32BE(0) !== 0x89504e47) {
    throw new Error("not a PNG");
  }

  let at = 8;
  let width = 0, height = 0;
  let declared = -1, plays = -1;
  const seqs = [];
  const kinds = [];
  const frames = [];
  let cur = null;
  let ended = false;

  while (at + 8 <= buf.length) {
    const len = buf.readUInt32BE(at);
    const kind = buf.toString("latin1", at + 4, at + 8);
    if (at + 12 + len > buf.length) {
      throw new Error("chunk " + kind + " runs off the end");
    }
    const body = buf.subarray(at + 8, at + 8 + len);
    const want = buf.readUInt32BE(at + 8 + len);
    const got = crc32(buf.subarray(at + 4, at + 8 + len));
    if (want !== got) throw new Error("chunk " + kind + " has a bad CRC");
    at += 12 + len;
    kinds.push(kind);

    if (kind === "IHDR") {
      width = body.readUInt32BE(0);
      height = body.readUInt32BE(4);
      if (body[8] !== 8 || body[9] !== 2) {
        throw new Error("APNG is " + body[8] + "-bit type " + body[9] +
                        ", this reads 8-bit truecolour only");
      }
      if (body[12] !== 0) throw new Error("an interlaced APNG");
    } else if (kind === "acTL") {
      declared = body.readUInt32BE(0);
      plays = body.readUInt32BE(4);
    } else if (kind === "fcTL") {
      seqs.push(body.readUInt32BE(0));
      cur = {
        w: body.readUInt32BE(4), h: body.readUInt32BE(8),
        x: body.readUInt32BE(12), y: body.readUInt32BE(16),
        delayMs: body.readUInt16BE(20) * 1000 / body.readUInt16BE(22),
        dispose: body[24], blend: body[25],
        parts: [],
      };
      if (cur.dispose !== 0 || cur.blend !== 0) {
        throw new Error("frame " + frames.length + " disposes " + cur.dispose +
                        " blends " + cur.blend);
      }
      if (cur.x + cur.w > width || cur.y + cur.h > height) {
        throw new Error("frame " + frames.length + " is outside the canvas");
      }
      frames.push(cur);
    } else if (kind === "IDAT" || kind === "fdAT") {
      if (cur === null) throw new Error("an " + kind + " before any fcTL");
      if (kind === "fdAT") {
        seqs.push(body.readUInt32BE(0));
        cur.parts.push(body.subarray(4));
      } else {
        cur.parts.push(body);
      }
    } else if (kind === "IEND") {
      ended = true;
      break;
    }
  }

  if (!ended) throw new Error("no IEND");
  if (frames.length === 0) throw new Error("no frames");
  for (let i = 0; i < seqs.length; i++) {
    if (seqs[i] !== i) {
      throw new Error("sequence number " + seqs[i] + " where " + i +
                      " was due");
    }
  }

  /* Composited into one canvas that persists across frames, which is what
     dispose NONE means and what makes a rectangle a frame at all. */
  const canvas = Buffer.alloc(width * height * 4, 0xff);
  const out = [];

  for (const f of frames) {
    const rows = unfilter(inflateSync(Buffer.concat(f.parts)), f.w, f.h, 3);
    for (let y = 0; y < f.h; y++) {
      for (let x = 0; x < f.w; x++) {
        const s = y * f.w * 3 + x * 3;
        const d = ((f.y + y) * width + f.x + x) * 4;
        canvas[d] = rows[s];
        canvas[d + 1] = rows[s + 1];
        canvas[d + 2] = rows[s + 2];
        canvas[d + 3] = 255;
      }
    }
    out.push({ x: f.x, y: f.y, w: f.w, h: f.h, delayMs: f.delayMs,
               rgba: Buffer.from(canvas) });
  }

  return { width, height, declared, plays, kinds, frames: out };
}

/* PNG's five row filters undone, in place, for a bpp-byte pixel. */
function unfilter(raw, w, h, bpp) {
  const stride = w * bpp;
  if (raw.length !== h * (stride + 1)) {
    throw new Error("a frame inflated to " + raw.length + " bytes, " + w + "x" +
                    h + " needs " + h * (stride + 1));
  }

  const out = Buffer.alloc(h * stride);
  for (let y = 0; y < h; y++) {
    const filter = raw[y * (stride + 1)];
    const line = out.subarray(y * stride, (y + 1) * stride);
    raw.copy(line, 0, y * (stride + 1) + 1, y * (stride + 1) + 1 + stride);
    const prev = y === 0 ? null : out.subarray((y - 1) * stride, y * stride);

    for (let i = 0; i < stride; i++) {
      const a = i >= bpp ? line[i - bpp] : 0;
      const b = prev === null ? 0 : prev[i];
      const c = prev === null || i < bpp ? 0 : prev[i - bpp];
      if (filter === 1) line[i] = (line[i] + a) & 0xff;
      else if (filter === 2) line[i] = (line[i] + b) & 0xff;
      else if (filter === 3) line[i] = (line[i] + ((a + b) >> 1)) & 0xff;
      else if (filter === 4) line[i] = (line[i] + paeth(a, b, c)) & 0xff;
      else if (filter !== 0) throw new Error("PNG filter " + filter);
    }
  }
  return out;
}

function paeth(a, b, c) {
  const p = a + b - c;
  const pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c);
  return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}
