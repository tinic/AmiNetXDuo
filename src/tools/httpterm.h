/*
 * httpterm, an AmigaDOS Shell on the other end of a CONSOLE. One session at a
 * time. A spawned Process needs 64 KB of stack: a bsdsocket LVO call runs NetX
 * Duo on the caller's stack, so any Process that might touch a socket carries
 * the whole TCP/IP call depth.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_HTTPTERM_H
#define AMINETXDUO_HTTPTERM_H

#include "tools.h"

/* Take the rings and the port. Called once at startup, and only when a
   terminal was asked for. FALSE having said why. */
BOOL  http_term_init(VOID);

/* Give them back.  Waits for every runner still executing from httpd's load
   segment; unloading live code cannot be made into a safe bounded wait. */
VOID  http_term_shutdown(VOID);

/* Report at startup where the terminal is and that it has no password, and
   whether the address has taken over an entry of that name in the drawer. */
VOID  http_term_announce(const char *root, const char *dotted, UWORD port,
                         const char *url);

/* Say what the Shell is doing: Execute()'s answer, and the first packets of a
   session by name. */
VOID  http_term_trace(BOOL on);

/* TRUE when a session can be started: init happened and none is running. */
BOOL  http_term_available(VOID);

/* Start a Shell. It reads its commands from this side and writes everything,
   its own prompt included, back. FALSE having said why. */
BOOL  http_term_start(VOID);

/* TRUE while the Shell is running or its output is still being drained. */
BOOL  http_term_running(VOID);

/*
 * The signal WaitSelect() must include; zero when there is nothing to wait for.
 * Asked of the whole session and not of what the caller currently wants to
 * read: computing it from the current select() set deadlocks on ACTION_END.
 */
ULONG http_term_sigmask(VOID);

/* Answer whatever the Shell has sent.  Cheap when there is nothing. */
VOID  http_term_service(VOID);

/* Keystrokes to the Shell.  Returns the bytes taken, which can be short when
   the Shell is not reading.  The rest is the caller's to keep. */
LONG  http_term_write(const UBYTE *data, LONG len);

/* The Shell's output.  0 when there is none yet. */
LONG  http_term_read(UBYTE *buf, LONG len);

/* How much output is waiting. Asked rather than discovered by reading: a socket
   offered as writable with nothing to write turns the server's wait into a
   spin. */
ULONG http_term_pending(VOID);

/* The person stopped typing: the Shell reads end of file and exits. */
VOID  http_term_eof(VOID);

/* Ctrl-C, to the Shell's own process. */
VOID  http_term_break(VOID);

/* ------------------------------------------------------------ the mode --- */

/* RAW, as ACTION_SCREEN_MODE last said. SetMode(fh, 1) is how an AmigaOS
   program asks for no echo and one keystroke at a time. */
BOOL  http_term_raw(VOID);

/* The mode as a word for the page, or NULL when the page already knows. A peek;
   http_term_mode_sent() is the take. */
const char *http_term_mode_word(VOID);
VOID        http_term_mode_sent(VOID);

/* The write/frame counters, when the page has asked for them with `stats`. Same
   peek-then-take pair as the mode word. */
const char *http_term_stats_word(VOID);
VOID        http_term_stats_sent(VOID);

/* How big the page says its window is.  A zero in either is ignored.  A
   terminal component still laying out reports it, and a program told it has no
   columns divides by it. */
VOID  http_term_resize(UWORD cols, UWORD rows);

/* End the session and reclaim the runner.  Safe to call more than once. */
VOID  http_term_stop(VOID);

/* What the Shell exited with, once it has.  -1 while it is still running, and
   -1 again when no Shell started.  http_term_err() tells the two apart, and is
   0 unless no Shell started. */
LONG  http_term_rc(VOID);
LONG  http_term_err(VOID);

/* ------------------------------------------------- the socket, once it is --
 *                                                     no longer HTTP
 *
 * The frame buffer is borrowed from the caller's connection rather than owned,
 * so nothing here adds memory to a server that has no terminal.
 */

#include "httpws.h"
#include "toolsock.h"

