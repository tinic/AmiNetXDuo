/*
 * The frontmost screen, served live down a WebSocket.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_HTTPFB_H
#define AMINETXDUO_HTTPFB_H

#define HTTP_FB_TILE_W      16
#define HTTP_FB_TILE_H      16

/* A control frame to send back: ten bytes of header and 125 of payload, the
   same ceiling httpterm.c gives its own. */
#define HTTP_FB_CTL         136

BOOL http_fb_open(VOID);
VOID http_fb_close(VOID);

BOOL http_fb_enabled(VOID);

/*
 * Whether a viewer is holding a mouse button down on the guest right now.
 * Must be checked before logging: Intuition holds the screen's layer lock for
 * the whole of a drag, so a `-v` line written meanwhile deadlocks this task.
 */
BOOL http_fb_buttons_held(VOID);

/* Why the last thing that failed did.  Never NULL, and "" when nothing has. */
const char *http_fb_fault(VOID);

/* What the screen was when http_fb_open() looked at it, for the banner.  All
   three are 0 when there was no screen to look at; ask http_fb_screenless()
   rather than reading a zero as a size. */
VOID http_fb_geometry(UWORD *w, UWORD *h, UWORD *depth);

BOOL http_fb_screenless(VOID);

/* Nobody is holding it. */
BOOL http_fb_available(VOID);

/*
 * Take it.  `first` is whatever the client pipelined behind the request head,
 * which is already the first frames.  FALSE having filled http_fb_fault() and
 * having freed whatever it had taken.
 */
BOOL http_fb_start(struct Library *sb, LONG sock,
                   const UBYTE *first, ULONG first_len, ULONG now);

/* The geometry selected by the current session, which may differ from the
   startup banner after a screen-mode or front-screen change. */
BOOL http_fb_session_geometry(UWORD *w, UWORD *h, UWORD *depth);

/* Give it back.  Safe to call when there is no session. */
VOID http_fb_stop(VOID);

BOOL http_fb_wants_write(VOID);

/* One pass each.  Each returns FALSE when the session is finished with. */
BOOL http_fb_read(ULONG now);
BOOL http_fb_slice(ULONG now);
BOOL http_fb_write(ULONG now);
BOOL http_fb_idle(ULONG now, ULONG timeout);

/* The same rule http_fb_idle() ends a session on, with no side effect, so a
   second viewer can find out whether the one holding it still answers. */
BOOL http_fb_stale(ULONG now, ULONG timeout);

/* Take the session off its client: a close frame with a reason first, best
   effort, then the caller closes the connection. */
VOID http_fb_evict(UWORD code);

/* The close code the session ended with, for the log. */
UWORD http_fb_why(VOID);

/*
 * What the session cost, for the log when it ends.  Ticks are fiftieths and
 * are what the guest can measure: `grab` is the locked copy out of chip RAM
 * and `encode` is the delta over it, and the two together are the slice.
 */
VOID http_fb_stats(ULONG *frames, ULONG *bytes, ULONG *grab_ticks,
                   ULONG *encode_ticks);

#endif /* AMINETXDUO_HTTPFB_H */
