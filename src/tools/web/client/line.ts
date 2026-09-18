/*
 * Readline-style line editing, on this side of the wire.
 *
 * THIS FILE IS ONE INPUT MODE, NOT THE INPUT PATH
 *
 *   It is everything the client does INSTEAD of the far side's COOKED mode,
 *   kept in one place so that it can be switched off in one place.  With it
 *   off, the terminal component's keystrokes go to the socket as they are
 *   typed and nothing here runs -- see `input` in client/main.ts.  Nothing
 *   outside this file echoes, edits, or decides what a key means.
 *
 * WHEN IT IS ON, AND WHO DECIDES
 *
 *   The far side IS a console handler now, and it says which mode it is in:
 *   `mode cooked` turns this on, `mode raw` turns it off.  In raw mode the
 *   handler does not echo, so neither does this, which is what stops a
 *   password appearing on the screen.
 *
 *   In cooked mode the echo stays HERE rather than moving to the handler, and
 *   that is a deliberate choice about distance rather than an absence of the
 *   feature.  A real console echoes because the keyboard and the handler are
 *   the same machine; here they are a LAN apart, and every echoed byte would
 *   cross it twice before the letter appeared.  So every character you see as
 *   you type it is drawn here, and Ctrl-W has to know what a word is here.
 *
 * WHAT IT SWALLOWS, WHICH A CONSOLE WOULD NOT
 *
 *   In this mode an escape sequence with no verb below is DROPPED rather than
 *   inserted as its letters, and Ctrl-C and Ctrl-D become the two words the
 *   server takes instead of bytes.  Both are line-mode decisions.  Char mode
 *   forwards every one of them untouched, which is why it is a different
 *   path and not a flag inside this one.
 *
 * THE HARD PART IS NOT THE KEYS
 *
 *   It is that the Amiga writes whenever it likes.  A command that prints
 *   while you are half way through typing the next one would interleave its
 *   output with the echo and leave the cursor somewhere neither side agrees
 *   on.  So the input line is ERASED before remote output is written and
 *   REDRAWN after it, and the anchor it is redrawn from is read back out of
 *   the terminal's own buffer rather than counted.  Counting is what breaks
 *   the moment a line wraps.
 *
 * SPDX-License-Identifier: MIT
 */

import type { Terminal } from "@xterm/xterm";

const ESC = "\u001B";
const HIST_KEY = "aminetxduo-shell-history";

export interface LineHandlers {
  /* A finished line, without its terminator.  The caller adds one. */
  onLine: (line: string) => void;
  /* Ctrl-C and Ctrl-D: neither is a byte the Shell can be sent. */
  onBreak: () => void;
  onEof: () => void;
  /* Enter on a dead socket, which is how you ask for a new session. */
  onDeadEnter: () => void;
  /* Tab: ask the far side to complete the word before the cursor. */
  onComplete: (arg: string) => void;
  /* Reverse history search: the prompt to show, or null when it ends. */
  onSearch: (prompt: string | null) => void;
}

export class LineEditor {
  private readonly term: Terminal;
  private readonly h: LineHandlers;

  private buf = "";
  private cur = 0;

  private history: string[] = [];
  private hAt = 0;                  /* history.length means "the live line" */
  private hSaved = "";              /* the live line, parked during a recall */

  private shown = false;            /* is the input line currently drawn */
  private anchorX = 0;
  private anchorY = 0;              /* ABSOLUTE row: baseY + cursorY */

  private enabled = false;

  /* Tab completion: the matches for the current word and where we are in
     them, so a repeated Tab cycles like PowerShell; null when not cycling. */
  private cyc: { start: number; cands: string[]; idx: number } | null = null;
  private pendCompStart = 0;        /* where a pending completion replaces from */

  /* Ctrl-R reverse search: the query, the history line it found, and the line
     it interrupted, to restore on cancel; null when not searching. */
  private search:
    { query: string; matchIdx: number; saved: string; savedCur: number } | null = null;

  /* Every write goes through one chain.  term.write() is asynchronous -- the
     parser runs on its own schedule -- so reading the cursor back straight
     after a write reads where the cursor USED to be.  Serialising on the
     completion callback is what makes the anchor arithmetic true. */
  private chain: Promise<void> = Promise.resolve();

