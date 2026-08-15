/*
 * Input, produced now and consumed later.
 *
 * The far end writes these into input.device, so a word that goes out is a
 * real event on a real Amiga.  The pointer is still drawn locally as well as
 * sent, which is deliberate: the local one moves at compositor rate and the
 * Amiga's at frame rate, and the gap between the two IS the latency, made
 * visible rather than hidden.
 *
 * Words, not binary.  Everything here is control -- the binary channel is the
 * framebuffer and nothing else -- and a word is a thing you can read in a log
 * next to the frame it did or did not cause.
 *
 *   m  X Y BUTTONS     pointer, screen pixels.  1 left, 2 right, 4 middle,
 *                      which is the browser's own bitmask and also the order
 *                      IEQUALIFIER_ puts them in.
 *   w  DX DY           wheel, in notches
 *   kd RAW QUAL        key down, Amiga rawkey and qualifier bits
 *   ku RAW QUAL        key up
 *
 * Moves are coalesced to one animation frame.  A trackpad produces well over
 * 100 events a second and an Amiga cannot act on more than it can redraw, so
 * the rate is pinned to the rate the person can see -- and the pointer is
 * still drawn on the event itself, so coalescing costs nothing visible.
 *
 * Keys are only taken while the screen has focus.  Grabbing Tab and the
 * arrows from the whole document would make the rest of the page unusable
 * with a keyboard, and the focus ring is what says which of the two machines
 * is currently being typed at.
 *
 * SPDX-License-Identifier: MIT
 */

import { qualifiers, rawkeyOf } from "./rawkey";
import type { View } from "./view";

export interface InputSink {
  /* Where a word goes.  Separate from the log so a session that is not
     connected still shows what it would have sent. */
  send: (w: string) => void;
  log: (w: string) => void;
}

/*
 * Keys the browser is not going to give up, and asking for them is worse than
 * not having them: Cmd-W closes the tab whatever anybody prefers, and a page
 * that swallows Cmd-R and then fails to reload is a page that has to be
 * killed.  Ctrl and Amiga-key combinations that are NOT these do go through.
 */
const BROWSER_KEEPS = new Set(["KeyW", "KeyR", "KeyT", "KeyN", "KeyQ"]);

