/*
 * The receiver's side of the frame encoder's wire format.
 *
 * The format is the encoder's and is written down in its header, not here:
 * include/aminetxduo/rfb_encode.h on branch proto/rfb-encoder, whose
 * rfbbench.c carries the C decoder this one has to agree with byte for byte.
 * What follows is the shape, so that reading the code does not require having
 * the other file open:
 *
 *   u8  version = 1
 *   u8  flags
 *   u16 seq                     wraps
 *   then ops in stream order until OP_END
 *
 *   OP_END  0x00
 *   OP_COPY 0x01  u16 x0, u16 w   byte columns
 *                 u16 y0, u16 h   rows
 *                 s16 dy          source row = destination row + dy
 *                 In EVERY plane.  Reads the previous frame, which is what
 *                 the buffer still holds because copies precede the tiles.
 *   OP_TILE 0x02  u16 index, u8 planes, then per set plane from 0 up:
 *                 u8 code  0 RAW     tw*th bytes
 *                          1 PB_RAW  u16 len, PackBits of the same
 *                          2 PB_XOR  u16 len, PackBits of it XORed with what
 *                                    is already there
 *   OP_TILE8 0x03 u16 index, then ONE plane's u8 code and payload, exactly as
 *                 above.  The RTG sibling of OP_TILE: an eight-bit or a
 *                 sixteen-bit source has one plane, so the mask would say 1
 *                 on every tile of every frame and is not sent.  Which of the
 *                 two arrives is decided by FORMAT in the `geom` word and not
 *                 by the op, so an op that disagrees with the geometry is an
 *                 error here rather than a picture drawn eight times too wide.
 *
 * tile_w is in BYTES and not pixels, and the tile grid is over bytesPerRow
 * rather than the width: every byte of a row is encoded, padding included, so
 * a decoded frame is byte-identical to the BitMap that went in.  Getting that
 * wrong is a picture that is right until the screen is not a whole number of
 * tiles wide.  A byte is eight pixels planar, one chunky and half of one at
 * sixteen bits, which is the only other place the formats part company: the
 * PackBits, the XOR and the copies are byte operations and read the same
 * whichever it is.
 *
 * This is the only file that reads a byte off the socket.  It was written
 * against a placeholder framing for as long as the encoder did not exist, and
 * replacing that with this changed nothing outside it.
 *
 * SPDX-License-Identifier: MIT
 */

import {
  FMT_CLUT8,
  FMT_EHB,
  FMT_HAM6,
  FMT_HAM8,
  FMT_PLANAR,
  FMT_RGB565,
  isChunky,
  isHam,
  planeBytes,
  planeCount,
  pixelsPerByte,
  screenFault,
  screenFormat,
  type Screen,
} from "./planar";

/* rfb_geom.format, as the `geom` word carries it.  They belong to a Screen
   and are declared with one, and this is where everything else reads them. */
export { FMT_CLUT8, FMT_EHB, FMT_HAM6, FMT_HAM8, FMT_PLANAR, FMT_RGB565 };

export const RFB_VERSION = 1;

const OP_END = 0x00;
const OP_COPY = 0x01;
const OP_TILE = 0x02;
const OP_TILE8 = 0x03;

/* For the one message that has to name a format rather than test one. */
const FMT_NAME = ["planar", "chunky", "16-bit", "HAM6", "HAM8", "EHB"];

const CODE_RAW = 0;
const CODE_PB_RAW = 1;
const CODE_PB_XOR = 2;

export interface Geometry {
  readonly screen: Screen;
  /* Bytes, matching rfb_geom.tile_w. */
  readonly tileW: number;
  readonly tileH: number;
  readonly across: number;
  readonly down: number;
}

export interface Damage {
  /* Screen pixels.  Empty when x1 <= x0. */
  x0: number;
  y0: number;
  x1: number;
  y1: number;
  tiles: number;
  copies: number;
  bytes: number;
  seq: number;
}

