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
