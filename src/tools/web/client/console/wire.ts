/*
 * The socket.  Same convention as the Shell's, opposite payload.
 *
 * Binary frames are the data stream and text frames are control words, which
 * is what client/wire.ts already does for the terminal.  What is NOT shared
 * with it is the translation: that one turns every binary frame into a
 * Latin-1 string because the thing on the other end is a Shell, and doing
 * that to a bitplane would be a copy, a re-encode and a second copy of a
 * quarter-megabyte per frame.  Here the ArrayBuffer is handed over as it
 * arrived and the decoder reads it in place.
 *
 * The refused-versus-closed distinction below is client/wire.ts's, COPIED.
 * It was briefly factored out into a module both pages imported, and that is
 * the right end state -- but it regenerates shell.html, which is a committed,
 * shipped artifact, and a prototype does not get to perturb one.  Eighteen
 * lines of duplication is the cheaper side of that trade until this is a
 * feature rather than an experiment; the extraction is then its own commit
 * with the artifact regeneration as its visible point.
 *
 * ?ws= overrides the endpoint, and it is not a nicety: the player half of
 * this page is worth opening straight off the filesystem, and a file:// page
 * has no host to upgrade.  Absent, the address is /console on whatever
 * machine served the page, so an Amiga serving it is the Amiga it connects
 * back to with nothing configured.
 *
 * SPDX-License-Identifier: MIT
 */

export type WireState = "idle" | "connecting" | "open" | "closed" | "refused";

/* The console's own address, beside the terminal's /shell.  It is a separate
   app and not a mode of that one. */
export const CONSOLE_URL = "/console";

export interface WireHandlers {
  onFrame: (data: ArrayBuffer) => void;
  onWord: (w: string) => void;
  onState: (state: WireState, detail: string) => void;
}

export function defaultEndpoint(): string {
  const q = new URLSearchParams(location.search).get("ws");
  if (q !== null && q !== "") return q;
  if (location.protocol === "file:") return "ws://127.0.0.1:8098" + CONSOLE_URL;
  const scheme = location.protocol === "https:" ? "wss://" : "ws://";
  return scheme + location.host + CONSOLE_URL;
}

export class Wire {
  private ws: WebSocket | null = null;
  private readonly h: WireHandlers;

  /* Counted here because this is the only place that sees every frame, and
     the bar wants a rate rather than the viewer keeping a second tally that
     can drift from it. */
  bytesIn = 0;
  framesIn = 0;
  wordsIn = 0;
  wordsOut = 0;

  constructor(h: WireHandlers) {
    this.h = h;
  }

  get open(): boolean {
    return this.ws !== null && this.ws.readyState === WebSocket.OPEN;
  }

  connect(url: string): void {
    if (this.ws !== null && this.ws.readyState <= WebSocket.OPEN) return;

    let ws: WebSocket;
    try {
      ws = new WebSocket(url);
    } catch (e) {
      /* A malformed address throws synchronously and never produces a close
         event, so without this the page sits on "connecting" for ever. */
      this.h.onState("refused", String(e instanceof Error ? e.message : e));
      return;
    }

    ws.binaryType = "arraybuffer";
    this.ws = ws;
    this.h.onState("connecting", url);

    /*
     * A socket that never opened is a DIFFERENT thing from one that closed,
     * and the browser reports both as 1006 with no reason on it: the HTTP
     * status behind a refused upgrade is not exposed to script at all.  So
     * the two are told apart by whether onopen ever ran.
     *
     * Worth the four lines because the refusal a working client can provoke
     * is "somebody else has the screen", and "closed (1006)" sends that
     * person looking at the network.
     */
    let opened = false;

    ws.onopen = () => { opened = true; this.h.onState("open", url); };

    ws.onclose = (e: CloseEvent) => {
      this.ws = null;
      if (opened) this.h.onState("closed", e.reason || String(e.code));
      else this.h.onState("refused", "");
    };

    /* onerror carries nothing a person can act on and is always followed by
       onclose, which does.  Left to it. */
    ws.onerror = () => { /* onclose says what happened */ };

    ws.onmessage = (e: MessageEvent) => {
      if (typeof e.data === "string") {
        this.wordsIn++;
        this.h.onWord(e.data);
        return;
      }
      const buf = e.data as ArrayBuffer;
      this.framesIn++;
      this.bytesIn += buf.byteLength;
      this.h.onFrame(buf);
    };
  }

  disconnect(): void {
    const ws = this.ws;
    this.ws = null;
    if (ws !== null) {
      try { ws.close(1000, ""); } catch { /* already gone */ }
    }
    this.h.onState("idle", "");
  }

  /*
   * Every outbound message is a word.  There is no inbound data stream on
   * this socket -- a viewer sends input and asks for redraws, and both are
   * control -- so the binary direction is unused and stays that way rather
   * than being filled with a second encoding of the same thing.
   */
  word(w: string): void {
    if (this.open) {
      this.wordsOut++;
      this.ws!.send(w);
    }
  }
}
