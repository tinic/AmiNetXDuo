/*
 * The pointer, drawn here and never waited for.
 *
 * This is the whole latency argument in one file.  A remote framebuffer that
 * ships the pointer as part of the picture makes the mouse feel as far away
 * as the machine is -- move it, and the arrow follows a round trip later.
 * The browser already knows where the mouse is, so the arrow is drawn from
 * that, locally, on an overlay above the screen: it moves at the rate the
 * host's compositor moves, and only what it lands ON is remote.
 *
 * A consequence worth stating rather than discovering: when the Amiga side
 * gains input injection there will be two pointers for as long as the Amiga
 * is also drawing one, and the answer is for the capture to leave the sprite
 * out, not for this to start waiting.
 *
 * The shape is the Workbench arrow at 16x16 with a black outline, which is
 * what a hardware sprite gave it -- two bitplanes, three colours, and the
 * outline is what makes it visible over a white window and a blue title bar
 * alike.  Written as rows rather than hex so that changing it is possible.
 *
 * SPDX-License-Identifier: MIT
 */

const ROWS = [
  "k...............",
  "kk..............",
  "kwk.............",
  "kwwk............",
  "kwwwk...........",
  "kwwwwk..........",
  "kwwwwwk.........",
  "kwwwwwwk........",
  "kwwwwwwwk.......",
  "kwwwwwkkkk......",
  "kwwkwwk.........",
  "kwk.kwwk........",
  "kk..kwwk........",
  "k....kwwk.......",
  ".....kwwk.......",
  "......kk........",
];

export const POINTER_W = 16;
export const POINTER_H = 16;

/* The tip, in pointer pixels.  A hotspot elsewhere is what makes a click land
   somewhere the person did not point at. */
export const HOT_X = 0;
export const HOT_Y = 0;

/* The buffer type is spelled out because ImageData will not take one over a
   SharedArrayBuffer, which is what a bare Uint8ClampedArray now means. */
export function pointerRGBA(): Uint8ClampedArray<ArrayBuffer> {
  const out = new Uint8ClampedArray(POINTER_W * POINTER_H * 4);

  for (let y = 0; y < POINTER_H; y++) {
    const row = ROWS[y];
    for (let x = 0; x < POINTER_W; x++) {
      const c = row[x];
      const o = (y * POINTER_W + x) * 4;
      if (c === "k") {
        out[o + 3] = 255;
      } else if (c === "w") {
        out[o] = 255; out[o + 1] = 255; out[o + 2] = 255; out[o + 3] = 255;
      }
    }
  }

  return out;
}

/*
 * THE REAL POINTER, WHEN THE AMIGA SENDS ONE.
 *
 * The arrow above is what this page draws before it has been told anything --
 * a machine serving a .pfs capture with no pointer in it, or a session in the
 * first moments before the `ptr` word arrives.  Once one does, the Amiga's own
 * sprite replaces it, at the Amiga's own hotspot.
 *
 * SCALED HERE, ONCE, AND NOT BY THE CANVAS.  A sprite pixel is not a screen
 * pixel: it is a lores pixel whatever the screen is, so on a hires screen it
 * covers two screen pixels across and on an interlaced screen two rows down.
 * The server works the factors out from the screen's mode and the sprite's own
 * resolution and sends them, so the two ends cannot disagree about it, and the
 * expansion happens when the image arrives rather than on every move.
 *
 * THE HOTSPOT SCALES WITH THE IMAGE.  It arrives in sprite pixels, so a
 * pointer whose tip is three sprite pixels in has its tip six screen pixels in
 * on a hires screen; using the unscaled number would put every click that many
 * pixels off, and further off the deeper into the sprite the hotspot is.
 */
export interface PointerImage {
  readonly w: number;             /* screen pixels, after scaling */
  readonly h: number;
  readonly hotX: number;          /* screen pixels, after scaling */
  readonly hotY: number;
  readonly rgba: Uint8ClampedArray<ArrayBuffer>;
}

/* Planar sprite bits and its own colours, expanded to RGBA at the scale the
   screen wants.  Colour 0 is transparent and has no entry in `rgb`, which is
   why the palette is one short and every lookup is index-1. */
export function pointerImage(
  width: number, height: number, depth: number,
  xScale: number, yScale: number,
  hotX: number, hotY: number,
  rgb: Uint8Array, bits: Uint8Array,
): PointerImage {
  const rowBytes = Math.floor((width + 15) / 16) * 2;
  const plane = rowBytes * height;
  const w = width * xScale;
  const h = height * yScale;
  const out = new Uint8ClampedArray(w * h * 4);

  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const byte = (x >> 3);
      const bit = 7 - (x & 7);

      let idx = 0;
      for (let p = 0; p < depth; p++) {
        if ((bits[p * plane + y * rowBytes + byte] >> bit) & 1) idx |= 1 << p;
      }
      if (idx === 0) continue;          /* transparent */

      const r = rgb[(idx - 1) * 3];
      const g = rgb[(idx - 1) * 3 + 1];
      const b = rgb[(idx - 1) * 3 + 2];

      /* Whole-number duplication, which is what the hardware does: a sprite
         pixel IS two screen pixels on a hires screen, not one interpolated
         across two. */
      for (let dy = 0; dy < yScale; dy++) {
        for (let dx = 0; dx < xScale; dx++) {
          const o = (((y * yScale + dy) * w) + (x * xScale + dx)) * 4;
          out[o] = r; out[o + 1] = g; out[o + 2] = b; out[o + 3] = 255;
        }
      }
    }
  }

  return { w, h, hotX: hotX * xScale, hotY: hotY * yScale, rgba: out };
}

/* The built-in arrow, in the same shape, for a viewer that has not been told
   one.  Scale 1: it is drawn in screen pixels and always was. */
export function defaultPointer(): PointerImage {
  return {
    w: POINTER_W, h: POINTER_H, hotX: HOT_X, hotY: HOT_Y, rgba: pointerRGBA(),
  };
}
