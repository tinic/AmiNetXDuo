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
 * The refused-versus-closed distinction is copied deliberately, and the
 * reasoning is client/wire.ts:166.  A browser reports both as 1006 with no
 * reason on it, and the only refusal a correct client can provoke here --
 * something else already has the screen -- reads as a network fault unless
 * the two are told apart by whether onopen ever ran.
 *
 * ?ws= overrides the endpoint, and it is not a nicety: the player half of
 * this page is worth opening straight off the filesystem, and a file:// page
 * has no host to upgrade.  Absent, the address is the page's own, so a page
 * served by an Amiga connects back to that Amiga with nothing configured.
 *
 * SPDX-License-Identifier: MIT
 */

export type WireState = "idle" | "connecting" | "open" | "closed" | "refused";

export interface WireHandlers {
  onFrame: (data: ArrayBuffer) => void;
  onWord: (w: string) => void;
  onState: (state: WireState, detail: string) => void;
}

export function defaultEndpoint(): string {
  const q = new URLSearchParams(location.search).get("ws");
  if (q !== null && q !== "") return q;
  if (location.protocol === "file:") return "ws://127.0.0.1:8098/screen";
  const scheme = location.protocol === "https:" ? "wss://" : "ws://";
  return scheme + location.host + location.pathname;
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

    let opened = false;

    ws.onopen = () => { opened = true; this.h.onState("open", url); };

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

    ws.onclose = (e: CloseEvent) => {
      this.ws = null;
      if (opened) this.h.onState("closed", e.reason || String(e.code));
      else this.h.onState("refused", "");
    };

    ws.onerror = () => { /* onclose says what happened */ };
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