export function makeGeometry(
  screen: Screen, tileW: number, tileH: number,
): Geometry {
  const fault = screenFault(screen);
  if (fault !== null) throw new Error("the geometry says " + fault);
  if (!Number.isInteger(tileW) || tileW < 1 || tileW > 64) {
    throw new Error("tile width " + tileW + " bytes, which is not 1..64");
  }
  if (!Number.isInteger(tileH) || tileH < 1 || tileH > 64) {
    throw new Error("tile height " + tileH + " rows, which is not 1..64");
  }

  return {
    screen,
    tileW,
    tileH,
    /* Over bytesPerRow, not width: the encoder codes the padding too. */
    across: Math.ceil(screen.bytesPerRow / tileW),
    down: Math.ceil(screen.height / tileH),
  };
}

/*
 * `geom W H DEPTH BYTESPERROW TILEWBYTES TILEH FORMAT`, the control word that
 * opens a session.  Text rather than a binary header because it is control,
 * and the split between the two channels is the whole convention: binary
 * frames are the data stream and text frames are words.
 */
export function geometryFromWord(w: string): Geometry {
  const f = w.trim().split(/\s+/);
  if (f[0] !== "geom" || f.length !== 8) {
    throw new Error("geom takes seven numbers, got: " + w);
  }
  const n = f.slice(1).map(Number);
  if (n.some((x) => !Number.isInteger(x))) {
    throw new Error("geom has something that is not a whole number: " + w);
  }
  if (n[6] < FMT_PLANAR || n[6] > FMT_EHB) {
    throw new Error("geom format " + n[6] + " is not one this viewer draws");
  }
  return makeGeometry(
    {
      width: n[0],
      height: n[1],
      /* Planes on FMT_PLANAR and on all three chipset modes, what sizes the
         palette on FMT_CLUT8, and bits per pixel on FMT_RGB565.  screenFault()
         knows which, and which depths each one is allowed. */
      depth: n[2],
      bytesPerRow: n[3],
      format: n[6],
      chunky: n[6] === FMT_CLUT8,
    },
    n[4], n[5],
  );
}

/*
 * `pal RRGGBB...`, hex, one triple per entry.  1536 characters at 256
 * colours, which is a text frame and not a problem; it is sent once per
 * session and again whenever LoadRGB4 moves something.
 *
 * `colours` is entries and not a depth, because the two are the same number
 * on one format only; palColours() in planar.ts is what answers it.
 */
export function paletteFromWord(w: string, colours: number): Uint8Array {
  const hex = w.trim().slice(4).replace(/\s+/g, "");
  const want = 3 * colours;
  if (hex.length !== want * 2) {
    throw new Error("pal is " + hex.length / 2 + " bytes, " + colours +
                    " colours need " + want);
  }
  const out = new Uint8Array(want);
  for (let i = 0; i < want; i++) {
    const v = parseInt(hex.substr(i * 2, 2), 16);
    if (Number.isNaN(v)) throw new Error("pal is not hex at byte " + i);
    out[i] = v;
  }
  return out;
}

/*
 * PackBits, the IFF ILBM byte RLE:
 *
 *   n in 0..127     the next n+1 bytes are literal
 *   n in 129..255   the next byte repeated 257-n times
 *   n == 128        no operation, and the encoder never emits one
 *
 * A tile is one continuous stream across its rows and not one run per row,
 * so this decodes tw*th bytes in a single call.  Into a caller-owned buffer
 * rather than allocating: it runs once per plane per changed tile, which is a
 * few hundred calls in a frame where a menu drops.
 */
export function unpackBits(
  src: Uint8Array, at: number, end: number, dst: Uint8Array, want: number,
): number {
  let o = 0;
  let i = at;

  while (o < want) {
    if (i >= end) throw new Error("PackBits ran out of input");
    const n = src[i++];

    if (n === 128) continue;

    if (n < 128) {
      const run = n + 1;
      if (i + run > end || o + run > want) {
        throw new Error("PackBits literal of " + run + " does not fit");
      }
      dst.set(src.subarray(i, i + run), o);
      i += run;
      o += run;
    } else {
      const run = 257 - n;
      if (i >= end || o + run > want) {
        throw new Error("PackBits run of " + run + " does not fit");
      }
      dst.fill(src[i++], o, o + run);
      o += run;
    }
  }

  return i;
}

/* One plane of one tile, which is the most a single op decodes into. */
export function scratchBytes(g: Geometry): number {
  return g.tileW * g.tileH;
}