  private readonly table: [string, () => void | Promise<void>][];

  constructor(term: Terminal, h: LineHandlers) {
    this.term = term;
    this.h = h;
    this.table = this.buildTable();
    this.loadHistory();
  }

  setEnabled(on: boolean): void {
    this.enabled = on;
    if (!on) this.queue(() => this.hide());
    else this.queue(() => this.show());
  }

  /* Remote output.  Erase, write, redraw: in that order, always. */
  write(text: string): void {
    this.queue(async () => {
      const was = this.shown;
      if (was) await this.hide();
      await this.w(text);
      if (was || this.enabled) await this.show();
    });
  }

  /*
   * Something of ours rather than the Shell's.  It gets a line to itself, and
   * takes one only if the cursor is not already at the start of one: a Shell
   * that exits having printed a newline would otherwise be followed by a
   * blank row nobody wrote.
   */
  notice(text: string): void {
    this.queue(async () => {
      const was = this.shown;
      if (was) await this.hide();
      const gap = this.term.buffer.active.cursorX === 0 ? "" : "\r\n";
      await this.w(gap + text);
      if (was) await this.show();
    });
  }

  clear(): void {
    this.queue(async () => {
      const was = this.shown;
      if (was) await this.hide();
      await this.w(ESC + "[H" + ESC + "[2J" + ESC + "[3J");
      if (was) await this.show();
    });
  }

  /* Keys, as xterm.js hands them over: already a terminal encoding, and
     already batched when the source is a paste. */
  input(data: string): void {
    this.queue(() => this.take(data));
  }

  // ------------------------------------------------------------- plumbing --

  private queue(fn: () => Promise<void> | void): void {
    this.chain = this.chain.then(fn).catch(() => { /* a closed terminal */ });
  }

  private w(text: string): Promise<void> {
    return new Promise<void>((done) => this.term.write(text, () => done()));
  }

  private absRow(): number {
    const b = this.term.buffer.active;
    return b.baseY + b.cursorY;
  }

  /*
   * Put the cursor back where the line starts.  Relative moves computed from
   * where the cursor is NOW, because the only fixed point available is the
   * buffer's own absolute row -- and an absolute row index survives scrolling,
   * which is the case that a saved-cursor sequence gets wrong.
   */
  private toAnchor(): string {
    const up = this.absRow() - this.anchorY;
    let s = "";
    if (up > 0) s += ESC + "[" + up + "A";
    else if (up < 0) s += ESC + "[" + (-up) + "B";
    s += "\r";
    if (this.anchorX > 0) s += ESC + "[" + this.anchorX + "C";
    return s;
  }

  private async hide(): Promise<void> {
    if (!this.shown) return;
    /* ED 0 rather than EL: the line may have wrapped onto rows below, and the
       input line is always the last thing on the screen, so everything from
       the anchor down is ours to take back. */
    await this.w(this.toAnchor() + ESC + "[J");
    this.shown = false;
  }

  private async show(): Promise<void> {
    if (this.shown || !this.enabled) return;

    this.anchorX = this.term.buffer.active.cursorX;
    this.anchorY = this.absRow();
    this.shown = true;
    await this.draw();
  }

  /*
   * Draw the buffer and leave the cursor at `cur`.
   *
   * The trailing space is not decoration.  A terminal does not move to the
   * next row until a character is written PAST the last column, so a line
   * whose last character lands exactly on the right margin leaves the cursor
   * still on that column, and the arithmetic below would then place the caret
   * a row too high.  Writing one more character forces the wrap, and the
   * caret is walked back over it.
   */
  private async draw(): Promise<void> {
    const cols = this.term.cols;
    const start = this.anchorX;
    const end = start + this.buf.length;
    const caret = start + this.cur;

    let s = this.toAnchor() + ESC + "[J" + this.buf + " ";

    /* Where the trailing space left the cursor, in cells from the anchor. */
    const at = end + 1;
    const atRow = Math.floor(at / cols);
    const atCol = at % cols;
    const caretRow = Math.floor(caret / cols);
    const caretCol = caret % cols;

    const dy = atRow - caretRow;
    if (dy > 0) s += ESC + "[" + dy + "A";
    if (caretCol !== atCol) {
      const dx = caretCol - atCol;
      s += dx > 0 ? ESC + "[" + dx + "C" : ESC + "[" + (-dx) + "D";
    }

    await this.w(s);
  }