/*
 * The wire, mirrored in src/tools/web/client/wire.ts. Binary frames are bytes
 * both ways; a text frame is one word -- `break`, `eof`, `size <c> <r>` from the
 * page, `mode raw` / `mode cooked` to it. An unrecognised word is ignored.
 */

/* HTTP_TERM_PEND is what a client has typed that the Shell has not taken yet;
   the socket is not read at all while any of it is left. One read is the most
   the decoder can turn into payload, so it cannot overflow. */
#define HTTP_TERM_READ      512
#define HTTP_TERM_PEND      HTTP_TERM_READ

/* A control frame to send back: ten bytes of header and 125 of payload. */
#define HTTP_TERM_CTL       136

typedef struct HttpTermSock
{
    struct Library *sb;             /* bsdsocket.library                    */
    LONG            sock;

    HttpWsIn        in;

    UBYTE           pend[HTTP_TERM_PEND];
    UWORD           pend_n;
    UWORD           pend_at;

    /* Bytes already read behind the HTTP upgrade.  The caller's request
       buffer is idle for the rest of this connection, so keep a cursor into
       it and decode only as fast as pend[] and the Shell can drain. */
    const UBYTE    *first;
    ULONG           first_len;
    ULONG           first_at;

    UBYTE           ctl[HTTP_TERM_CTL];
    UWORD           ctl_n;
    UWORD           ctl_at;

    char            word[24];       /* a text frame, see the vocabulary     */
    UBYTE           word_n;
    UBYTE           word_over;      /* it did not fit and must be ignored   */

    UBYTE           pinged;         /* a ping is out and unanswered         */
    UBYTE           closing;        /* a close has been sent                */
    UWORD           why;            /* the code it was closed with, for a log */

    /* Borrowed: the caller's own send buffer, and where in it we are. */
    UBYTE          *out;
    ULONG           out_size;
    ULONG           out_len;
    ULONG           out_sent;

    ULONG           progress;       /* seconds, when anything last arrived  */
} HttpTermSock;

/*
 * Begin. `first` is whatever the client pipelined behind the request head,
 * which is already the first frames. The storage remains borrowed until the
 * session ends; the caller's request buffer is otherwise idle after the upgrade.
 */
VOID http_term_sock_begin(HttpTermSock *t, struct Library *sb, LONG sock,
                          UBYTE *out, ULONG out_size,
                          const UBYTE *first, ULONG first_len, ULONG now);

/* Whether this socket can take another read. Retained bytes have to reach the
   Shell first: leaving the socket in the read set while they cannot be taken
   turns the server's wait into a spin instead of TCP back pressure. */
BOOL http_term_sock_wants_read(const HttpTermSock *t);

/* Whether this socket wants to be offered as writable.  Asked rather than
   assumed, because a socket offered with nothing to write turns the server's
   wait into a spin. */
BOOL http_term_sock_wants_write(const HttpTermSock *t);

/* One pass.  Each returns FALSE when the connection is finished with. */
BOOL http_term_sock_read(HttpTermSock *t, ULONG now);
BOOL http_term_sock_write(HttpTermSock *t, ULONG now);

/* The idle rule. A WebSocket has no request to time out, so RFC 6455 5.5.2 is
   used instead: one ping after `timeout` seconds of silence, and the connection
   goes if the next `timeout` passes with no answer. */
BOOL http_term_sock_idle(HttpTermSock *t, ULONG now, ULONG timeout);

/*
 * Whether the far end is still there, asked from outside the session, with no
 * side effect: the same rule http_term_sock_idle() ends a session on, so a
 * second client can find out whether the one holding it is still answering.
 */
BOOL http_term_sock_stale(const HttpTermSock *t, ULONG now, ULONG timeout);

/*
 * Take this session off its client. A close frame with a code and a reason goes
 * out first in one blocking-free send: best effort by design, because this
 * exists for a peer that is not reading. The caller then closes the connection.
 */
VOID http_term_sock_evict(HttpTermSock *t, UWORD code);

#endif /* AMINETXDUO_HTTPTERM_H */
