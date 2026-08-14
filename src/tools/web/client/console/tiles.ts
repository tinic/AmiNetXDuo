/*
 * Tile updates, and the one file that has to change when the encoder lands.
 *
 * The wire format is being written on the encoder side (rfb_encode.c, branch
 * proto/rfb-encoder) and does not exist yet.  Everything in this file below
 * the PackBits decoder is therefore a PLACEHOLDER: a framing invented here so
 * that the socket, the damage tracking and the redraw path could be built and
 * measured, chosen to match what the encoder was described as doing -- a tile
 * grid, a per-tile plane mask, and per-tile PackBits over an XOR against the
 * previous frame.
 *
 * It is one module, and the rest of the viewer reaches it through applyUpdate
 * and geometryFromWord alone, so replacing it with the real thing is a file
 * and not a refactor.  Nothing else in client/console/ parses a byte off the
 * socket.
 *
 * PLACEHOLDER framing, big-endian:
 *
 *   0   u8   version = 1
 *   1   u8   flags        bit 0: keyframe -- payloads are literal, not XOR
 *   2   u16  seq          wraps; the viewer reports gaps and nothing else
 *   4   u16  tileCount
 *   6   tileCount records of
 *          u16  tile       ty * across + tx
 *          u8   planes     bit p set: plane p is present
 *          u8   coding     0 raw, 1 PackBits
 *          u16  length     bytes of payload that follow
 *          ..   payload    each present plane in ascending order,
 *                          (tileW/8)*tileH bytes, XOR against what is already
 *                          there unless the keyframe bit is set
 *
 * The one part of this that is NOT a guess is PackBits.  It is the Amiga's
 * own run coder -- cmpByteRun1, what every compressed IFF ILBM in existence
 * uses -- so a 68020 encoding with it is running an algorithm it has shipped
 * since 1985, and a decoder for it is worth having whatever the framing turns
 * out to be.
 *
 * Why XOR and not a plain replace: two planes of a Workbench screen change
 * when a menu drops over it and six do not, and the XOR of a tile against its
 * previous self is mostly zero, which is exactly what a run coder is good at.
 * The cost is that a dropped update poisons everything after it, which is
 * what the keyframe bit and the `refresh` word are for.
 *
 * SPDX-License-Identifier: MIT
 */

import { planeBytes, screenFault, type Screen } from "./planar";

export interface Geometry {
  readonly screen: Screen;
  readonly tileW: number;
  readonly tileH: number;
  readonly across: number;
  readonly down: number;
  /* Bytes one plane of one tile occupies.  A tile is byte aligned in x, which
     is what makes a tile row a memcpy on both sides. */
  readonly tileBytes: number;
  readonly tileBytesPerRow: number;
}

export interface Damage {
  /* In screen pixels, empty when x1 <= x0. */
  x0: number;
  y0: number;
  x1: number;
  y1: number;
  tiles: number;
  bytes: number;
  keyframe: boolean;
  seq: number;
}

