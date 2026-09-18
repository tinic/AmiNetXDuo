/*
 * Find in the scrollback.
 *
 * xterm draws to a canvas, so the browser's own Find cannot reach a word of
 * the output.  This is that: a box in the bar, opened with the button or
 * Cmd-F / Ctrl-Shift-F, that searches the buffer and highlights the match.
 * Enter steps to the next older match, Shift-Enter to the newer, Esc closes.
 *
 * Row-by-row: each visual row of the buffer is searched on its own, so a match
 * that straddles a wrapped line is not found -- rare on a Shell's short lines,
 * and not worth a second buffer to join them.
 *
 * SPDX-License-Identifier: MIT
 */

import type { Terminal } from "@xterm/xterm";

export class Find {
  private readonly term: Terminal;
  private readonly box: HTMLElement;
  private readonly input: HTMLInputElement;
  private readonly info: HTMLElement;

  private row = -1;      /* absolute buffer row of the current match, or -1 */

  constructor(term: Terminal, box: HTMLElement, input: HTMLInputElement,
              info: HTMLElement) {
    this.term = term;
    this.box = box;
    this.input = input;
    this.info = info;

    this.input.addEventListener("input", () => { this.row = -1; this.step(-1); });
    this.input.addEventListener("keydown", (e) => {
      if (e.key === "Enter") { e.preventDefault(); this.step(e.shiftKey ? 1 : -1); }
      else if (e.key === "Escape") { e.preventDefault(); this.close(); }
    });
  }

  get isOpen(): boolean { return !this.box.hidden; }

  open(): void {
    this.box.hidden = false;
    this.input.focus();
    this.input.select();
    if (this.input.value.length > 0) { this.row = -1; this.step(-1); }
  }

  close(): void {
    this.box.hidden = true;
    this.info.textContent = "";
    this.clear();
    this.term.focus();
  }

  private clear(): void {
    try { this.term.clearSelection(); } catch { /* nothing selected */ }
  }

  /* dir: -1 older (up), +1 newer (down).  Starts one past the current match,
     or at the bottom for a fresh query, and wraps once around the buffer. */
  private step(dir: 1 | -1): void {
    const q = this.input.value.toLowerCase();
    if (q.length === 0) { this.clear(); this.info.textContent = ""; this.row = -1; return; }

    const buf = this.term.buffer.active;
    const n = buf.length;
    if (n === 0) return;

    const start = this.row >= 0 ? this.row : (dir < 0 ? n : -1);

    for (let s = 1; s <= n; s++) {
      const r = (((start + dir * s) % n) + n) % n;
      const line = buf.getLine(r);
      if (line === undefined) continue;
      const idx = line.translateToString(true).toLowerCase().indexOf(q);
      if (idx >= 0) {
        this.row = r;
        this.clear();
        try { this.term.select(idx, r, q.length); } catch { /* off-buffer */ }
        this.term.scrollToLine(Math.max(0, r - 2));
        this.info.textContent = "";
        return;
      }
    }

    this.info.textContent = "no match";
    this.row = -1;
    this.clear();
  }
}
