/*
 * The socket, and the two translations either side of it.
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Latin-1 both ways, and not by accident.
 *
 * AmigaDOS output is Latin-1.  Handing it to a UTF-8 decoder turns every
 * accented character in a filename into a replacement glyph, and there is no
 * way to tell afterwards which byte it was.  A byte is a character here.
 *
 * The other direction drops anything above 255 rather than encoding it as a
 * pair: an AmigaDOS Shell has no code for it, and two bytes on the wire is
 * two characters it would act on.
 */
export function fromLatin1(buf: ArrayBuffer): string {
  const b = new Uint8Array(buf);
  let s = "";

  /* Chunked because String.fromCharCode.apply blows the argument limit on a
     big paste of output, and a per-byte += is measurably slower on the sizes
     Type of a real file produces. */
  for (let i = 0; i < b.length; i += 4096) {
    s += String.fromCharCode.apply(null, Array.from(b.subarray(i, i + 4096)));
  }

  return s;
}

export function toLatin1(text: string): Uint8Array {
  const b: number[] = [];

  for (let i = 0; i < text.length; i++) {
    const c = text.charCodeAt(i);
    if (c < 256) b.push(c);
  }

  return new Uint8Array(b);
}

/*
 * AmigaOS writes CSI as ONE byte, 0x9B, where the rest of the world writes
 * ESC [.  It is the 8-bit C1 control the standard allows and almost nothing
 * else still emits; xterm.js parses only the 7-bit form, so the byte is
 * rewritten into it here, before the parser sees it.
 *
 * MEASURED, SO THAT THE CLAIM IS THE RIGHT SIZE
 *
 *   A Shell on a PIPE does not send it.  871 bytes of Dir, List, Version and
 *   Type off a WB3.1 A1200 contained no 0x9B at all, and `Echo "*e[33m"`
 *   sends ESC [ because *e is ESC and the [ was typed.  What the old page
 *   actually got wrong was those ESC [ sequences -- a <pre> renders the ESC
 *   as nothing and leaves `[33mYELLOW[0m` on the screen as text.
 *
 *   0x9B is what the console.device writes, and a console handler on the far
 *   side is the next thing being built.  This is here so that arrives working
 *   rather than as a bug report, and it costs one indexOf on a string that
 *   almost never contains it.
 *
 * Safe to do a frame at a time: 0x9B is a single byte, so unlike a multi-byte
 * sequence it can never be split across two arrivals.  And nothing is lost --
 * 0x80..0x9F are controls in Latin-1 and no printable character lives there.
 */
const CSI8 = /\u009B/g;

export function csiTo7Bit(s: string): string {
  return s.indexOf("\u009B") < 0 ? s : s.replace(CSI8, "\u001B[");
}

/*
 * And the mirror of it, which now has a reader.
 *
 * An AmigaOS program in RAW mode expects its cursor keys as 0x9B 'A', the
 * 8-bit form: Ed reads a keystroke and compares it against 0x9B before it
 * looks at anything else, and the standard keymap is what produces that byte.
 * A browser produces ESC '['.  So in RAW mode -- and ONLY in raw mode -- the
 * two-byte introducer is folded back into the one the far side is written
 * against.
 *
 * ONLY IN RAW MODE, because in cooked mode nothing on the far side reads an
 * escape sequence at all and a lone 0x9B typed into a Shell command line is a
 * character it would have to quote.
 *
 * A bare ESC with nothing after it is left alone: it is Escape, the key, and
 * it is not an introducer.  This runs over what one keystroke produced, so
 * the '[' is either in the same string or was never coming.
 */
