/*
 * Planar bitmaps into RGBA.  This is the one loop that has to be fast.
 *
 * An Amiga screen is `depth` separate 1-bit planes stored one after another,
 * each bytesPerRow*height bytes; a pixel's colour index is one bit taken from
 * each plane with plane 0 as the low bit.  There is no arrangement of that a
 * browser can hand to a canvas, so every frame passes through here and this is
 * where the viewer's whole cost lives.
 *
 * bytesPerRow is read from the capture and is NOT width/8.  A BitMap is
 * allocated in whole words and a Workbench screen is routinely padded, so a
 * decoder that computes the stride draws a picture that shears one byte
 * further left on every row.
 *
 * The `if (b === 0) continue` in the inner loop is worth more than it looks.
 * A Workbench screen is mostly pen 0, which is all-zero in every plane, so
 * most bytes skip the eight tests below them -- and it is also why the times
 * quoted in the selftest are quoted against synthesised Workbench-like
 * content rather than noise, which would be the slowest case and not the one
 * anybody is going to look at.
 *
 * Scaling is deliberately absent.  The canvas is the screen's native size and
 * CSS stretches it with image-rendering: pixelated, so nearest-neighbour is
 * exact, costs no CPU and runs where the pixels already are.  A JS scaler
 * would be several times more expensive than the decode it followed.
 *
 * SPDX-License-Identifier: MIT
 */

export interface Screen {
  readonly width: number;
  readonly height: number;
  readonly depth: number;
  readonly bytesPerRow: number;
}

export function planeBytes(s: Screen): number {
  return s.bytesPerRow * s.height;
}

export function frameBytes(s: Screen): number {
  return s.bytesPerRow * s.height * s.depth;
}

/* Reasons a Screen cannot be one, as a sentence or null.  Every entry point
   that takes bytes off a wire or a file runs this first: a bad depth turns
   into an out-of-range palette index and a silent black picture. */
export function screenFault(s: Screen): string | null {
  if (!Number.isInteger(s.width) || s.width <= 0) return "width " + s.width;
  if (!Number.isInteger(s.height) || s.height <= 0) return "height " + s.height;
  if (!Number.isInteger(s.depth) || s.depth < 1 || s.depth > 8) {
    return "depth " + s.depth + ", which is not 1..8";
  }
  if (!Number.isInteger(s.bytesPerRow) || s.bytesPerRow * 8 < s.width) {
    return "bytesPerRow " + s.bytesPerRow + " holds " + s.bytesPerRow * 8 +
           " pixels and the screen is " + s.width + " wide";
  }
  return null;
}

/*
 * The palette, as the 32-bit words a canvas's backing store is made of.
 *
 * Byte order is the machine's, not the format's: an ImageData is bytes in
 * R,G,B,A order and a Uint32Array over it reads them in whichever order the
 * CPU assembles a word.  Every browser this will ever run in is little
 * endian, and the probe is two lines, so the assumption is checked rather
 * than written down.
 */
const LITTLE_ENDIAN = (() => {
  const word = new Uint32Array(1);
  new Uint8Array(word.buffer)[0] = 1;
  return word[0] === 1;
})();

export function palette32(rgb: Uint8Array, depth: number): Uint32Array {
  const n = 1 << depth;
  if (rgb.length < n * 3) {
    throw new Error("palette is " + rgb.length + " bytes, depth " + depth +
                    " needs " + n * 3);
  }

  const pal = new Uint32Array(n);
  for (let i = 0; i < n; i++) {
    const r = rgb[i * 3], g = rgb[i * 3 + 1], b = rgb[i * 3 + 2];
    pal[i] = LITTLE_ENDIAN
      ? ((255 << 24) | (b << 16) | (g << 8) | r) >>> 0
      : ((r << 24) | (g << 16) | (b << 8) | 255) >>> 0;
  }
  return pal;
}

