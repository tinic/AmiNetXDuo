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
 * The refused-versus-closed distinction IS shared, out of client/socket.ts:
 * both pages have a refusal a correct client can provoke and a browser
 * reports it identically to a network fault.
 *
 * ?ws= overrides the endpoint, and it is not a nicety: the player half of
 * this page is worth opening straight off the filesystem, and a file:// page
 * has no host to upgrade.  Absent, the address is /console on whatever
 * machine served the page, so an Amiga serving it is the Amiga it connects
 * back to with nothing configured.
 *
 * SPDX-License-Identifier: MIT
 */

import { watchSocket } from "../socket";

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

/* Inflate a frame whose ops (bytes 4..) are zlib-deflated, via the browser's
   native DecompressionStream -- no bundled codec.  The 4-byte header
   (version, flags, seq) is copied through raw and the flag cleared, so the
   decoder sees an ordinary frame. */
async function inflateFrame(u: Uint8Array): Promise<ArrayBuffer> {
  const ds = new DecompressionStream("deflate");
  const w = ds.writable.getWriter();
  void w.write(u.subarray(4));
  void w.close();
  const r = ds.readable.getReader();
  const chunks: Uint8Array[] = [];
  let total = 0;
  for (;;) {
    const { done, value } = await r.read();
    if (done) break;
    chunks.push(value);
    total += value.length;
  }
  const out = new Uint8Array(4 + total);
  out.set(u.subarray(0, 4), 0);
  out[1] = 0;
  let off = 4;
  for (const c of chunks) {
    out.set(c, off);
    off += c.length;
  }
  return out.buffer;
}

export class Wire {
  private ws: WebSocket | null = null;
  private readonly h: WireHandlers;
  /* Serialises async inflation so frames reach the decoder in wire order. */
  private q: Promise<void> = Promise.resolve();

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

    watchSocket(ws, {
      onOpen: () => this.h.onState("open", url),
      onGone: (state, detail) => {
        this.ws = null;
        this.h.onState(state, detail);
      },
    });

    ws.onmessage = (e: MessageEvent) => {
      if (typeof e.data === "string") {
        this.h.onWord(e.data);
        return;
      }
      /* A binary frame may carry deflate-compressed ops (header flags bit 0).
         Inflation is async (DecompressionStream), so every binary frame goes
         through one promise chain to stay in wire order: a delta applied out
         of order corrupts every frame after it. */
      const buf = e.data as ArrayBuffer;
      const u = new Uint8Array(buf);
      if (u.length >= 4 && (u[1] & 0x01) !== 0) {
        this.q = this.q
          .then(() => inflateFrame(u))
          .then((b) => this.h.onFrame(b))
          .catch(() => { /* drop it; the next frame's seq gap forces a refresh */ });
      } else {
        this.q = this.q.then(() => { this.h.onFrame(buf); });
      }
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
    if (this.open) this.ws!.send(w);
  }
}
