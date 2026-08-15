/*
 * The part of a WebSocket that both pages need, and only that part.
 *
 * A socket that never opened is a DIFFERENT thing from one that closed, and
 * the browser reports both as 1006 with no reason on it: the HTTP status
 * behind a refused upgrade is not exposed to script at all.  The only way to
 * tell them apart is whether onopen ever ran, and both the Shell page and the
 * console page have a refusal a correct client can provoke -- something else
 * already holds the Shell, something else already holds the screen -- so both
 * would otherwise send the person looking at their network.
 *
 * It lives here rather than in either page's wire because it was written
 * once, for the terminal, and the second reader of it should not be a copy.
 * Everything ABOVE this line differs between the two: the terminal turns
 * binary frames into Latin-1 text, and the console hands the ArrayBuffer to a
 * decoder untouched, which is the one thing they must not share.
 *
 * SPDX-License-Identifier: MIT
 */

export interface SocketEnds {
  onOpen: () => void;
  /* "closed" was open once, "refused" never was. */
  onGone: (state: "closed" | "refused", detail: string) => void;
}

export function watchSocket(ws: WebSocket, ends: SocketEnds): void {
  let opened = false;

  ws.onopen = () => { opened = true; ends.onOpen(); };

  ws.onclose = (e: CloseEvent) => {
    if (opened) ends.onGone("closed", e.reason || String(e.code));
    else ends.onGone("refused", "");
  };

  /* onerror carries nothing a person can act on and is always followed by
     onclose, which does.  Left to it. */
  ws.onerror = () => { /* onclose says what happened */ };
}
