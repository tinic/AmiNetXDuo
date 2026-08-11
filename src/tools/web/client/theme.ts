/*
 * How it looks: Workbench 2.x/3.x, and the palette is Commodore's own.
 *
 * WHERE THESE NUMBERS COME FROM
 *
 *   Not from a fan page and not from memory -- both disagree with themselves,
 *   and the one set that turns up in searches (10,8,6 / 0,0,2 / 15,15,15 /
 *   7,7,9) is the A2024 GREY-SCALE monitor palette out of the Style Guide,
 *   which is a different thing.
 *
 *   These are read out of the ROM.  Kickstart holds the default Workbench
 *   screen colours as a table of 12-bit words immediately after the string
 *   "Workbench Screen":
 *
 *     $ python3 -c 'd=open(ROM,"rb").read(); i=d.find(b"Workbench Screen");
 *                   print(d[i+16:i+32].hex(" ",2))'
 *
 *     0aaa 0000 0fff 068b 0e44 05d5 004d 0e90
 *
 *   Identical in Kickstart 2.04 (37.175, at 0x6d28a) and 3.1 (40.68, at
 *   0x6895e), which is what makes it "2.x/3.x" and not one release's idea.
 *   Twelve bits to twenty-four by repeating the nibble, which is what the
 *   hardware does: $A becomes $AA.
 *
 *     pen 0  $0AAA  #AAAAAA  grey    background, and the Shell's ground
 *     pen 1  $0000  #000000  black   text
 *     pen 2  $0FFF  #FFFFFF  white   shine, the top-left of every bevel
 *     pen 3  $068B  #6688BB  blue    fill: title bars and selection
 *     pen 4  $0E44  #EE4444  red
 *     pen 5  $05D5  #55DD55  green
 *     pen 6  $004D  #0044DD  blue
 *     pen 7  $0E90  #EE9900  orange
 *
 * WHY THAT IS THE ANSI PALETTE TOO
 *
 *   On an Amiga, SGR 30..37 select PENS 0..7 -- the colour names are the pen
 *   numbers, which is why `Echo "*e[33m"` is blue on a Workbench screen and
 *   not yellow.  So the eight above go straight into the eight ANSI slots and
 *   AmigaDOS colour output comes out the colour AmigaDOS meant.
 *
 * IS IT READABLE
 *
 *   Black on #AAAAAA is 9.0:1, which clears WCAG AAA with room to spare.  It
 *   is lower contrast than black-on-white and that is the point -- it is what
 *   the machine looked like -- but nothing here is a squint.  The one real
 *   cost is that pen 0 as a FOREGROUND (`*e[30m`) is invisible against the
 *   ground, which is equally true on the Amiga.
 *
 * THE BRIGHTS ARE THE SAME EIGHT
 *
 *   A Workbench screen has eight pens and no bright variants, so 90..97 are
 *   90..97.  Bold is then handled as bold -- see drawBoldTextInBrightColors
 *   in main.ts -- which is what an Amiga does too: bold there is a heavier
 *   glyph and never a change of pen.  There is a real bold face embedded for
 *   it rather than a browser smearing the regular one.
 *
 * SPDX-License-Identifier: MIT
 */

import type { ITheme } from "@xterm/xterm";

/* The eight, by pen number, so the mapping below can be read against the ROM
   dump in the comment. */
export const PEN = [
  "#aaaaaa",
  "#000000",
  "#ffffff",
  "#6688bb",
  "#ee4444",
  "#55dd55",
  "#0044dd",
  "#ee9900",
] as const;

export const THEME: ITheme = {
  background: PEN[0],
  foreground: PEN[1],

  /* The Amiga console's cursor is the character in reverse, not a coloured
     block: black on the ground, and the glyph under it in the ground colour. */
  cursor: PEN[1],
  cursorAccent: PEN[0],

  /* Workbench selects in pen 3 with white on it, the way a title bar reads. */
  selectionBackground: PEN[3],
  selectionForeground: PEN[2],

  /*
   * In SGR order, 30 to 37, so pen N is the colour SGR 3N selects.  Written
   * out in that order and not alphabetically: blue and yellow were swapped
   * here for an afternoon, which put `Echo "*e[33m"` on the screen in red,
   * and the ANSI slot NAMES are the thing that misleads -- on an Amiga they
   * are pen numbers and nothing else.
   */
  black: PEN[0],       /* 30 */
  red: PEN[1],         /* 31 */
  green: PEN[2],       /* 32 */
  yellow: PEN[3],      /* 33 -- blue on a Workbench screen */
  blue: PEN[4],        /* 34 -- red */
  magenta: PEN[5],     /* 35 -- green */
  cyan: PEN[6],        /* 36 -- blue */
  white: PEN[7],       /* 37 -- orange */

  brightBlack: PEN[0],
  brightRed: PEN[1],
  brightGreen: PEN[2],
  brightYellow: PEN[3],
  brightBlue: PEN[4],
  brightMagenta: PEN[5],
  brightCyan: PEN[6],
  brightWhite: PEN[7],
};

/*
 * Hack, embedded, with a system monospace behind it.
 *
 * src/tools/web/vendor/hack/, subset to printable Latin-1 and box drawing and
 * base64 into the page by tools/web/build.mjs.  There is no fetch.
 *
 * The stack behind it is not decoration.  If the @font-face fails for any
 * reason the terminal is still a terminal, and a browser falls back PER
 * CHARACTER, so anything outside the subset -- an arrow, a box-drawing
 * character we did not include -- still draws in the face named after it
 * instead of coming out as a missing-glyph box.
 */
export const FONT =
  'Hack, ui-monospace, "SF Mono", SFMono-Regular, Menlo, Consolas, ' +
  '"Cascadia Mono", "DejaVu Sans Mono", "Liberation Mono", monospace';