export function makeGeometry(
  screen: Screen, tileW: number, tileH: number,
): Geometry {
  const fault = screenFault(screen);
  if (fault !== null) throw new Error("the geometry says " + fault);
  if (tileW <= 0 || tileW % 8 !== 0) {
    throw new Error("tile width " + tileW + " is not a multiple of 8, and a " +
                    "tile that starts mid-byte is a shift on the Amiga side");
  }
  if (tileH <= 0) throw new Error("tile height " + tileH);

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
 * `geom W H DEPTH BYTESPERROW TILEW TILEH`, the control word that opens a
 * session.  Text rather than a binary header because it is control, and the
 * split between the two channels is the whole convention: binary frames are
 * the data stream and text frames are words.
 */
export function geometryFromWord(w: string): Geometry {
  const f = w.trim().split(/\s+/);
  if (f[0] !== "geom" || f.length !== 7) {
    throw new Error("geom takes six numbers, got: " + w);
  }
  const n = f.slice(1).map(Number);
  if (n.some((x) => !Number.isInteger(x))) {
    throw new Error("geom has something that is not a whole number: " + w);
  }
  return makeGeometry(
    { width: n[0], height: n[1], depth: n[2], bytesPerRow: n[3] },
    n[4], n[5],
  );
}

/*
 * `pal RRGGBB...`, hex, one triple per entry.  1536 characters at depth 8,
 * which is a text frame and not a problem; it is sent once per session and
 * again whenever LoadRGB4 moves something.
 */
export function paletteFromWord(w: string, depth: number): Uint8Array {
  const hex = w.trim().slice(4).replace(/\s+/g, "");
  const want = 3 * (1 << depth);
  if (hex.length !== want * 2) {
    throw new Error("pal is " + hex.length / 2 + " bytes, depth " + depth +
                    " needs " + want);
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
 * PackBits, the ILBM run coder.
 *
 *   n in 0..127     the next n+1 bytes are literal
 *   n in 129..255   the next byte repeated 257-n times
 *   n == 128        no operation
 *
 * Decoding into a caller-owned scratch buffer rather than allocating: this
 * runs once per plane per changed tile, which at 640x480 with 16x16 tiles and
 * a menu dropping is a few hundred calls in one frame.
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

/*
 * Apply an update to a planar framebuffer in place, and say what moved.
 *
 * The damage rectangle is what the caller redraws.  It is one rectangle
 * rather than a tile list because the decode cost is per pixel and the
 * bounding box of a few scattered tiles is still far cheaper to re-decode
 * than it is to keep a per-tile ImageData and blit each one; measured at
 * 640x480x3, a whole-screen decode is 0.9 ms, so a bounding box is never the
 * thing to optimise first.
 */
export function applyUpdate(
  g: Geometry, frame: Uint8Array, dst: Uint8Array, scratch: Uint8Array,
): Damage {
  const b = frame;
  if (b.length < 6) throw new Error("update is " + b.length + " bytes");

  const version = b[0];
  if (version !== 1) throw new Error("update version " + version + ", not 1");

  const keyframe = (b[1] & 1) !== 0;
  const seq = (b[2] << 8) | b[3];
  const count = (b[4] << 8) | b[5];

  const plane = planeBytes(g.screen);
  const bpr = g.screen.bytesPerRow;
  const rowBytes = g.tileBytesPerRow;

  const d: Damage = {
    x0: g.screen.width, y0: g.screen.height, x1: 0, y1: 0,
    tiles: 0, bytes: b.length, keyframe, seq,
  };

  let at = 6;

  for (let t = 0; t < count; t++) {
    if (at + 6 > b.length) throw new Error("update ends inside tile " + t);

    const tile = (b[at] << 8) | b[at + 1];
    const mask = b[at + 2];
    const coding = b[at + 3];
    const length = (b[at + 4] << 8) | b[at + 5];
    at += 6;

    if (at + length > b.length) {
      throw new Error("tile " + t + " claims " + length + " bytes and " +
                      (b.length - at) + " are left");
    }
    const end = at + length;

    const tx = tile % g.across;
    const ty = (tile - tx) / g.across;
    if (ty >= g.down) throw new Error("tile index " + tile + " is off the grid");

    const px = tx * g.tileW;
    const py = ty * g.tileH;
    /* The right and bottom edges are short when the screen is not a whole
       number of tiles.  The payload is a full tile either way -- padding is
       cheaper on the Amiga than a special case -- and the rows past the
       bottom are dropped here. */
    const rows = Math.min(g.tileH, g.screen.height - py);

    let src = b;
    let so = at;

    if (coding === 1) {
      const nplanes = countBits(mask);
      unpackBits(b, at, end, scratch, nplanes * g.tileBytes);
      src = scratch;
      so = 0;
    } else if (coding !== 0) {
      throw new Error("tile " + t + " uses coding " + coding);
    }

    for (let p = 0; p < g.screen.depth; p++) {
      if ((mask & (1 << p)) === 0) continue;

      for (let r = 0; r < rows; r++) {
        const to = p * plane + (py + r) * bpr + (px >> 3);
        const fo = so + r * rowBytes;
        if (keyframe) {
          for (let i = 0; i < rowBytes; i++) dst[to + i] = src[fo + i];
        } else {
          for (let i = 0; i < rowBytes; i++) dst[to + i] ^= src[fo + i];
        }
      }

      so += g.tileBytes;
    }

    at = end;

    d.tiles++;
    if (px < d.x0) d.x0 = px;
    if (py < d.y0) d.y0 = py;
    if (px + g.tileW > d.x1) d.x1 = Math.min(px + g.tileW, g.screen.width);
    if (py + rows > d.y1) d.y1 = py + rows;
  }

  if (d.tiles === 0) { d.x0 = 0; d.y0 = 0; }
  return d;
}

function countBits(v: number): number {
  let n = 0;
  for (let i = 0; i < 8; i++) if (v & (1 << i)) n++;
  return n;
}

/* The biggest scratch one update can need: every plane of one tile. */
export function scratchBytes(g: Geometry): number {
  return g.tileBytes * g.screen.depth;
}