/*
 * Apply one frame to a planar framebuffer in place, and say what moved.
 *
 * In place, including the copies.  The buffer still holds the previous frame
 * when a copy op runs -- copies always precede the tiles and never overlap
 * each other -- so a copy's source is the previous frame's rows exactly as
 * the encoder meant, provided the rows are moved in the direction that does
 * not overwrite a row it has yet to read.  Keeping a second buffer to avoid
 * thinking about that would be a megabyte and a memcpy a frame for nothing.
 *
 * The damage rectangle is what the caller redraws.  One rectangle rather than
 * a tile list because the decode is per pixel and cheap -- 0.144 ms for a
 * whole 640x256x2 Workbench, and apply plus damage-rectangle decode together
 * come to 0.038 ms a frame on the synthetic 640x480x3 case -- so the bounding
 * box of a few scattered tiles is never the thing to optimise first.
 */
export function applyUpdate(
  g: Geometry, frame: Uint8Array, dst: Uint8Array, scratch: Uint8Array,
): Damage {
  const b = frame;
  if (b.length < 5) throw new Error("frame is " + b.length + " bytes");
  if (b[0] !== RFB_VERSION) {
    throw new Error("frame version " + b[0] + ", not " + RFB_VERSION);
  }

  const w = g.screen.width;
  const h = g.screen.height;
  const bpr = g.screen.bytesPerRow;
  const plane = planeBytes(g.screen);
  /* PLANES, not the depth.  Both RTG formats are one plane and eight or
     sixteen bits deep, and every loop below wants the first number. */
  const depth = planeCount(g.screen);
  const fmt = screenFormat(g.screen);
  /* Which of the two tile ops the stream carries, and therefore whether a
     tile has a plane mask on it.  Both RTG formats have one plane, so both
     use OP_TILE8; how wide a tile's bytes are in pixels is the only thing
     that separates them and no op reader here needs to know.
     The three chipset modes are on the other side of this: they are the Amiga
     BitMap and carry OP_TILE with a mask, exactly as a plain planar screen
     does, which is why the test is chunkiness and not format 0. */
  const onePlane = isChunky(fmt);
  const perByte = pixelsPerByte(g.screen);

  const d: Damage = {
    x0: w, y0: h, x1: 0, y1: 0,
    tiles: 0, copies: 0, bytes: b.length, seq: (b[2] << 8) | b[3],
  };

  /* Byte columns to pixels, clipped to the screen: the padding at the end of
     a row is encoded and is not on the display.
     Rounded outwards because a byte is HALF a pixel at 16 bits and a tile
     grid over bytes need not land on one: the left edge floors and the right
     edge ceils, so a rectangle that covers the low byte of a pixel covers
     that whole pixel.  Rounding either edge the other way leaves a column
     undrawn, which is a stale strip down the side of whatever moved. */
  const hit = (bx0: number, bw: number, y0: number, rows: number) => {
    const px0 = Math.floor(bx0 * perByte);
    const px1 = Math.min(Math.ceil((bx0 + bw) * perByte), w);
    if (px1 <= px0 || rows <= 0) return;
    if (px0 < d.x0) d.x0 = px0;
    if (px1 > d.x1) d.x1 = px1;
    if (y0 < d.y0) d.y0 = y0;
    if (y0 + rows > d.y1) d.y1 = y0 + rows;
  };

  let i = 4;

  for (;;) {
    if (i >= b.length) throw new Error("frame ended without an END op");
    const op = b[i++];

    if (op === OP_END) break;

    if (op === OP_COPY) {
      if (i + 10 > b.length) throw new Error("a copy op is cut short");
      const x0 = (b[i] << 8) | b[i + 1];
      const cw = (b[i + 2] << 8) | b[i + 3];
      const y0 = (b[i + 4] << 8) | b[i + 5];
      const ch = (b[i + 6] << 8) | b[i + 7];
      /* Signed: a scroll can go either way. */
      const dy = (((b[i + 8] << 8) | b[i + 9]) << 16) >> 16;
      i += 10;

      if (x0 + cw > bpr || y0 + ch > h) {
        throw new Error("a copy op leaves the screen");
      }
      if (y0 + dy < 0 || y0 + ch + dy > h) {
        throw new Error("a copy op reads from off the screen");
      }

      for (let p = 0; p < depth; p++) {
        const base = p * plane;
        if (dy > 0) {
          /* Source below destination: front to back, or a row that has yet
             to be read is overwritten by one already written. */
          for (let r = 0; r < ch; r++) {
            const to = base + (y0 + r) * bpr + x0;
            const from = base + (y0 + r + dy) * bpr + x0;
            dst.copyWithin(to, from, from + cw);
          }
        } else {
          for (let r = ch; r > 0; r--) {
            const to = base + (y0 + r - 1) * bpr + x0;
            const from = base + (y0 + r - 1 + dy) * bpr + x0;
            dst.copyWithin(to, from, from + cw);
          }
        }
      }

      d.copies++;
      hit(x0, cw, y0, ch);
      continue;
    }

    if (op !== OP_TILE && op !== OP_TILE8) {
      throw new Error("op " + op + " is not one of ours");
    }
    /* The geometry decides which of the two a stream carries, so meeting the
       other one means the `geom` and the frames disagree about what a byte
       is.  That draws a picture rather than failing, which is why it is
       checked. */
    if ((op === OP_TILE8) !== onePlane) {
      throw new Error("op " + op + " on a " + FMT_NAME[fmt] + " screen");
    }

    /* No plane mask on a one-plane tile: there is one plane and it is the one
       that changed.  Two bytes of index either way. */
    if (i + (onePlane ? 2 : 3) > b.length) {
      throw new Error("a tile op is cut short");
    }
    const idx = (b[i] << 8) | b[i + 1];
    const mask = onePlane ? 1 : b[i + 2];
    i += onePlane ? 2 : 3;

    if (idx >= g.across * g.down) {
      throw new Error("tile index " + idx + " is off the grid");
    }

    const tx = idx % g.across;
    const ty = (idx - tx) / g.across;
    const x0 = tx * g.tileW;
    const y0 = ty * g.tileH;
    const tw = Math.min(g.tileW, bpr - x0);
    const th = Math.min(g.tileH, h - y0);
    const want = tw * th;

    for (let p = 0; p < depth; p++) {
      if ((mask & (1 << p)) === 0) continue;

      if (i >= b.length) throw new Error("a tile op ran out of planes");
      const code = b[i++];

      let src = b;
      let so = i;

      if (code === CODE_RAW) {
        if (i + want > b.length) throw new Error("a raw tile is cut short");
        i += want;
      } else if (code === CODE_PB_RAW || code === CODE_PB_XOR) {
        if (i + 2 > b.length) throw new Error("a packed tile is cut short");
        const len = (b[i] << 8) | b[i + 1];
        i += 2;
        if (i + len > b.length) {
          throw new Error("a packed tile claims " + len + " bytes and " +
                          (b.length - i) + " are left");
        }
        unpackBits(b, i, i + len, scratch, want);
        i += len;
        src = scratch;
        so = 0;
      } else {
        throw new Error("tile code " + code + " is not one of ours");
      }

      const base = p * plane;
      for (let r = 0; r < th; r++) {
        const to = base + (y0 + r) * bpr + x0;
        const fo = so + r * tw;
        if (code === CODE_PB_XOR) {
          for (let c = 0; c < tw; c++) dst[to + c] ^= src[fo + c];
        } else {
          for (let c = 0; c < tw; c++) dst[to + c] = src[fo + c];
        }
      }
    }

    d.tiles++;
    hit(x0, tw, y0, th);
  }

  if (d.x1 <= d.x0) { d.x0 = 0; d.y0 = 0; d.x1 = 0; d.y1 = 0; }

  /*
   * A HAM row is damaged from its left edge whatever moved in it.
   *
   * Every pixel takes its colour from the one before it, so changing a byte
   * half way along a row changes the meaning of every pixel to the RIGHT of it
   * as well -- and redrawing only the tile that moved would decode those from
   * a running colour that is no longer what the encoder had.  Widening the
   * rectangle to the full width is the whole fix: the rows are the ones that
   * moved, and a row is what the HAM decoder works in anyway.
   *
   * It costs the width of the screen rather than the width of a tile, on a
   * format that is 320 or 640 pixels across.  Left as a rectangle rather than
   * pushed into the decoder because the same rectangle is what the caller
   * uploads to the canvas, and the two must agree.
   */
  if (isHam(fmt) && d.x1 > d.x0) { d.x0 = 0; d.x1 = w; }

  return d;
}