/*
 * A rectangle of one frame, plane-major at `off`, into `out` -- a
 * Uint32Array over an ImageData's data at the screen's full width.
 *
 * Eight pixels a byte, accumulated across the planes and written once,
 * because the alternative is eight read-modify-writes into the output for
 * every plane after the first.
 *
 * x0 is snapped down to a byte and x1 up to one: a tile is byte aligned and
 * nothing that calls this ever asks for a rectangle that is not, so paying
 * for a shift on every byte to support one would be paying for nothing.
 */
export function decodeRectInto(
  s: Screen,
  planes: Uint8Array,
  off: number,
  pal: Uint32Array,
  out: Uint32Array,
  x0: number,
  y0: number,
  x1: number,
  y1: number,
): void {
  const w = s.width;
  const d = s.depth;
  const bpr = s.bytesPerRow;
  const plane = bpr * s.height;

  const bx0 = Math.max(0, x0) >> 3;
  const bx1 = Math.min((w + 7) >> 3, (Math.min(x1, w) + 7) >> 3);
  const ry0 = Math.max(0, y0);
  const ry1 = Math.min(s.height, y1);

  for (let y = ry0; y < ry1; y++) {
    const row = off + y * bpr;
    let o = y * w + (bx0 << 3);

    for (let bx = bx0; bx < bx1; bx++) {
      let a0 = 0, a1 = 0, a2 = 0, a3 = 0, a4 = 0, a5 = 0, a6 = 0, a7 = 0;

      for (let p = 0, at = row + bx; p < d; p++, at += plane) {
        const b = planes[at];
        if (b === 0) continue;
        const m = 1 << p;
        if (b & 0x80) a0 |= m;
        if (b & 0x40) a1 |= m;
        if (b & 0x20) a2 |= m;
        if (b & 0x10) a3 |= m;
        if (b & 0x08) a4 |= m;
        if (b & 0x04) a5 |= m;
        if (b & 0x02) a6 |= m;
        if (b & 0x01) a7 |= m;
      }

      /* The last byte of a width that is not a multiple of eight holds
         pixels that are not on the screen.  Workbench never produces one and
         it costs four lines to not be wrong about it. */
      if (w - (bx << 3) >= 8) {
        out[o] = pal[a0];
        out[o + 1] = pal[a1];
        out[o + 2] = pal[a2];
        out[o + 3] = pal[a3];
        out[o + 4] = pal[a4];
        out[o + 5] = pal[a5];
        out[o + 6] = pal[a6];
        out[o + 7] = pal[a7];
        o += 8;
      } else {
        const eight = [a0, a1, a2, a3, a4, a5, a6, a7];
        for (let k = 0, n = w - (bx << 3); k < n; k++) out[o++] = pal[eight[k]];
      }
    }
  }
}

export function decodeInto(
  s: Screen,
  planes: Uint8Array,
  off: number,
  pal: Uint32Array,
  out: Uint32Array,
): void {
  decodeRectInto(s, planes, off, pal, out, 0, 0, s.width, s.height);
}

/*
 * How many square pixels one Amiga pixel covers.
 *
 * A 640x256 screen is a hires non-interlaced one and fills the same area of
 * glass as a 640x512 interlaced one, so it is displayed at 1:2 and a viewer
 * that shows it 640x256 shows Workbench squashed flat.  There is no field in
 * a BitMap that says any of this -- the modulo is in the display hardware --
 * so it is inferred from the dimensions, which is what every Amiga screen
 * grabber has always done:
 *
 *   width  < 640   lores, two square pixels wide
 *   height < 400   not interlaced, two square pixels tall
 *
 * 800x600 and anything else larger comes out 1:1, which is right for the RTG
 * and Super72-ish modes where it is the only sensible answer.
 */
export function pixelAspect(s: Screen): { x: number; y: number } {
  return { x: s.width < 640 ? 2 : 1, y: s.height < 400 ? 2 : 1 };
}
