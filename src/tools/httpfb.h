/*
 * The frontmost screen, served live down a WebSocket.
 *
 * httpd's second endpoint that is not a file.  /console serves an HTML page and
 * a WebSocket upgrade to the same address gets the frontmost screen: one text
 * frame saying what shape it is, one saying what colours it has, and then a
 * binary frame per grab carrying only what changed.  The wire format is the
 * frame encoder's and is written down in include/aminetxduo/rfb_encode.h.  The
 * control words are in include/aminetxduo/rfb_words.h.
 *
 * The delta stream is a conversation with one receiver: the encoder keeps a
 * shadow of what that receiver last drew and every frame is expressed against
 * it.  Two viewers would need two shadows and two encoders, which at depth 4 is
 * a second 160 KB for the second person to watch the same screen.  So one at a
 * time, and the same stale-reclaim rule the terminal has, for the same reason: a
 * tab that goes away when the network does sends neither a close frame nor a
 * FIN, and without reclaim the endpoint stays held until the machine is
 * restarted.
 *
 * The screen served is IntuitionBase->FirstScreen, which is what is in front,
 * and the picture is its whole bitmap at its own origin.  That is how RTG
 * works: a screen owns the display and screen dragging does not exist, so a
 * screen dragged down is shown whole rather than composited over what is
 * behind it, and a screen whose bitmap is bigger than the display, an
 * autoscroll or overscan screen, goes out entire rather than scrolled under a
 * pointer the far end does not have.  The geometry advertised is always the
 * bitmap's, which is what the frames carry.
 *
 * It used to be LockPubScreen("Workbench") and nothing else, which locked a
 * remote viewer out of the Palette and Overscan editors: each opens a screen
 * of its own that is not public, so the browser went on showing an unchanged
 * Workbench with no way to see or dismiss what had opened in front.
 *
 * Three kinds of screen, and the difference between them stops at this file's
 * edge.  A chipset screen is depth one-bit planes, read where they lie.  A
 * graphics card's is one plane of bytes, which cannot be read where it lies
 * at all -- httprtg.c fetches it into a staging buffer in Fast RAM and the
 * encoder is pointed at that -- and is either a byte a pixel with a palette
 * or two bytes a pixel with none, the second being what a 15, 16, 24 or
 * 32-bit screen is converted to on the way.  Which one it is travels as
 * rfb_geom.format, and past the geometry nothing here asks: the encoder walks
 * bytes and the tile grid is over bytes whichever it is.
 *
 * What is still refused is a bitmap nothing can read: no BMF_STANDARD and
 * neither Picasso96 nor CyberGraphX claiming it, or a card format there is no
 * conversion for.  Those get a sentence, and since the browser is the only
 * place it can usefully arrive, httpd.c sends it in the WebSocket close
 * frame as well as writing it to the log.
 *
 * The locks are not held across the socket.  docs/FORBID.md: nothing inside
 * Forbid() must not block, LockLayers stops every other task drawing, and
 * LockIBase() stops Intuition.  So a grab takes what it needs, the geometry,
 * the palette and the bitplane addresses, gives the lock back, and the encode
 * and the send both happen with nothing held.
 *
 * Without -C none of this runs: no library opened, no buffer taken.  With -C
 * the three libraries are opened at startup so a machine that cannot serve a
 * screen says so before it is serving anything.  The buffers, which are the
 * part with size to them, are taken when a viewer connects and given back when
 * it goes.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_HTTPFB_H
#define AMINETXDUO_HTTPFB_H

/*
 * The tile grid, in bytes across and rows down.  16x16 is the encoder's
 * measured shipping shape and is not a knob here.  A receiver is told the
 * numbers in the `geom` word and works from those, so changing them would only
 * make two builds of this server disagree.
 */
#define HTTP_FB_TILE_W      16
#define HTTP_FB_TILE_H      16

/* A control frame to send back: ten bytes of header and 125 of payload, the
   same ceiling httpterm.c gives its own. */
#define HTTP_FB_CTL         136

/* ------------------------------------------------------------ the module --- */

/* -C.  Opens graphics, intuition and layers at V39 and looks at the front
   screen once, so that a machine which cannot serve one refuses to start
   rather than answering 503 to the first person who asks.  FALSE having
   filled http_fb_fault(). */
BOOL http_fb_open(VOID);
VOID http_fb_close(VOID);

/* Whether -C was given and the libraries are open. */
BOOL http_fb_enabled(VOID);

/*
 * Whether a viewer is holding a mouse button down on the guest right now.
 *
 * Asked by the log, from httpd.c's httpd_may_log().  Intuition holds the
 * screen's layer lock for the whole of a menu or a drag, the console handler
 * needs that lock to draw, and this server has one task.  So a `-v` line
 * written to a Shell window while a button is down waits inside DOS.  A person
 * holding a menu open lets go.  A button held by the remote console cannot come
 * up, because the only thing that would release it is this task reading the
 * release word off the socket it is not reading.
 */
BOOL http_fb_buttons_held(VOID);

/* Why the last thing that failed did.  Never NULL, and "" when nothing has. */
const char *http_fb_fault(VOID);

/* What the screen was when http_fb_open() looked at it, for the banner. */
VOID http_fb_geometry(UWORD *w, UWORD *h, UWORD *depth);

/* ----------------------------------------------------------- the session --- */

/* Nobody is holding it. */
BOOL http_fb_available(VOID);

/*
 * Take it.  `first` is whatever the client pipelined behind the request head,
 * which is already the first frames.  FALSE having filled http_fb_fault() and
 * having freed whatever it had taken.
 */
BOOL http_fb_start(struct Library *sb, LONG sock,
                   const UBYTE *first, ULONG first_len, ULONG now);

/* Give it back.  Safe to call when there is no session. */
VOID http_fb_stop(VOID);

/* Whether the socket wants to be offered as writable.  Asked rather than
   assumed: offering one with nothing to write turns the server's wait into a
   spin. */
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