const CSI7 = /\u001B\[/g;

export function csiTo8Bit(s: string): string {
  return s.indexOf("\u001B[") < 0 ? s : s.replace(CSI7, "\u009B");
}

import { watchSocket } from "./socket";

export type WireState =
  | "connecting"
  | "open"
  | "closed"     /* it was open and then it was not */
  | "refused";   /* it never opened at all */

export interface WireHandlers {
  onText: (s: string) => void;
  /* A word from the server: see the vocabulary in src/tools/httpterm.h. */
  onWord: (w: string) => void;
  onState: (state: WireState, detail: string) => void;
}

export class Wire {
  private ws: WebSocket | null = null;
  private readonly h: WireHandlers;

  constructor(h: WireHandlers) {
    this.h = h;
  }

  get open(): boolean {
    return this.ws !== null && this.ws.readyState === WebSocket.OPEN;
  }

  /*
   * The endpoint is the page's own path, upgraded in place: whatever address
   * the terminal was served from is the address the socket goes to, so there
   * is nothing to configure and nothing to get wrong when the lease changes.
   *
   * `take` appends ?take=1, which is the server's word for "give me the
   * terminal even though somebody has it".  A query parameter rather than a
   * header because a browser cannot put a header on a WebSocket handshake,
   * and rather than a subprotocol because a subprotocol has to be echoed in
   * the 101 and this is a request, not a dialect.
   */
  connect(take = false): void {
    if (this.ws !== null && this.ws.readyState <= WebSocket.OPEN) return;

    const scheme = location.protocol === "https:" ? "wss://" : "ws://";
    const ws = new WebSocket(scheme + location.host + location.pathname +
                             (take ? "?take=1" : ""));
    ws.binaryType = "arraybuffer";
    this.ws = ws;

    this.h.onState("connecting", "");

    /*
     * Open, closed and refused are client/socket.ts's -- the console page
     * has the same three states and the same reason for needing the last two
     * told apart, and it reads the same rule from the same place.
     */
    watchSocket(ws, {
      onOpen: () => this.h.onState("open", ""),
      onGone: (state, detail) => {
        this.ws = null;
        this.h.onState(state, detail);
      },
    });

    /*
     * WHICH FRAME IS WHICH, AND WHY THE OPCODE IS ENOUGH
     *
     *   Binary is the Shell's output and always was.  Text is the word channel
     *   -- and the server sent NO text frames at all before this existed, so
     *   there is no ambiguity to resolve and no sentinel to prefix: a text
     *   frame from the server is a word, by construction.  The reverse
     *   direction has always been the same shape (`break`, `eof`), so both
     *   ends now read the same rule.
     */
    ws.onmessage = (e: MessageEvent) => {
      if (typeof e.data === "string") {
        this.h.onWord(e.data);
        return;
      }
      this.h.onText(csiTo7Bit(fromLatin1(e.data as ArrayBuffer)));
    };

  }

  disconnect(): void {
    const ws = this.ws;
    this.ws = null;
    if (ws !== null) {
      try { ws.close(1000, ""); } catch { /* already gone */ }
    }
  }

  /*
   * Keystrokes.  Binary, always.
   *
   * `raw` says which introducer the far side is expecting -- see csiTo8Bit()
   * above.  It is passed in rather than kept here because the mode belongs to
   * the session and this class owns the socket, not the session.
   */
  keys(text: string, raw = false): void {
    if (this.open) this.ws!.send(toLatin1(raw ? csiTo8Bit(text) : text));
  }

  /*
   * The other channel.  Ctrl-C on an Amiga is a signal and not a byte -- a
   * console handler turns the key into SIGBREAKF_CTRL_C -- so there is no
   * in-band way to send one.  Keystrokes never go as text, so the two cannot
   * be confused.  The vocabulary, both directions, is in src/tools/httpterm.h.
   */
  word(w: string): void {
    if (this.open) this.ws!.send(w);
  }

  /* How big this window is.  A word rather than a header because it changes
     while the session is up, and the far side has a program in it that may
     ask at any moment. */
  size(cols: number, rows: number): void {
    if (cols > 0 && rows > 0) this.word("size " + cols + " " + rows);
  }
}
