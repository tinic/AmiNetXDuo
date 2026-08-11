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
 * The mirror of it does not exist yet, and this is the note for when it does.
 *
 * Nothing is rewritten on the way OUT: a DOS pipe reads bytes and a Shell
 * reads lines, so what leaves here is exactly what was typed.  A real console
 * handler on the far side would want the other half -- an AmigaOS CON:
 * expects its cursor keys as 0x9B A, the 8-bit form, and a browser produces
 * ESC [ A -- so char-at-a-time against a console will need ESC [ turned back
 * into 0x9B before it is sent.  That is the exact inverse of the function
 * above and belongs beside it.  It is absent rather than written and unused
 * because nothing on the far side reads a cursor key today.
 */

export type WireState = "connecting" | "open" | "closed" | "failed";

export interface WireHandlers {
  onText: (s: string) => void;
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

  /* The endpoint is the page's own path, upgraded in place: whatever address
     the terminal was served from is the address the socket goes to, so there
     is nothing to configure and nothing to get wrong when the lease changes. */
  connect(): void {
    if (this.ws !== null && this.ws.readyState <= WebSocket.OPEN) return;

    const scheme = location.protocol === "https:" ? "wss://" : "ws://";
    const ws = new WebSocket(scheme + location.host + location.pathname);
    ws.binaryType = "arraybuffer";
    this.ws = ws;

    this.h.onState("connecting", "");

    ws.onopen = () => this.h.onState("open", "");

    ws.onmessage = (e: MessageEvent) => {
      const s = typeof e.data === "string"
        ? e.data
        : fromLatin1(e.data as ArrayBuffer);
      this.h.onText(csiTo7Bit(s));
    };

    ws.onclose = (e: CloseEvent) => {
      this.ws = null;
      this.h.onState("closed", e.reason || String(e.code));
    };

    ws.onerror = () => this.h.onState("failed", "");
  }

  disconnect(): void {
    const ws = this.ws;
    this.ws = null;
    if (ws !== null) {
      try { ws.close(1000, ""); } catch { /* already gone */ }
    }
  }

  /* Keystrokes.  Binary, always. */
  keys(text: string): void {
    if (this.open) this.ws!.send(toLatin1(text));
  }

  /*
   * The other channel.  Ctrl-C on an Amiga is a signal and not a byte -- a
   * console handler turns the key into SIGBREAKF_CTRL_C -- so there is no
   * in-band way to send one down a pipe.  The server reads exactly two words
   * out of a text frame, and keystrokes never go as text, so the two cannot
   * be confused.
   */
  word(w: "break" | "eof"): void {
    if (this.open) this.ws!.send(w);
  }
}
