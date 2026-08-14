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
 * No dependencies.  zlib and a 40-line CRC are the whole of what a PNG needs
 * and the tree vendors its dependencies deliberately.
 *
 * SPDX-License-Identifier: MIT
 */

import { deflateSync } from "node:zlib";

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

/* --------------------------------------------------------------- the file -- */

export function writePfs(cap) {
  const head = Buffer.alloc(16);
  head.write("PFS1", 0, "latin1");
  head.writeUInt16BE(cap.screen.width, 4);
  head.writeUInt16BE(cap.screen.height, 6);
  head.writeUInt8(cap.screen.depth, 8);
  head.writeUInt8(0, 9);
  head.writeUInt16BE(cap.screen.bytesPerRow, 10);
  head.writeUInt16BE(cap.frameCount, 12);
  head.writeUInt16BE(0, 14);
  return Buffer.concat([head, cap.rgb, Buffer.from(cap.frames)]);
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
    tileW,
    tileH,
    across: Math.ceil(screen.width / tileW),
    down: Math.ceil(screen.height / tileH),
    tileBytesPerRow: tileW >> 3,
    tileBytes: (tileW >> 3) * tileH,
  };
}

/*
 * An update, in the PLACEHOLDER framing documented at the top of
 * client/console/tiles.ts.  Per tile, per plane: XOR against the previous frame,
 * skip the planes that are all zero, PackBits what is left and keep it only
 * if it came out smaller.
 */
export function encodeUpdate(g, prev, next, seq, keyframe) {
  const plane = g.screen.bytesPerRow * g.screen.height;
  const bpr = g.screen.bytesPerRow;
  const rowBytes = g.tileBytesPerRow;
  const parts = [];
  let tiles = 0;

  const payload = Buffer.alloc(g.tileBytes * g.screen.depth);

  for (let ty = 0; ty < g.down; ty++) {
    for (let tx = 0; tx < g.across; tx++) {
      const px = tx * g.tileW;
      const py = ty * g.tileH;
      const rows = Math.min(g.tileH, g.screen.height - py);

      let mask = 0;
      let at = 0;

      for (let p = 0; p < g.screen.depth; p++) {
        const start = at;
        let any = false;

        for (let r = 0; r < g.tileH; r++) {
          const so = p * plane + (Math.min(py + r, g.screen.height - 1)) * bpr + (px >> 3);
          for (let i = 0; i < rowBytes; i++) {
            /* Rows past the bottom edge are padding: the payload is always a
               whole tile, which is one less special case on the 68020. */
            const v = r < rows
              ? (keyframe ? next[so + i] : (next[so + i] ^ prev[so + i]))
              : 0;
            payload[at + r * rowBytes + i] = v;
            if (v !== 0) any = true;
          }
        }

        /* A keyframe carries every plane whether or not it has a bit set: it
           REPLACES rather than XORs, and a plane left out would be whatever
           was there before. */
        if (any || keyframe) {
          mask |= 1 << p;
          at = start + g.tileBytes;
        }
        /* else: leave `at` where it was and the next plane overwrites it */
      }

      if (mask === 0) continue;

      const raw = payload.subarray(0, at);
      const packed = packBits(raw);
      const use = packed.length < raw.length ? packed : raw;
      const coding = packed.length < raw.length ? 1 : 0;

      const head = Buffer.alloc(6);
      head.writeUInt16BE(ty * g.across + tx, 0);
      head.writeUInt8(mask, 2);
      head.writeUInt8(coding, 3);
      head.writeUInt16BE(use.length, 4);
      parts.push(head, Buffer.from(use));
      tiles++;
    }
  }

  const head = Buffer.alloc(6);
  head.writeUInt8(1, 0);
  head.writeUInt8(keyframe ? 1 : 0, 1);
  head.writeUInt16BE(seq & 0xffff, 2);
  head.writeUInt16BE(tiles, 4);

  return Buffer.concat([head, ...parts]);
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
