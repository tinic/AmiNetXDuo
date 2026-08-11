/*
 * How it looks.
 *
 * A GOOD TERMINAL FIRST, AN AMIGA SECOND
 *
 *   The nostalgic answer is topaz, and topaz is a bitmap font drawn for 640
 *   pixels across an interlaced PAL screen.  On anything made since it is
 *   either blurred or enormous, and the page would also have to carry it,
 *   which is a font file this machine may have no route to fetch and no
 *   reason to embed.  So the type is the reader's own system monospace and
 *   the Amiga is in the PALETTE, where it costs nothing and reads fine at
 *   any size.
 *
 *   The colours are the Workbench ones pulled towards a dark ground: the 3.1
 *   four-pen palette is grey, black, white and that blue, and AmigaDOS pens
 *   0..3 are exactly those, so `Echo "*e[33mblue"` on the far side comes out
 *   blue here.  The remaining twelve are a conventional 16-colour set, since
 *   anything that emits them is not an AmigaDOS command and expects ANSI.
 *
 * SPDX-License-Identifier: MIT
 */

import type { ITheme } from "@xterm/xterm";

export const THEME: ITheme = {
  background: "#0d1017",
  foreground: "#c8cede",
  cursor: "#f0a423",
  cursorAccent: "#0d1017",
  selectionBackground: "#2c4a7c",
  selectionForeground: "#eef1f8",

  /* Pens 0..3 are Workbench's own: the grey, the black, the white and the
     blue, in the order AmigaDOS numbers them. */
  black: "#0d1017",
  red: "#d4553f",
  green: "#5fb37a",
  yellow: "#f0a423",
  blue: "#5f8fd6",
  magenta: "#b57edc",
  cyan: "#56aec2",
  white: "#c8cede",

  brightBlack: "#5a6274",
  brightRed: "#ef6f57",
  brightGreen: "#7fd39a",
  brightYellow: "#ffc255",
  brightBlue: "#82adf0",
  brightMagenta: "#d09aff",
  brightCyan: "#79cddf",
  brightWhite: "#ffffff",
};

/*
 * No @font-face, so this is a stack and not a name: the first family the
 * reader's machine actually has.  Every entry is a monospace face that ships
 * with an operating system, and the generic keyword at the end is what makes
 * the list a preference rather than a requirement.
 */
export const FONT =
  'ui-monospace, "SF Mono", SFMono-Regular, Menlo, Consolas, ' +
  '"Cascadia Mono", "DejaVu Sans Mono", "Liberation Mono", monospace';
