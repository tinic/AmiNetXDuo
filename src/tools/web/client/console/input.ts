/*
 * Input, produced now and consumed later.
 *
 * There is nothing on the Amiga side to receive any of this yet -- the
 * injection into input.device does not exist -- and the events are still
 * generated, sent and shown, because the thing worth looking at before it
 * works is the latency: how long after a mouse move the word leaves, and how
 * long after that anything changes on the screen.  A viewer that draws its
 * own pointer answers the first half of that at compositor rate and leaves
 * the second half visible, which is the point.
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
  moves: () => number;
} {
  let pending: { x: number; y: number; b: number } | null = null;
  let scheduled = 0;
  let moves = 0;
  let buttons = 0;

  const flush = () => {
    scheduled = 0;
    const p = pending;
    pending = null;
    if (p === null) return;
    const w = "m " + p.x + " " + p.y + " " + p.b;
    sink.send(w);
    sink.log(w);
  };

  const at = (e: MouseEvent) => view.toNative(e.clientX, e.clientY);

  const onMove = (e: PointerEvent) => {
    const p = at(e);
    if (p === null) { view.clearPointer(); return; }

    /* Drawn on the event and sent on the frame: the two halves of the
       latency story, deliberately not the same rate. */
    view.movePointer(p.x, p.y);
    moves++;

    pending = { x: p.x, y: p.y, b: buttons };
    if (scheduled === 0) scheduled = requestAnimationFrame(flush);
  };

  const onButton = (e: PointerEvent) => {
    const p = at(e);
    if (p === null) return;
    e.preventDefault();
    stage.focus();
    buttons = e.buttons;
    /* Not coalesced.  A click is the event whose timing is the whole
       question, and holding it back to the next frame would be adding the
       latency this page exists to measure. */
    pending = null;
    const w = "m " + p.x + " " + p.y + " " + buttons;
    sink.send(w);
    sink.log(w);
  };

  const onWheel = (e: WheelEvent) => {
    if (at(e) === null) return;
    e.preventDefault();
    const w = "w " + Math.sign(e.deltaX) + " " + Math.sign(e.deltaY);
    sink.send(w);
    sink.log(w);
  };

  const onLeave = () => { view.clearPointer(); };

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
  stage.addEventListener("pointerdown", onButton);
  stage.addEventListener("pointerup", onButton);
  stage.addEventListener("pointerleave", onLeave);
  stage.addEventListener("wheel", onWheel, { passive: false });
  stage.addEventListener("contextmenu", onContext);
  addEventListener("keydown", onDown);
  addEventListener("keyup", onUp);

  return {
    detach: () => {
      if (scheduled !== 0) cancelAnimationFrame(scheduled);
      stage.removeEventListener("pointermove", onMove);
      stage.removeEventListener("pointerdown", onButton);
      stage.removeEventListener("pointerup", onButton);
      stage.removeEventListener("pointerleave", onLeave);
      stage.removeEventListener("wheel", onWheel);
      stage.removeEventListener("contextmenu", onContext);
      removeEventListener("keydown", onDown);
      removeEventListener("keyup", onUp);
    },
    moves: () => moves,
  };
}
