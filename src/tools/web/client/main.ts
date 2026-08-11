/*
 * The client half of httpd's terminal.  See src/tools/httpterm.h for the
 * server half and what it deliberately does not protect.
 *
 * ENTIRELY SELF-CONTAINED, AND THAT IS THE REQUIREMENT
 *
 *   An Amiga serves this file over a LAN, so there is no CDN to reach, no
 *   font to fetch, no script to load and no source map to go looking for:
 *   one file, no requests after it, and it still works on a machine with no
 *   route off the network.  tools/web/build.mjs is what makes it one file and
 *   tools/ci.sh's web stage is what keeps it honest.
 *
 * SPDX-License-Identifier: MIT
 */

import { Terminal } from "@xterm/xterm";
import { FitAddon } from "@xterm/addon-fit";
import { LineEditor } from "./line";
import { Wire, type WireState } from "./wire";
import { FONT, THEME } from "./theme";

const $ = (id: string) => document.getElementById(id)!;

const term = new Terminal({
  theme: THEME,
  fontFamily: FONT,
  fontSize: 14,
  lineHeight: 1.15,
  letterSpacing: 0,
  cursorBlink: true,
  cursorStyle: "block",
  /* A session left open all afternoon has to stop growing somewhere, and five
     thousand lines is more than anyone scrolls back through by hand. */
  scrollback: 5000,
  /*
   * AmigaDOS ends a line with LF and nothing else.  A terminal reads a bare
   * LF as "down one row, same column", so without this every line of Dir
   * starts one further right than the last and the output walks off the
   * screen in a staircase.  It is not a preference: it is the difference
   * between the far side's line ending and this one's.
   */
  convertEol: true,
  allowProposedApi: false,
  macOptionIsMeta: true,
});

const fit = new FitAddon();
term.loadAddon(fit);
term.open($("term"));

/* ------------------------------------------------------------- the wire -- */

const line = new LineEditor(term, {
  onLine: (text) => wire.keys(text + "\n"),
  onBreak: () => wire.word("break"),
  onEof: () => wire.word("eof"),
  onDeadEnter: () => connect(),
});

const wire = new Wire({
  onText: (s) => line.write(s),
  onState: (state, detail) => setState(state, detail),
});

term.onData((d: string) => line.input(d));

/*
 * Two keys the terminal must NOT eat.
 *
 * Ctrl-C is a break here, which is right when nothing is selected and wrong
 * the instant something is: the browser's copy is the only copy there is.
 * Ctrl-V and Ctrl-Shift-V are the paste, which the browser does into the
 * hidden textarea and xterm turns back into one onData call.
 */
term.attachCustomKeyEventHandler((e: KeyboardEvent) => {
  if (e.type !== "keydown") return true;
  const mod = e.ctrlKey || e.metaKey;
  if (!mod) return true;
  if (e.key === "c" && term.hasSelection()) return false;
  if (e.key === "v") return false;
  if (e.key === "a" && e.metaKey) return false;      /* macOS select all */
  return true;
});

/* ------------------------------------------------------------- the bar -- */

const stateEl = $("state");
const wordEl = $("word");
const brkEl = $("brk") as HTMLButtonElement;
const eofEl = $("eof") as HTMLButtonElement;
const againEl = $("again") as HTMLButtonElement;

const WORDS: Record<WireState, string> = {
  connecting: "connecting",
  open: "connected",
  closed: "closed",
  failed: "no answer",
};

function setState(state: WireState, detail: string): void {
  const live = state === "open";

  stateEl.className = live ? "up" : state === "connecting" ? "" : "down";
  wordEl.textContent = detail && !live ? WORDS[state] + ": " + detail
                                       : WORDS[state];

  brkEl.disabled = !live;
  eofEl.disabled = !live;
  againEl.disabled = state === "connecting";

  line.setEnabled(live);

  if (state === "closed" || state === "failed") {
    /* Said in the terminal and not only in the bar, because the bar is at the
       top and the eye is at the bottom.  Enter is the fastest way back. */
    line.notice("\u001B[38;5;244m[" +
                (detail || "the session ended") +
                " -- Enter or Reconnect to start another]\u001B[0m\r\n");
  }

  if (live) term.focus();
}

function connect(): void {
  term.focus();
  wire.connect();
}

brkEl.onclick = () => { term.focus(); line.input("\u0003"); };
eofEl.onclick = () => { term.focus(); wire.word("eof"); };
againEl.onclick = () => { wire.disconnect(); connect(); };
($("clear") as HTMLButtonElement).onclick = () => { term.focus(); line.clear(); };

/* ------------------------------------------------------------- the size -- */

/*
 * Refit on anything that changes the box, and NOT on a timer.  The observer
 * catches the window, the device rotating, and the browser's own font size
 * changing under us, which a resize listener alone does not.
 *
 * Nothing is told about the new size: a DOS pipe has no window size to set,
 * so the far side formats for whatever it always formats for and the columns
 * here are purely how much of that is readable without wrapping.
 */
function refit(): void {
  try { fit.fit(); } catch { /* not laid out yet */ }
}

new ResizeObserver(refit).observe($("term"));
addEventListener("resize", refit);
document.fonts?.ready.then(refit);
refit();

/* ------------------------------------------------------------ the start -- */

$("host").textContent = location.host;
connect();