  private async redraw(): Promise<void> {
    if (!this.shown) return;
    await this.draw();
  }

  // ----------------------------------------------------------------- keys --

  private async take(data: string): Promise<void> {
    /* Any input but a lone Tab ends a completion cycle. */
    if (data !== "\t") this.cyc = null;

    let i = 0;

    /* A dead socket takes one key.  Anything else would build up a line
       nothing can see, behind a prompt that is not coming back. */
    if (!this.enabled) {
      if (/[\r\n]/.test(data)) this.h.onDeadEnter();
      return;
    }

    while (i < data.length) {
      const rest = data.slice(i);

      /* Ctrl-R starts, or steps to the next older match of, a reverse search
         through the history. */
      if (data.charCodeAt(i) === 0x12) {
        if (this.search === null) await this.searchStart();
        else await this.searchStep();
        i += 1;
        continue;
      }
      /* While searching, a key refines the query, cancels, or is accepted --
         the match stays on the line and the key is then handled below. */
      if (this.search !== null) {
        const n = await this.searchKey(rest);
        if (n > 0) { i += n; continue; }
        await this.searchEnd(true);
      }

      /* Enter, in either of the forms a terminal produces, and the CRLF a
         paste from a Windows editor arrives as. */
      if (rest.startsWith("\r\n")) { i += 2; await this.submit(); continue; }
      const c = data.charCodeAt(i);
      if (c === 13 || c === 10) { i += 1; await this.submit(); continue; }

      /*
       * Tab, expanded here rather than passed on.  Every other character in
       * the buffer is one cell wide and the redraw arithmetic depends on it;
       * a tab is as many cells as the column it lands on says.  Expanding to
       * the next multiple of eight keeps the two the same thing, and the far
       * side cannot tell: AmigaDOS separates command arguments on whitespace
       * and does not care which.  Dropping it, which is what happened before,
       * lost characters out of the middle of a pasted line.
       */
      if (c === 9) {
        /*
         * A Tab on its own is a completion request.  A Tab arriving inside a
         * larger chunk is part of a paste and still expands to the next tab
         * stop: the far side splits arguments on whitespace and does not care
         * which, and dropping it lost characters out of a pasted line.
         */
        if (data.length === 1) {
          if (this.cyc !== null) {
            this.cyc.idx = (this.cyc.idx + 1) % this.cyc.cands.length;
            await this.showCandidate();
          } else {
            this.requestComplete();
          }
          i += 1;
          continue;
        }
        const to = 8 - ((this.anchorX + this.cur) % 8);
        this.insert(" ".repeat(to));
        i += 1;
        await this.redraw();
        continue;
      }

      const seq = this.match(rest);
      if (seq !== null) {
        i += seq.len;
        await seq.run();
        continue;
      }

      /* A run of printable characters at once, so a paste is one redraw and
         not one per character. */
      let j = i;
      while (j < data.length) {
        const k = data.charCodeAt(j);
        if (k < 32 || k === 127 || k > 255) break;
        j++;
      }
      if (j > i) {
        this.insert(data.slice(i, j));
        i = j;
        await this.redraw();
        continue;
      }

      i += 1;                       /* something we do not act on */
    }
  }

