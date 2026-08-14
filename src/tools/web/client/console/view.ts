/*
 * The screen on the page: two canvases, one of them the Amiga's.
 *
 * The framebuffer canvas is the screen's NATIVE size -- 640x256 backing store
 * for a 640x256 screen -- and CSS stretches it.  That is what makes
 * nearest-neighbour free: `image-rendering: pixelated` is a GPU sampler, so
 * scaling costs nothing per frame and a 3x window is the same work as a 1x
 * one.  Scaling in JS instead would multiply the decode by the zoom factor
 * and be the most expensive thing in the viewer by several times.
 *
 * The second canvas is the pointer, above the first and cleared to
 * transparent.  It is separate because it moves at mouse rate and the screen
 * moves at frame rate, and compositing them would redraw one at the other's
 * pace; see pointer.ts for why it is drawn locally at all.
 *
 * Aspect is applied to the CSS box only -- the backing store stays square
 * pixels.  A 640x256 Workbench displayed 640x256 is Workbench squashed flat,
 * and the correction is a stretch of the element, so it is a style change and
 * not a re-decode.  See pixelAspect() in planar.ts for how it is inferred.
 *
 * SPDX-License-Identifier: MIT
 */

import {
  decodeRectInto,
  pixelAspect,
  type Screen,
} from "./planar";
import { HOT_X, HOT_Y, POINTER_H, POINTER_W, pointerRGBA } from "./pointer";

export type Aspect = "auto" | "square";
export type Zoom = number | "fit";

export interface Rect { x0: number; y0: number; x1: number; y1: number }

export class View {
  private readonly box: HTMLElement;
  private readonly fb: HTMLCanvasElement;
  private readonly ptr: HTMLCanvasElement;
  private readonly fbCtx: CanvasRenderingContext2D;
  private readonly ptrCtx: CanvasRenderingContext2D;

  private screen: Screen = { width: 0, height: 0, depth: 1, bytesPerRow: 0 };
  private palette: Uint32Array = new Uint32Array(2);
  private image: ImageData | null = null;
  private words: Uint32Array | null = null;

  private aspect: Aspect = "auto";
  private zoom: Zoom = "fit";

  private readonly cursor: ImageData;
  private drawnAt: { x: number; y: number } | null = null;

  /* The last decode, in milliseconds, for the bar.  Kept as a number and not
     a rolling average: an average hides the frame that took 40 ms, which is
     the only frame anybody is looking for. */
  lastDecodeMs = 0;

  constructor(box: HTMLElement) {
    this.box = box;

    this.fb = document.createElement("canvas");
    this.fb.id = "fb";
    this.ptr = document.createElement("canvas");
    this.ptr.id = "ptr";
    box.append(this.fb, this.ptr);

    /* alpha:false on the framebuffer: it is opaque by construction and the
       compositor can skip a blend per frame for it.  The overlay is the
       opposite and keeps its alpha. */
    this.fbCtx = this.fb.getContext("2d", { alpha: false })!;
    this.ptrCtx = this.ptr.getContext("2d")!;

    this.cursor = new ImageData(pointerRGBA(), POINTER_W, POINTER_H);

    new ResizeObserver(() => this.relayout()).observe(box.parentElement ?? box);
  }

  get geometry(): Screen { return this.screen; }

  setScreen(screen: Screen, palette: Uint32Array): void {
    this.screen = screen;
    this.palette = palette;

    this.fb.width = screen.width;
    this.fb.height = screen.height;
    this.ptr.width = screen.width;
    this.ptr.height = screen.height;

    this.image = this.fbCtx.createImageData(screen.width, screen.height);
    this.words = new Uint32Array(this.image.data.buffer);
    this.drawnAt = null;

    this.relayout();
  }

  setPalette(palette: Uint32Array): void {
    this.palette = palette;
  }

