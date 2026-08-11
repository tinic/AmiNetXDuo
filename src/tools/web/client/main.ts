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
 * WHICH SIDE EDITS THE LINE.  THE SERVER SAYS.
 *
 *   line   readline and local echo here; one binary frame per LINE.  What a
 *          COOKED console would be doing on the far side, done here instead
 *          because the far side is across a LAN and an echoed byte would
 *          cross it twice before the letter appeared.
 *
 *   char   nothing here echoes, edits or interprets: every keystroke the
 *          terminal component produces goes down the same binary frames as
 *          the bytes it already is.  This is RAW, and it is what ssh's
 *          password prompt, Ed and More ask the console for.
 *
 * IT USED TO BE ?input=char AND A NOTE SAYING "ONE DAY".  IT IS NOW NEGOTIATED
 *
 *   The far side is a console handler and answers ACTION_SCREEN_MODE, so it
 *   KNOWS which mode it is in and says so: `mode raw` / `mode cooked` on the
 *   word channel, once when the session opens and then on every change.  A
 *   query parameter could never have fixed the password prompt, because the
 *   program that wants the echo off is chosen after the page has loaded.
 *
 *   ?input= is kept as an OVERRIDE and nothing else: it pins the mode and
 *   ignores the server, which is how you look at what one side is doing
 *   without the other side moving.  Absent, which is the normal case, the
 *   server drives.
 */
type InputMode = "line" | "char";

const PINNED: InputMode | null = (() => {
  const q = new URLSearchParams(location.search).get("input");
  return q === "char" ? "char" : q === "line" ? "line" : null;
})();

let input: InputMode = PINNED ?? "line";

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
  onWord: (w) => heard(w),
  onState: (state, detail) => setState(state, detail),
});

/*
 * The one fork.  In char mode the keystroke goes to the socket as the bytes
 * xterm.js already made of it -- arrows, function keys, Del, Help, Ctrl-
 * anything -- with nothing here reading it first, except that ESC [ becomes
 * the 8-bit CSI the far side's programs are written against.  Ctrl-C is the
 * other exception and is described below.
 */
term.onData((d: string) => {
  if (input === "line") line.input(d);
  else wire.keys(d, true);
});

/*
 * A word from the server.  The vocabulary is in src/tools/httpterm.h and the
 * rule for one that is not below is to ignore it: that is what lets either
 * side learn a new word without the other having to.
 */
function heard(w: string): void {
  if (w === "mode raw" || w === "mode cooked") {
    if (PINNED !== null) return;      /* ?input= wins, deliberately */
    setInput(w === "mode raw" ? "char" : "line");
  }
}

/*
 * Switch modes.  The editor is stood down or up as part of it: it is what
 * draws the input line, and a line left drawn while the far side has started
 * reading keystrokes one at a time is a line on the screen that nothing will
 * ever submit.
 */
function setInput(next: InputMode): void {
  if (next === input) return;
  input = next;
  line.setEnabled(wire.open && input === "line");
}

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
  if (e.key === "c" && input === "char") { wire.word("break"); return false; }
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

const REFUSED_AGAIN =
  REFUSED + ".  Take it: the button, or add ?take=1";

/*
 * ONE AUTOMATIC RETRY AFTER A REFUSAL, AND WHY EXACTLY ONE.
 *
 * A session whose browser vanished with the network -- no close frame, no
 * FIN -- used to hold the terminal for ever, and the only cure was restarting
 * the machine.  The server now lets go of a session that has stopped
 * answering its pings, but it lets go DURING the refusal: the Shell is on its
 * way out when the 503 is written, so the answer to the next ask is the one
 * that works.  Retrying once turns that into "the page just connects".
 *
 * Once, and not a loop: a terminal genuinely held by somebody else refuses
 * every time, and a page that kept asking would be a page hammering a 14 MHz
 * machine on behalf of a person who has gone to lunch.  The second refusal is
 * shown, with the way to insist.
 */
const RETRY_MS = 1500;
let retried = false;
let retryTimer = 0;

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
  line.setEnabled(live && input === "line");

  /*
   * A new session starts COOKED unless the query pinned it, and the server
   * says so as its first word.  Resetting here as well is what covers a
   * RECONNECT into a session that was left in raw mode: the word arrives
   * either way, but between the socket opening and it arriving the page
   * would otherwise still be in the last session's mode.
   */
  if (!live && PINNED === null) input = "line";

  /* And the size, first thing, so a program that asks before anybody types
     gets the real answer rather than the 80x25 default. */
  if (live) wire.size(size.cols, size.rows);

  if (live) retried = false;

  /*
   * The retry, before anything is drawn about it.  A refusal that the server
   * answered by releasing a dead session is a refusal that will not happen
   * again, and telling somebody about it and then succeeding is worse than
   * not telling them.
   */
  if (state === "refused" && !retried) {
    retried = true;
    clearTimeout(retryTimer);
    retryTimer = setTimeout(() => wire.connect(), RETRY_MS);
    return;
  }

  if (state === "closed" || state === "refused") {
    /* Said in the terminal and not only in the bar, because the bar is at the
       top and the eye is at the bottom.  Enter is the fastest way back. */
    line.notice("\u001B[38;5;244m[" +
                (state === "refused" ? REFUSED_AGAIN
                                     : detail || "the session ended") +
                " -- Enter or Reconnect to try again]\u001B[0m\r\n");
  }

  if (live) term.focus();
}

function connect(take = false): void {
  term.focus();
  retried = false;
  wire.connect(take);
}

brkEl.onclick = () => {
  term.focus();
  /* Through the editor in line mode, so the half-typed line goes with it
     and the ^C is drawn; straight to the wire when there is no editor. */
  if (input === "line") line.input("\u0003");
  else wire.word("break");
};
eofEl.onclick = () => { term.focus(); wire.word("eof"); };
/*
 * Reconnect, and TAKE IT when the last answer was a refusal.
 *
 * One button rather than two.  A person pressing Reconnect after being told
 * somebody else has the terminal is asking for the terminal, and offering
 * them a second button to mean it is a distinction that exists only in the
 * server.  After a plain close it is an ordinary reconnect and takes nothing.
 */
againEl.onclick = () => {
  const take = wordEl.textContent !== null &&
               wordEl.textContent.indexOf("refused") === 0;
  wire.disconnect();
  connect(take);
};
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
 * How big the window is, and the far side is told.
 *
 * A console has a size and programs ask it for one -- it is what makes Ed use
 * the whole screen -- so every change goes down the wire as it happens.  Sent
 * from HERE, off the component's own event, rather than read back at the
 * moment somebody wants it: the event is the only thing that knows the
 * browser's font finished loading and the box reflowed.
 *
 * Not throttled.  Dragging a window edge produces a handful of these, each
 * one under twenty bytes, on a socket that carries a Shell's output.
 */
let size = { cols: term.cols, rows: term.rows };

term.onResize((s) => {
  size = { cols: s.cols, rows: s.rows };
  $("size").textContent = size.cols + "x" + size.rows;
  wire.size(size.cols, size.rows);
});

new ResizeObserver(refit).observe($("term"));
addEventListener("resize", refit);
document.fonts?.ready.then(refit);
refit();
$("size").textContent = size.cols + "x" + size.rows;

/* ------------------------------------------------------------ the start -- */

$("host").textContent = location.host;
connect();
