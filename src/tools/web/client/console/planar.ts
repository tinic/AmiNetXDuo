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
 * allocated in whole words, BMF_INTERLEAVED moves the stride again, and a
 * decoder that computes it draws a picture that shears one byte further left
 * on every row.
 *
 * Measured on the real captures, node on an M-series laptop, whole screen:
 * 640x256x2 in 0.144 ms and 640x256x4 in 0.173 ms, which is what stock
 * Workbench 3.1 is.  As headroom, 640x480x3 in 0.284 ms and 800x600x8 in
 * 0.668 ms.  In the browser the same call plus the putImageData upload is
 * 0.20 ms at 640x256x2.  A 60 Hz budget is 16.7 ms, so the straightforward
 * loop stays.
 *
 * The `if (b === 0) continue` in the inner loop is worth more than it looks:
 * a Workbench screen is mostly pen 0, which is all-zero in every plane, so
 * most bytes skip the eight tests below them.  It is also why those times are
 * quoted against Workbench and not noise, which is the slowest case and not
 * the one anybody is going to look at.
 *
 * Scaling is deliberately absent.  The canvas is the screen's native size and
 * CSS stretches it with image-rendering: pixelated, so nearest-neighbour is
 * exact, costs no CPU and runs where the pixels already are.  A JS scaler
 * would be several times more expensive than the decode it followed.
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * What a byte of a frame is, as rfb_geom.format numbers it and as a .pfs
 * header's flags byte says the same thing:
 *
 *   FMT_PLANAR   depth one-bit planes, the Amiga BitMap above.  depth is the
 *                plane count and 1 << depth is the palette.
 *   FMT_CLUT8    one eight-bit plane: a byte IS the palette index and there
 *                is nothing to unpack.  depth is 8 because that is what sizes
 *                the palette, not because there are eight planes.
 *   FMT_RGB565   one sixteen-bit plane, big-endian R5G6B5, which is what a
 *                15, 16, 24 or 32-bit RTG screen is downsampled to before it
 *                is sent.  depth is 16 and IS BITS PER PIXEL: there is no
 *                palette, none ever arrives, and 1 << depth is not a colour
 *                count anything should ask for.
 *
 * Here rather than in tiles.ts because they are what a Screen means; tiles.ts
 * re-exports them, which is where the rest of the page reads them from.
 */
export const FMT_PLANAR = 0;
export const FMT_CLUT8 = 1;
export const FMT_RGB565 = 2;

export interface Screen {
  readonly width: number;
  readonly height: number;
  readonly depth: number;
  readonly bytesPerRow: number;
  /* One of FMT_ above.  Optional, and screenFormat() is what reads it. */
  readonly format?: number;
  /*
   * FMT_CLUT8, in the spelling that predates there being three formats to
   * tell apart.  Every capture and every caller written when there were two
   * still says chunky and still means the same screen, so it is what
   * screenFormat() falls back to; readers that ask for it directly keep
   * working because both fields are set from the same number wherever a
   * Screen is built.
   */
  readonly chunky?: boolean;
}

/* Which of the three, whichever field the Screen was built with. */
export function screenFormat(s: Screen): number {
  return s.format ?? (s.chunky ? FMT_CLUT8 : FMT_PLANAR);
}

/*
 * ENTRIES IN THE PALETTE, WHICH IS A PROPERTY OF THE FORMAT AND NOT OF THE
 * DEPTH.  1 << depth is the answer on FMT_PLANAR and nowhere else: FMT_CLUT8
 * is 256 whatever a byte is called, and FMT_RGB565 has no palette at all and
 * is never sent one.
 *
 * A function rather than an expression at each site because the formats
 * coming next are the ones that break the old rule outright -- HAM6 is six
 * planes deep with sixteen base colours, HAM8 eight with sixty-four, EHB six
 * with thirty-two -- and every one of them sends a `pal` far shorter than its
 * depth implies.  rfb_pal_colours() in include/aminetxduo/rfb_encode.h is the
 * same rule on the other side of the wire.  Nothing outside this function
 * should size a palette.
 */
export function palColours(s: Screen): number {
  switch (screenFormat(s)) {
    case FMT_CLUT8: return 256;
    case FMT_RGB565: return 0;
    default: return 1 << s.depth;
  }
}

/* Planes in the source, which is NOT the depth on either chunky format. */
export function planeCount(s: Screen): number {
  return screenFormat(s) === FMT_PLANAR ? s.depth : 1;
}

export function planeBytes(s: Screen): number {
  return s.bytesPerRow * s.height;
}

export function frameBytes(s: Screen): number {
  return s.bytesPerRow * s.height * planeCount(s);
}

/* Pixels one byte of a row covers, by format.  The tile grid is over
   bytesPerRow whatever the format is, so this is what turns a byte column
   into a pixel column -- and on a 16-bit screen it is a HALF, which is why
   every caller either multiplies a byte count by it or rounds the result. */
const PIXELS_PER_BYTE = [8, 1, 0.5];

export function pixelsPerByte(s: Screen): number {
  return PIXELS_PER_BYTE[screenFormat(s)];
}

/* Reasons a Screen cannot be one, as a sentence or null.  Every entry point
   that takes bytes off a wire or a file runs this first: a bad depth turns
   into an out-of-range palette index and a silent black picture. */