  /*
   * Decode and put.  `rect` is the damaged area when one is known; a full
   * frame is the default because the player has no damage information and
   * the whole screen at 640x480x3 is under a millisecond anyway.
   */
  paint(planes: Uint8Array, off: number, rect?: Rect): void {
    const img = this.image;
    const words = this.words;
    if (img === null || words === null) return;

    const r = rect ?? {
      x0: 0, y0: 0, x1: this.screen.width, y1: this.screen.height,
    };
    if (r.x1 <= r.x0 || r.y1 <= r.y0) return;

    const t0 = performance.now();
    decodeRectInto(this.screen, planes, off, this.palette, words,
                   r.x0, r.y0, r.x1, r.y1);
    /*
     * The dirty-rectangle form of putImageData, so the browser uploads the
     * damaged strip rather than the screen.  It reads the rectangle out of
     * the SAME full-size ImageData, which is why the decode above writes at
     * the screen's stride and not the rectangle's.
     */
    this.fbCtx.putImageData(img, 0, 0, r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0);
    this.lastDecodeMs = performance.now() - t0;
  }

  setAspect(a: Aspect): void { this.aspect = a; this.relayout(); }
  setZoom(z: Zoom): void { this.zoom = z; this.relayout(); }

  scaleOf(): { x: number; y: number } {
    const a = this.aspect === "auto"
      ? pixelAspect(this.screen)
      : { x: 1, y: 1 };

    if (this.zoom !== "fit") {
      return { x: a.x * this.zoom, y: a.y * this.zoom };
    }

    /* Fit to the stage, which is the parent: the box itself is the thing
       being sized and measuring it would be measuring the answer. */
    const stage = this.box.parentElement;
    if (stage === null || this.screen.width === 0) return a;

    const k = Math.min(
      stage.clientWidth / (this.screen.width * a.x),
      stage.clientHeight / (this.screen.height * a.y),
    );
    /* Never below 1:1 of one Amiga pixel to one CSS pixel in the smaller
       axis: past that the picture is a resample and every one-pixel
       Workbench bevel starts vanishing on alternate rows. */
    return { x: a.x * Math.max(k, 0.1), y: a.y * Math.max(k, 0.1) };
  }

  relayout(): void {
    if (this.screen.width === 0) return;
    const s = this.scaleOf();
    const w = Math.round(this.screen.width * s.x);
    const h = Math.round(this.screen.height * s.y);
    this.box.style.width = w + "px";
    this.box.style.height = h + "px";
  }

  /*
   * Client coordinates to Amiga pixels, or null off the screen.  The
   * rectangle is read every time on purpose: it changes with the zoom, the
   * window, and the page scrolling, and caching it is how a viewer ends up
   * clicking somewhere the person did not.  Dividing by the RENDERED width is
   * also what makes the aspect correction free -- a 640x256 screen shown
   * 640x512 has r.height 512, and the ratio takes the 2x back out.
   *
   * `clamp` is for a drag.  A button held down owns the pointer until it comes
   * up, wherever the mouse has got to, and an Amiga drag has to be able to
   * reach the edge of the screen: dropping the coordinate because it is one
   * pixel outside the canvas is how a window stops following the mouse
   * half way through being moved.
   */
  toNative(clientX: number, clientY: number, clamp = false):
      { x: number; y: number } | null {
    const r = this.fb.getBoundingClientRect();
    if (r.width === 0 || r.height === 0) return null;

    let x = Math.floor((clientX - r.left) / r.width * this.screen.width);
    let y = Math.floor((clientY - r.top) / r.height * this.screen.height);

    if (x < 0 || y < 0 || x >= this.screen.width || y >= this.screen.height) {
      if (!clamp) return null;
      x = Math.min(Math.max(x, 0), this.screen.width - 1);
      y = Math.min(Math.max(y, 0), this.screen.height - 1);
    }
    return { x, y };
  }

  /*
   * The pointer, at Amiga pixel coordinates.  Cleared from where it was and
   * put where it is: two rectangles a mouse move rather than a canvas the
   * size of the screen.
   */
  movePointer(x: number, y: number): void {
    const was = this.drawnAt;
    if (was !== null && was.x === x && was.y === y) return;
    this.clearPointer();
    this.ptrCtx.putImageData(this.cursor, x - HOT_X, y - HOT_Y);
    this.drawnAt = { x, y };
  }

  clearPointer(): void {
    const was = this.drawnAt;
    if (was === null) return;
    this.ptrCtx.clearRect(was.x - HOT_X, was.y - HOT_Y, POINTER_W, POINTER_H);
    this.drawnAt = null;
  }
}
