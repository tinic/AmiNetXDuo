/*
 * KeyboardEvent.code to Amiga rawkey, as a table.
 *
 * The far side of this is IECLASS_RAWKEY, so what has to go down the wire is
 * the code the keyboard controller produces and the qualifier bits beside it
 * -- not a character.  A character is what a keymap makes of a rawkey and the
 * Amiga has its own, which is the point: a Norwegian Workbench with a
 * Norwegian keymap gets its own letters out of the same code, and a viewer
 * that sent "ø" would be second-guessing it.
 *
 * `code` and not `key`, for the same reason.  KeyboardEvent.key is already
 * through the browser's keymap and Shift; KeyboardEvent.code is the physical
 * key, which is the thing a rawkey is.
 *
 * Codes are the standard US positions from the Amiga hardware reference
 * appendix.  The gaps in the table -- 0x0E, 0x1C, 0x2C, 0x3B -- are gaps on
 * the machine too and are not omissions here.
 *
 * Nothing consumes this yet: the Amiga side has no input injection.  The
 * events are produced and shown so the round trip can be looked at before
 * there is anything to complete it.
 *
 * SPDX-License-Identifier: MIT
 */

export const RAWKEY: Readonly<Record<string, number>> = {
  Backquote: 0x00,
  Digit1: 0x01, Digit2: 0x02, Digit3: 0x03, Digit4: 0x04, Digit5: 0x05,
  Digit6: 0x06, Digit7: 0x07, Digit8: 0x08, Digit9: 0x09, Digit0: 0x0a,
  Minus: 0x0b, Equal: 0x0c, Backslash: 0x0d,
  Numpad0: 0x0f,

  KeyQ: 0x10, KeyW: 0x11, KeyE: 0x12, KeyR: 0x13, KeyT: 0x14, KeyY: 0x15,
  KeyU: 0x16, KeyI: 0x17, KeyO: 0x18, KeyP: 0x19,
  BracketLeft: 0x1a, BracketRight: 0x1b,
  Numpad1: 0x1d, Numpad2: 0x1e, Numpad3: 0x1f,

  KeyA: 0x20, KeyS: 0x21, KeyD: 0x22, KeyF: 0x23, KeyG: 0x24, KeyH: 0x25,
  KeyJ: 0x26, KeyK: 0x27, KeyL: 0x28,
  Semicolon: 0x29, Quote: 0x2a, IntlBackslash: 0x2b,
  Numpad4: 0x2d, Numpad5: 0x2e, Numpad6: 0x2f,

  IntlRo: 0x30,
  KeyZ: 0x31, KeyX: 0x32, KeyC: 0x33, KeyV: 0x34, KeyB: 0x35, KeyN: 0x36,
  KeyM: 0x37,
  Comma: 0x38, Period: 0x39, Slash: 0x3a,
  NumpadDecimal: 0x3c, Numpad7: 0x3d, Numpad8: 0x3e, Numpad9: 0x3f,

  Space: 0x40, Backspace: 0x41, Tab: 0x42, NumpadEnter: 0x43, Enter: 0x44,
  Escape: 0x45, Delete: 0x46,
  NumpadSubtract: 0x4a,
  ArrowUp: 0x4c, ArrowDown: 0x4d, ArrowRight: 0x4e, ArrowLeft: 0x4f,

  F1: 0x50, F2: 0x51, F3: 0x52, F4: 0x53, F5: 0x54,
  F6: 0x55, F7: 0x56, F8: 0x57, F9: 0x58, F10: 0x59,
  NumpadDivide: 0x5c, NumpadMultiply: 0x5d, NumpadAdd: 0x5e,
  /* Help.  A PC keyboard has no Help key and Insert is where an Amiga
     emulator has always put it. */
  Insert: 0x5f,

  ShiftLeft: 0x60, ShiftRight: 0x61, CapsLock: 0x62,
  ControlLeft: 0x63, ControlRight: 0x63,
  AltLeft: 0x64, AltRight: 0x65,
  MetaLeft: 0x66, MetaRight: 0x67,
};

/* IEQUALIFIER_*, the bits an InputEvent carries beside the code. */
export const QUAL_LSHIFT = 0x0001;
export const QUAL_RSHIFT = 0x0002;
export const QUAL_CONTROL = 0x0008;
export const QUAL_LALT = 0x0010;
export const QUAL_RALT = 0x0020;
export const QUAL_LCOMMAND = 0x0040;
export const QUAL_RCOMMAND = 0x0080;

export function qualifiers(e: KeyboardEvent): number {
  let q = 0;
  /* The browser reports Shift as one bit, not two, except on the event that
     pressed one of them -- where `code` says which.  Left is the honest
     default for everything else. */
  if (e.shiftKey) q |= e.code === "ShiftRight" ? QUAL_RSHIFT : QUAL_LSHIFT;
  if (e.ctrlKey) q |= QUAL_CONTROL;
  if (e.altKey) q |= e.code === "AltRight" ? QUAL_RALT : QUAL_LALT;
  if (e.metaKey) q |= e.code === "MetaRight" ? QUAL_RCOMMAND : QUAL_LCOMMAND;
  return q;
}

export function rawkeyOf(e: KeyboardEvent): number | null {
  const v = RAWKEY[e.code];
  return v === undefined ? null : v;
}