  private match(rest: string): { len: number; run: () => Promise<void> } | null {
    for (const [key, fn] of this.table) {
      if (rest.startsWith(key)) {
        return { len: key.length, run: async () => { await fn(); } };
      }
    }

    /* An escape sequence we have no verb for -- a mouse report, a key with a
       modifier nobody bound.  Swallowed rather than let its letters land in
       the line as text.  A LINE-mode decision: char mode forwards it. */
    if (rest.charCodeAt(0) === 27) {
      const m = /^\u001B(\[[0-9;?]*[ -/]*[@-~]|O.|.)/.exec(rest);
      if (m !== null) {
        const len = m[0].length;
        return { len, run: async () => { /* dropped */ } };
      }
    }

    return null;
  }

  /*
   * The verbs, longest spelling first so ESC [ 1 ; 5 C is not matched by a
   * looser entry above it.  One table rather than a ladder of ifs: the CSI
   * forms and the control characters are the same twenty editing verbs under
   * two spellings, and a table is the only shape in which that stays obvious.
   *
   * Built once, in the constructor.  It was rebuilt on every keystroke, which
   * allocated thirty closures per character typed.
   */
  private buildTable(): [string, () => void | Promise<void>][] {
    return [
      [ESC + "[1;5C", () => this.wordRight()],
      [ESC + "[1;5D", () => this.wordLeft()],
      [ESC + "[3~",   () => this.deleteRight()],
      [ESC + "[1~",   () => this.home()],
      [ESC + "[4~",   () => this.end()],
      [ESC + "[A",    () => this.historyBack()],
      [ESC + "[B",    () => this.historyForward()],
      [ESC + "[C",    () => this.right()],
      [ESC + "[D",    () => this.left()],
      [ESC + "[H",    () => this.home()],
      [ESC + "[F",    () => this.end()],
      [ESC + "OH",    () => this.home()],
      [ESC + "OF",    () => this.end()],
      [ESC + "b",     () => this.wordLeft()],
      [ESC + "f",     () => this.wordRight()],
      [ESC + "\u007F", () => this.killWordLeft()],
      ["\u0001",      () => this.home()],          /* Ctrl-A */
      ["\u0002",      () => this.left()],          /* Ctrl-B */
      ["\u0003",      () => this.breakKey()],      /* Ctrl-C */
      ["\u0004",      () => this.eofKey()],        /* Ctrl-D */
      ["\u0005",      () => this.end()],           /* Ctrl-E */
      ["\u0006",      () => this.right()],         /* Ctrl-F */
      ["\u0008",      () => this.deleteLeft()],    /* Ctrl-H */
      ["\u000B",      () => this.killToEnd()],     /* Ctrl-K */
      ["\u000C",      () => this.clearKey()],      /* Ctrl-L */
      ["\u000E",      () => this.historyForward()],/* Ctrl-N */
      ["\u0010",      () => this.historyBack()],   /* Ctrl-P */
      ["\u0015",      () => this.killToStart()],   /* Ctrl-U */
      ["\u0017",      () => this.killWordLeft()],  /* Ctrl-W */
      ["\u007F",      () => this.deleteLeft()],    /* Backspace */
    ];
  }

  // ---------------------------------------------------------------- verbs --

  /*
   * Tab.  The word before the cursor, and whether it is the command -- the
   * first word on the line -- so the far side knows to offer C: as well as
   * the current directory.  It answers with `comp <remainder>`.
   */
  private requestComplete(): void {
    /* The word before the cursor, and the point inside it a match replaces
       from: after the last '/' or ':', so a path keeps its directory and only
       the leaf is completed. */
    let ws = this.cur;
    while (ws > 0 && this.buf[ws - 1] !== " ") ws--;
    let ps = ws;
    for (let k = ws; k < this.cur; k++) {
      const ch = this.buf[k];
      if (ch === "/" || ch === ":") ps = k + 1;
    }
    this.pendCompStart = ps;
    const token = this.buf.slice(ws, this.cur);
    /* The command position is the first word on the line. */
    const col0 = this.buf.slice(0, ws).trim().length === 0 ? "1" : "0";
    this.h.onComplete(col0 + " " + token);
  }

  /*
   * The far side's answer: the matching names, one per line.  The first
   * replaces the word; further Tabs cycle through the rest (see the Tab key).
   * Queued, so it lands after whatever was mid-edit, and only in line mode.
   */
  applyCompletion(list: string): void {
    const cands = list.split("\n").filter((x) => x.length > 0);
    this.queue(async () => {
      if (!this.enabled || cands.length === 0) return;
      this.cyc = { start: this.pendCompStart, cands, idx: 0 };
      await this.showCandidate();
    });
  }

  /* Put the current candidate in place of what was completed last, keeping any
     text that was to the right of the cursor. */
  private async showCandidate(): Promise<void> {
    if (this.cyc === null) return;
    const cand = this.cyc.cands[this.cyc.idx];
    this.buf = this.buf.slice(0, this.cyc.start) + cand + this.buf.slice(this.cur);
    this.cur = this.cyc.start + cand.length;
    this.hAt = this.history.length;
    await this.redraw();
  }

  // ------------------------------------------------------------- history --

  private loadHistory(): void {
    try {
      const raw = localStorage.getItem(HIST_KEY);
      if (raw === null) return;
      const a: unknown = JSON.parse(raw);
      if (Array.isArray(a)) {
        this.history = a.filter((x): x is string => typeof x === "string").slice(-200);
        this.hAt = this.history.length;
      }
    } catch {
      /* Private window, blocked storage, or a corrupt value: start empty. */
    }
  }

  private saveHistory(): void {
    try {
      localStorage.setItem(HIST_KEY, JSON.stringify(this.history));
    } catch {
      /* Not fatal: the history is still live for this session. */
    }
  }

  // --------------------------------------------------- reverse-i-search ---

  /* The newest history entry at or before `fromIdx` that contains `query`,
     case-insensitively, or -1. */
  private findMatch(query: string, fromIdx: number): number {
    if (query.length === 0) return -1;
    const q = query.toLowerCase();
    for (let k = fromIdx - 1; k >= 0; k--)
      if (this.history[k].toLowerCase().includes(q)) return k;
    return -1;
  }

  private async searchStart(): Promise<void> {
    this.search = { query: "", matchIdx: -1, saved: this.buf, savedCur: this.cur };
    await this.searchShow();
  }

  /* Ctrl-R again: the next match older than the one shown. */
  private async searchStep(): Promise<void> {
    if (this.search === null) return;
    const from = this.search.matchIdx >= 0 ? this.search.matchIdx : this.history.length;
    const m = this.findMatch(this.search.query, from);
    if (m >= 0) this.search.matchIdx = m;
    await this.searchShow();
  }

  /* The query changed: search again from the newest. */
  private async searchRefine(): Promise<void> {
    if (this.search === null) return;
    this.search.matchIdx = this.findMatch(this.search.query, this.history.length);
    await this.searchShow();
  }

  private async searchShow(): Promise<void> {
    if (this.search === null) return;
    const s = this.search;
    if (s.matchIdx >= 0) {
      this.buf = this.history[s.matchIdx];
      this.cur = this.buf.length;
    }
    const failing = s.query.length > 0 && s.matchIdx < 0 ? "failing " : "";
    this.h.onSearch("(" + failing + "reverse-i-search)`" + s.query + "': ");
    await this.redraw();
  }

  /* End the search.  Accept keeps the match on the line (the caller then lets
     the key that ended it -- Enter, an arrow -- act on it); cancel restores
     the line the search interrupted. */
  private async searchEnd(accept: boolean): Promise<void> {
    if (this.search === null) return;
    const s = this.search;
    this.search = null;
    this.h.onSearch(null);
    if (!accept) {
      this.buf = s.saved;
      this.cur = s.savedCur;
      await this.redraw();
    }
  }

  /* A key while searching.  Returns how many characters it consumed; 0 means
     it is not a search key, so the caller accepts the match and re-handles it.
     Ctrl-G and Ctrl-C cancel; backspace and any printable refine. */
  private async searchKey(rest: string): Promise<number> {
    if (this.search === null) return 0;
    const c = rest.charCodeAt(0);
    if (c === 0x7f || c === 0x08) {
      this.search.query = this.search.query.slice(0, -1);
      await this.searchRefine();
      return 1;
    }
    if (c === 0x07 || c === 0x03) {
      await this.searchEnd(false);
      return 1;
    }
    if (c >= 0x20 && c < 0x7f) {
      this.search.query += rest[0];
      await this.searchRefine();
      return 1;
    }
    return 0;
  }

  private insert(s: string): void {
    this.buf = this.buf.slice(0, this.cur) + s + this.buf.slice(this.cur);
    this.cur += s.length;
    this.hAt = this.history.length;
  }

  private async left(): Promise<void> {
    if (this.cur > 0) { this.cur--; await this.redraw(); }
  }

  private async right(): Promise<void> {
    if (this.cur < this.buf.length) { this.cur++; await this.redraw(); }
  }

  private async home(): Promise<void> {
    if (this.cur !== 0) { this.cur = 0; await this.redraw(); }
  }

  private async end(): Promise<void> {
    if (this.cur !== this.buf.length) {
      this.cur = this.buf.length;
      await this.redraw();
    }
  }

  /* A word is a run of non-space, and the space before it belongs to it going
     left.  Same rule as readline, and the same rule as Ctrl-W below, so the
     two never disagree about where a word started. */
  private wordStart(): number {
    let i = this.cur;
    while (i > 0 && this.buf[i - 1] === " ") i--;
    while (i > 0 && this.buf[i - 1] !== " ") i--;
    return i;
  }

  private wordEnd(): number {
    let i = this.cur;
    const n = this.buf.length;
    while (i < n && this.buf[i] === " ") i++;
    while (i < n && this.buf[i] !== " ") i++;
    return i;
  }

  private async wordLeft(): Promise<void> {
    const i = this.wordStart();
    if (i !== this.cur) { this.cur = i; await this.redraw(); }
  }

  private async wordRight(): Promise<void> {
    const i = this.wordEnd();
    if (i !== this.cur) { this.cur = i; await this.redraw(); }
  }

  private async deleteLeft(): Promise<void> {
    if (this.cur === 0) return;
    this.buf = this.buf.slice(0, this.cur - 1) + this.buf.slice(this.cur);
    this.cur--;
    await this.redraw();
  }

  private async deleteRight(): Promise<void> {
    if (this.cur >= this.buf.length) return;
    this.buf = this.buf.slice(0, this.cur) + this.buf.slice(this.cur + 1);
    await this.redraw();
  }

  private async killToEnd(): Promise<void> {
    if (this.cur >= this.buf.length) return;
    this.buf = this.buf.slice(0, this.cur);
    await this.redraw();
  }

  private async killToStart(): Promise<void> {
    if (this.cur === 0) return;
    this.buf = this.buf.slice(this.cur);
    this.cur = 0;
    await this.redraw();
  }

  private async killWordLeft(): Promise<void> {
    const i = this.wordStart();
    if (i === this.cur) return;
    this.buf = this.buf.slice(0, i) + this.buf.slice(this.cur);
    this.cur = i;
    await this.redraw();
  }

  private async historyBack(): Promise<void> {
    if (this.hAt === 0) return;
    if (this.hAt === this.history.length) this.hSaved = this.buf;
    this.hAt--;
    this.buf = this.history[this.hAt];
    this.cur = this.buf.length;
    await this.redraw();
  }

  private async historyForward(): Promise<void> {
    if (this.hAt >= this.history.length) return;
    this.hAt++;
    this.buf = this.hAt === this.history.length
      ? this.hSaved
      : this.history[this.hAt];
    this.cur = this.buf.length;
    await this.redraw();
  }

  private async breakKey(): Promise<void> {
    /* The line goes with it, the way a console handler drops it: the Shell is
       being interrupted, and finishing the half-typed command afterwards is
       never what was meant. */
    this.buf = "";
    this.cur = 0;
    await this.hide();
    await this.w("^C\r\n");
    await this.show();
    this.h.onBreak();
  }

  private async eofKey(): Promise<void> {
    if (this.buf.length > 0) { await this.deleteRight(); return; }
    this.h.onEof();
  }

  private async clearKey(): Promise<void> {
    const was = this.shown;
    if (was) await this.hide();
    await this.w(ESC + "[H" + ESC + "[2J" + ESC + "[3J");
    if (was) await this.show();
  }

  private async submit(): Promise<void> {
    const line = this.buf;

    if (this.shown) {
      /*
       * Walk the caret to the end of what was typed before breaking the line.
       * Enter is allowed in the middle of a line, and a bare CR LF from there
       * would land inside the text on a line that had wrapped.  The typed
       * line itself stays on the screen exactly as it is -- hiding and
       * redrawing it would be two more repaints for no visible difference.
       */
      this.cur = this.buf.length;
      await this.draw();
      await this.w("\r\n");
      this.shown = false;
    }

    this.buf = "";
    this.cur = 0;

    if (!this.enabled) { this.h.onDeadEnter(); return; }

    /* Duplicates and blanks are not worth a slot: three Dirs in a row should
       be one press of Up, not three. */
    if (line.trim().length > 0 &&
        this.history[this.history.length - 1] !== line) {
      this.history.push(line);
      if (this.history.length > 200) this.history.shift();
      this.saveHistory();
    }
    this.hAt = this.history.length;
    this.hSaved = "";

    this.h.onLine(line);
    await this.show();
  }
}