export function attachInput(view: View, stage: HTMLElement, sink: InputSink): {
  detach: () => void;
} {
  let pending: { x: number; y: number; b: number } | null = null;
  let scheduled = 0;
  let buttons = 0;
  /* Where the pointer was last known to be, in Amiga pixels.  A release that
     arrives with no usable coordinate -- a cancelled gesture, a lost focus --
     still has to say WHERE the button came up, and this is that. */
  let last = { x: 0, y: 0 };

  const flush = () => {
    scheduled = 0;
    const p = pending;
    pending = null;
    if (p === null) return;
    const w = "m " + p.x + " " + p.y + " " + p.b;
    sink.send(w);
    sink.log(w);
  };

  /* While anything is held the coordinate is clamped rather than dropped: a
     drag owns the pointer until the button comes up, and it has to be able to
     reach the edge of the Amiga's screen. */
  const at = (e: MouseEvent) => view.toNative(e.clientX, e.clientY, buttons !== 0);

  const send = (x: number, y: number, b: number) => {
    const w = "m " + x + " " + y + " " + b;
    sink.send(w);
    sink.log(w);
  };

  const onMove = (e: PointerEvent) => {
    const p = at(e);
    if (p === null) { view.clearPointer(); return; }

    /* Drawn on the event and sent on the frame: the two halves of the
       latency story, deliberately not the same rate. */
    view.movePointer(p.x, p.y);
    last = p;

    pending = { x: p.x, y: p.y, b: buttons };
    if (scheduled === 0) scheduled = requestAnimationFrame(flush);
  };

  const onDownButton = (e: PointerEvent) => {
    const p = at(e);
    if (p === null) return;
    e.preventDefault();
    stage.focus();
    /*
     * THE POINTER IS CAPTURED FOR THE WHOLE OF A DRAG.
     *
     * Without this, a press inside the canvas and a release anywhere else --
     * off the edge, over the page furniture, in another window -- never
     * produces a pointerup on this element, and the Amiga is left believing
     * the button is still down.  That is not a cosmetic fault: Intuition and
     * Workbench hold the screen's layer lock for as long as a button is down,
     * so a lost release leaves the machine behaving as if somebody were
     * holding the mouse forever.
     */
    try { stage.setPointerCapture(e.pointerId); } catch { /* not fatal */ }
    buttons = e.buttons;
    last = p;
    /* Not coalesced.  A click is the event whose timing is the whole
       question, and holding it back to the next frame would be adding the
       latency this page exists to measure. */
    pending = null;
    send(p.x, p.y, buttons);
  };

  const onUpButton = (e: PointerEvent) => {
    const p = at(e);
    e.preventDefault();
    buttons = e.buttons;
    pending = null;
    if (p !== null) last = p;
    send(last.x, last.y, buttons);
    if (buttons === 0) {
      try { stage.releasePointerCapture(e.pointerId); } catch { /* gone */ }
    }
  };

  /*
   * Every other way a press can end without a release: the capture being
   * taken away, the gesture cancelled, the tab losing focus mid-drag.  All of
   * them send the buttons up, because the one state this must never leave
   * behind on the Amiga is a button that is down and never coming back.
   */
  const releaseAll = () => {
    if (buttons === 0) return;
    buttons = 0;
    pending = null;
    const p = last;
    send(p.x, p.y, 0);
  };

  const onWheel = (e: WheelEvent) => {
    if (at(e) === null) return;
    e.preventDefault();
    const w = "w " + Math.sign(e.deltaX) + " " + Math.sign(e.deltaY);
    sink.send(w);
    sink.log(w);
  };

  const onCancel = () => { releaseAll(); };

  /* Only the drawn pointer goes.  The buttons do NOT: leaving the canvas with
     one held is the normal middle of a drag, and releasing here would drop
     the window the person is still moving. */
  const onLeave = () => { if (buttons === 0) view.clearPointer(); };

  /* The right button is the Amiga's menu button and holding it is how a menu
     stays up, so the browser's context menu cannot have it. */
  const onContext = (e: Event) => { e.preventDefault(); };

  const key = (e: KeyboardEvent, down: boolean) => {
    if (document.activeElement !== stage) return;
    if ((e.metaKey || e.ctrlKey) && BROWSER_KEEPS.has(e.code)) return;

    const raw = rawkeyOf(e);
    if (raw === null) return;

    e.preventDefault();
    const w = (down ? "kd " : "ku ") + raw + " " + qualifiers(e);
    sink.send(w);
    sink.log(w);
  };

  const onDown = (e: KeyboardEvent) => key(e, true);
  const onUp = (e: KeyboardEvent) => key(e, false);

  stage.addEventListener("pointermove", onMove);
  stage.addEventListener("pointerdown", onDownButton);
  stage.addEventListener("pointerup", onUpButton);
  stage.addEventListener("pointercancel", onCancel);
  stage.addEventListener("lostpointercapture", onCancel);
  stage.addEventListener("pointerleave", onLeave);
  addEventListener("blur", releaseAll);
  stage.addEventListener("wheel", onWheel, { passive: false });
  stage.addEventListener("contextmenu", onContext);
  addEventListener("keydown", onDown);
  addEventListener("keyup", onUp);

  return {
    detach: () => {
      if (scheduled !== 0) cancelAnimationFrame(scheduled);
      releaseAll();
      stage.removeEventListener("pointermove", onMove);
      stage.removeEventListener("pointerdown", onDownButton);
      stage.removeEventListener("pointerup", onUpButton);
      stage.removeEventListener("pointercancel", onCancel);
      stage.removeEventListener("lostpointercapture", onCancel);
      stage.removeEventListener("pointerleave", onLeave);
      removeEventListener("blur", releaseAll);
      stage.removeEventListener("wheel", onWheel);
      stage.removeEventListener("contextmenu", onContext);
      removeEventListener("keydown", onDown);
      removeEventListener("keyup", onUp);
    },
  };
}