export function screenFault(s: Screen): string | null {
  if (!Number.isInteger(s.width) || s.width <= 0) return "width " + s.width;
  if (!Number.isInteger(s.height) || s.height <= 0) return "height " + s.height;

  const f = screenFormat(s);
  if (f !== FMT_PLANAR && f !== FMT_CLUT8 && f !== FMT_RGB565) {
    return "format " + f + ", which is not one this viewer draws";
  }

  /* Bits per pixel on the 16-bit format and a plane count on the other two,
     so the range depends on which it is: refusing 16 here is what a viewer
     that only knew about palettes did, and it is a blank screen. */
  if (f === FMT_RGB565) {
    if (s.depth !== 16) {
      return "a 16-bit screen " + s.depth + " deep; the pixels are R5G6B5, " +
             "so it is 16 or it is not this format";
    }
  } else if (!Number.isInteger(s.depth) || s.depth < 1 || s.depth > 8) {
    return "depth " + s.depth + ", which is not 1..8";
  } else if (f === FMT_CLUT8 && s.depth !== 8) {
    return "a chunky screen " + s.depth + " deep; a byte is the index, so it " +
           "is 8 or it is not this format";
  }

  const per = pixelsPerByte(s);
  if (!Number.isInteger(s.bytesPerRow) || s.bytesPerRow * per < s.width) {
    return "bytesPerRow " + s.bytesPerRow + " holds " + s.bytesPerRow * per +
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

/* `colours` is entries and NOT a depth: ask palColours() for it, which is the
   one place that knows how many a format has. */
export function palette32(rgb: Uint8Array, colours: number): Uint32Array {
  const n = colours;
  if (rgb.length < n * 3) {
    throw new Error("palette is " + rgb.length + " bytes, " + n +
                    " colours need " + n * 3);
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
 * R5G6B5 into the same 32-bit words, as two 256-entry tables and one add.
 *
 * A pixel is two bytes, so the obvious form is six shifts and three ors per
 * pixel to pull the fields out and replicate them up to eight bits.  Split by
 * byte instead: red comes only from the high byte, blue only from the low one,
 * and green is 4*g6 + (g6 >> 4), whose halves are 32*ghi + (ghi >> 1) from the
 * high byte and 4*glo from the low one.  Those two greens sum to at most 255,
 * so adding the two table entries cannot carry out of the green byte into a
 * neighbour -- which is what makes an add legal here where an or would not be.
 *
 * Worth the tables: a whole 640x480 screen measured in node on an M-series
 * laptop is 0.48 ms this way against 1.45 ms written out as shifts, and a
 * truecolour screen is the largest thing this decoder is ever handed.
 *
 * Bit replication and not a shift, so 0x1f comes out 0xff rather than 0xf8:
 * white on the Amiga has to be white here, and every other decoder of this
 * format does the same.
 */
const HI565 = new Uint32Array(256);
const LO565 = new Uint32Array(256);

for (let i = 0; i < 256; i++) {
  const r5 = i >> 3;
  const ghi = i & 7;
  const glo = i >> 5;
  const b5 = i & 0x1f;

  const r8 = (r5 << 3) | (r5 >> 2);
  const b8 = (b5 << 3) | (b5 >> 2);
  const gh = (ghi << 5) | (ghi >> 1);
  const gl = glo << 2;

  HI565[i] = LITTLE_ENDIAN
    ? ((255 << 24) | (gh << 8) | r8) >>> 0
    : ((r8 << 24) | (gh << 16) | 255) >>> 0;
  LO565[i] = LITTLE_ENDIAN
    ? ((b8 << 16) | (gl << 8)) >>> 0
    : ((gl << 16) | (b8 << 8)) >>> 0;
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

  /*
   * The two RTG layouts first, and between them they are the whole of the RTG
   * viewer: a pixel is one byte of palette index or two bytes of colour, so
   * there is no bit shuffling to do in either and the rectangle is in pixels
   * rather than in byte columns.  The padding past the width is skipped here
   * the same way the planar loop skips it below -- it is encoded, and it is
   * not on the display.
   */
  const fmt = screenFormat(s);

  if (fmt === FMT_CLUT8 || fmt === FMT_RGB565) {
    const cx0 = Math.max(0, x0);
    const cx1 = Math.min(w, x1);
    const cy0 = Math.max(0, y0);
    const cy1 = Math.min(s.height, y1);

    if (fmt === FMT_CLUT8) {
      for (let y = cy0; y < cy1; y++) {
        const row = off + y * bpr;
        let o = y * w + cx0;
        for (let x = cx0; x < cx1; x++) out[o++] = pal[planes[row + x]];
      }
      return;
    }

    /* Two bytes a pixel, high byte first, and `pal` is not read at all: on a
       truecolour screen there is nothing in it to read. */
    for (let y = cy0; y < cy1; y++) {
      let at = off + y * bpr + cx0 * 2;
      let o = y * w + cx0;
      for (let x = cx0; x < cx1; x++, at += 2) {
        out[o++] = HI565[planes[at]] + LO565[planes[at + 1]];
      }
    }
    return;
  }

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
 *
 * AN RTG SCREEN IS 1:1 AND IS NOT INFERRED AT ALL, whichever of the two RTG
 * formats it arrives in.  Those rules are about the chipset's display modulo;
 * a graphics card has square pixels at every size it can put up, so a 320x240
 * RTG screen guessed at by the rule above would be drawn four times the area
 * it is.
 */
export function pixelAspect(s: Screen): { x: number; y: number } {
  if (screenFormat(s) !== FMT_PLANAR) return { x: 1, y: 1 };
  return { x: s.width < 640 ? 2 : 1, y: s.height < 400 ? 2 : 1 };
}
