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

/* ------------------------------------------------------------ the input -- */

/*
 * WHICH SIDE EDITS THE LINE.  A MODE, NOT AN ASSUMPTION.
 *
 *   line   readline and local echo here; one binary frame per LINE.  This is
 *          the only mode that works against what the server is TODAY: the far
 *          side is a DOS pipe, nothing on it echoes and nothing edits, so a
 *          client that did neither would show you an empty screen while you
 *          typed.
 *
 *   char   nothing here echoes, edits or interprets: every keystroke the
 *          terminal component produces goes down the same binary frames as
 *          the bytes it already is.  This is what a real AmigaOS console
 *          handler on the far side needs -- raw mode, its own echo, cursor
 *          keys, Ed and More -- and it needs no change to the protocol,
 *          because the protocol was always bytes.
 *
 * ?input=char selects it.  It is a real switch and not a note in a comment,
 * so the day the far side grows a console the client is already there; today
 * it is also the honest way to see what the pipe does with a keystroke.
 */
type InputMode = "line" | "char";

const INPUT: InputMode =
  new URLSearchParams(location.search).get("input") === "char"
    ? "char"
    : "line";

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

/*
 * The one fork.  In char mode the keystroke goes to the socket as the bytes
 * xterm.js already made of it -- arrows, function keys, Del, Help, Ctrl-
 * anything -- with nothing here reading it first.  Ctrl-C is the exception
 * and is described below.
 */
term.onData((d: string) => {
  if (INPUT === "line") line.input(d);
  else wire.keys(d);
});

/*
 * What the browser keeps, and it is only what the browser has to keep.
 *
 * Ctrl-C with a selection is the copy, and the browser's is the only copy
 * there is.  With nothing selected it is a break in both modes -- Ctrl-C on
 * an Amiga is a signal and not a byte, so even a console handler wants the
 * text frame and not 0x03 in the stream.
 *
 * Ctrl-V and Cmd-A are the browser's too.  Everything else, in either mode,
 * reaches the terminal component: a key withheld here is a key char mode
 * would have to get back.
 */
term.attachCustomKeyEventHandler((e: KeyboardEvent) => {
  if (e.type !== "keydown") return true;
  if (!e.ctrlKey && !e.metaKey) return true;
  if (e.key === "c" && term.hasSelection()) return false;
  if (e.key === "v") return false;
  if (e.key === "a" && e.metaKey) return false;      /* macOS select all */
  if (e.key === "c" && INPUT === "char") { wire.word("break"); return false; }
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
  refused: "refused",
};

/* What the server will not say over a WebSocket, said here.  Its only
   refusal of a correct client is that the Shell is taken: one session at a
   time, and the second upgrade gets a 503 the browser does not pass on. */
const REFUSED =
  "no session -- the terminal takes one at a time, and something else has it";

function setState(state: WireState, detail: string): void {
  const live = state === "open";

  stateEl.className = live ? "up" : state === "connecting" ? "" : "down";
  wordEl.textContent = detail && !live ? WORDS[state] + ": " + detail
                                       : WORDS[state];

  brkEl.disabled = !live;
  eofEl.disabled = !live;
  againEl.disabled = state === "connecting";

  /* In char mode the editor never draws, so it is never enabled: the far
     side owns the cursor from the first keystroke. */
  line.setEnabled(live && INPUT === "line");

  if (state === "closed" || state === "refused") {
    /* Said in the terminal and not only in the bar, because the bar is at the
       top and the eye is at the bottom.  Enter is the fastest way back. */
    line.notice("\u001B[38;5;244m[" +
                (state === "refused" ? REFUSED
                                     : detail || "the session ended") +
                " -- Enter or Reconnect to try again]\u001B[0m\r\n");
  }

  if (live) term.focus();
}

function connect(): void {
  term.focus();
  wire.connect();
}

brkEl.onclick = () => {
  term.focus();
  /* Through the editor in line mode, so the half-typed line goes with it
     and the ^C is drawn; straight to the wire when there is no editor. */
  if (INPUT === "line") line.input("\u0003");
  else wire.word("break");
};
eofEl.onclick = () => { term.focus(); wire.word("eof"); };
againEl.onclick = () => { wire.disconnect(); connect(); };
($("clear") as HTMLButtonElement).onclick = () => { term.focus(); line.clear(); };

/* ------------------------------------------------------------- the size -- */

/*
 * Refit on anything that changes the box, and NOT on a timer.  The observer
 * catches the window, the device rotating, and the browser's own font size
 * changing under us, which a resize listener alone does not.
 */
function refit(): void {
  try { fit.fit(); } catch { /* not laid out yet */ }
}

/*
 * How big the window is, kept where somebody can reach it.
 *
 * A DOS pipe has no window size, so nothing is sent today: the far side
 * formats for whatever it always formats for, and the columns here are only
 * how much of that fits without wrapping.  A console handler DOES have one --
 * it is what makes More paginate and Ed use the whole screen -- so this is
 * the one place that learns the new size, and the one place a frame carrying
 * it would be sent from.  Reading it back off the component at the time
 * would work; having it arrive as an event is what makes the send a line
 * rather than a search.
 */
let size = { cols: term.cols, rows: term.rows };

term.onResize((s) => {
  size = { cols: s.cols, rows: s.rows };
  $("size").textContent = size.cols + "x" + size.rows;
});

new ResizeObserver(refit).observe($("term"));
addEventListener("resize", refit);
document.fonts?.ready.then(refit);
refit();
$("size").textContent = size.cols + "x" + size.rows;

/* ------------------------------------------------------------ the start -- */

$("host").textContent = location.host;
connect();
