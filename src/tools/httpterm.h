/*
 * httpterm, an AmigaDOS Shell on the other end of a pipe.
 *
 * WHAT IT IS
 *
 *   ANYONE WHO CAN REACH THE PORT GETS A SHELL.  There is no credential, no
 *   challenge and nothing to configure: the endpoint exists only when httpd
 *   was given TERMINAL, and where it exists it is open, exactly as the WebDAV
 *   write methods on the same port are.  That is the deliberate shape of this
 *   server and not an omission -- a LAN it is reachable on is the boundary.
 *
 * WHY A PIPE AND A SECOND PROCESS
 *
 *   AmigaOS has no pty and no fork().  A command's input and output are DOS
 *   file handles, and a file handle is a MsgPort somebody answers packets on:
 *   Read() sends ACTION_READ to fh_Type and sleeps until a reply comes back.
 *   Nothing says that port has to belong to a filesystem, so this answers them
 *   itself out of a ring buffer, which is what makes a live conversation
 *   possible without a PIPE: handler on the boot volume.
 *   src/bsdsocket/tcp_handler.c does the same for TCP:.
 *
 *   And SystemTagList() does not return until the Shell has finished, while
 *   the Shell cannot finish until somebody answers its Read() -- which is this
 *   process.  So the synchronous call goes to a Process of its own whose only
 *   job is to hold it, and httpd stays in its event loop.
 *
 *   Both halves are recovered from the archived anx-ssh-server branch
 *   (refs/deleted/attic/anx-ssh-server, tip 6300804), where they carried scp's
 *   request/response ping-pong.  Two things it learned the hard way are kept
 *   verbatim: a live pipe's port stays in the wait mask whether or not the
 *   caller currently wants to read it, and 64 KB is the floor for a spawned
 *   Process.
 *
 * SIXTY-FOUR KILOBYTES, AND WHY IT IS NOT A GUESS
 *
 *   A bsdsocket LVO call runs NetX Duo on the CALLER's stack --
 *   ami_netstack_enter() takes the baton and descends from there -- so any
 *   Process that might touch a socket carries the whole TCP/IP call depth.
 *   The archived branch shipped 16 KB for the runner and 8 KB for its console
 *   reader and had to raise both; 8 KB is where docs/RESEARCH.md 16.9's F-line
 *   trap and reboot loop came from, and a reboot loop truncates the transcript,
 *   so it reads as a timeout with no breadcrumbs.  Neither Process here calls
 *   a socket vector today, which is the wrong thing to size for: the Shell on
 *   the far end runs whatever the person types, and `ping` is a command.
 *
 * ONE SESSION
 *
 *   One terminal at a time, and a second upgrade is refused.  Everything here
 *   is a module-level object taken once: two rings, one message port and one
 *   runner record.  Two sessions would need two of each and a table to find
 *   them by, and the thing that would be bounded by it is the number of
 *   Shells a 68020 can usefully run at once, which is one.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_HTTPTERM_H
#define AMINETXDUO_HTTPTERM_H

#include "tools.h"

/*
 * Take the rings and the port.  Called once, at startup, and only when the
 * user asked for a terminal: it is about 5 KB, and httpd's whole design is
 * that a feature nobody enabled costs nothing.  FALSE having said why.
 */
BOOL  http_term_init(VOID);

/* Give them back.  Waits, bounded, for a Shell that is still running. */
VOID  http_term_shutdown(VOID);

/*
 * Say what has just been turned on, at startup, in the two sentences a person
 * needs: where the terminal is, and that it has no password.  Also whether the
 * address has taken something over -- it is decided before a path is resolved,
 * so an entry of that name in the served drawer becomes unreachable, and a
 * file that stops answering without a word gets diagnosed as a network fault.
 */
VOID  http_term_announce(const char *root, const char *dotted, UWORD port,
                         const char *url);

/* TRUE when a session may be started: init happened and none is in flight. */
BOOL  http_term_available(VOID);

/*
 * Start a Shell.  It reads its commands from this side and writes everything,
 * its own prompt included, back.  FALSE having said why.
 */
BOOL  http_term_start(VOID);

/* TRUE while the Shell is running or its output is still being drained. */
BOOL  http_term_running(VOID);

/*
 * The signal WaitSelect() must include, so a packet from the Shell wakes the
 * server.  Zero when there is nothing to wait for.
 *
 * Asked of the whole session rather than of what the caller currently wants to
 * read: the archived branch computed it from the descriptors in the current
 * select() call and deadlocked, because the far side was by then inside
 * Close() on the other pipe waiting for an ACTION_END only this process can
 * answer, and with the port out of the mask nothing woke to answer it.
 */
ULONG http_term_sigmask(VOID);

/* Answer whatever the Shell has sent.  Cheap when there is nothing. */
VOID  http_term_service(VOID);

/* Keystrokes to the Shell.  Bytes taken, which may be short when the Shell is
   not reading; the rest is the caller's to keep. */
LONG  http_term_write(const UBYTE *data, LONG len);

/* The Shell's output.  0 when there is none yet. */
LONG  http_term_read(UBYTE *buf, LONG len);

/*
 * How much output is waiting.  Asked rather than discovered by reading,
 * because the caller has to decide whether to put its socket in the writable
 * set BEFORE it has anywhere to put the bytes -- and a socket offered as
 * writable with nothing to write turns the server's wait into a spin.
 */
ULONG http_term_pending(VOID);

/* The person stopped typing: the Shell reads end of file and exits. */
VOID  http_term_eof(VOID);

/* Ctrl-C, to the Shell's own process. */
VOID  http_term_break(VOID);

/* End the session and reclaim the runner.  Safe to call more than once. */
VOID  http_term_stop(VOID);

/* What the Shell exited with, once it has.  -1 while it is still running. */
LONG  http_term_rc(VOID);

/* ------------------------------------------------- the socket, once it is --
 *                                                     no longer HTTP
 *
 * Everything above is the Shell.  This is the other half of the same subject:
 * a socket that has been upgraded, and what one pass of the server's event
 * loop does with it.
 *
 * IT LIVES HERE AND NOT IN httpd.c ON PURPOSE
 *
 *   httpd.c is 6300 lines and routes twelve methods, and more than one thing
 *   is being added to it at once.  A feature whose logic sits in that file is
 *   a feature that collides with the next one; a feature whose logic sits in
 *   its own translation unit is a routing entry.  So httpd.c keeps what is
 *   genuinely HTTP -- the headers it reads, the refusals it writes, and the
 *   101 -- and everything after the 101 is here, where nothing else is being
 *   edited.
 *
 *   The cost of that is this header: the session has to be told the socket,
 *   the library to call it through, and a buffer to build frames in.  The
 *   buffer is BORROWED from the caller's connection rather than owned, so
 *   nothing here adds memory to a server that has no terminal.
 */

#include "httpws.h"

/*
 * What a client may have typed that the Shell has not taken yet.  The socket
 * is not read at all while any of it is left, which is the back pressure a
 * frame decoder cannot express on its own: its sink is handed bytes and has
 * nowhere to refuse them to.
 *
 * The read size below is the same number, and one read is the most the decoder
 * can turn into payload, so this cannot overflow however the frames are
 * shaped.
 */
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

    UBYTE           ctl[HTTP_TERM_CTL];
    UWORD           ctl_n;
    UWORD           ctl_at;

    char            word[16];       /* a text frame: "break" or "eof"       */
    UBYTE           word_n;

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
 * Begin.  `first` is whatever the client pipelined behind the request head,
 * which is already the first frames: a browser that opens the socket and
 * types in the same segment is ordinary, and dropping that would drop the
 * first thing said.
 */
VOID http_term_sock_begin(HttpTermSock *t, struct Library *sb, LONG sock,
                          UBYTE *out, ULONG out_size,
                          const UBYTE *first, ULONG first_len, ULONG now);

/* Whether this socket wants to be offered as writable.  Asked rather than
   assumed: a socket offered with nothing to write turns the server's wait
   into a spin. */
BOOL http_term_sock_wants_write(const HttpTermSock *t);

/* One pass.  Each returns FALSE when the connection is finished with. */
BOOL http_term_sock_read(HttpTermSock *t, ULONG now);
BOOL http_term_sock_write(HttpTermSock *t, ULONG now);

/*
 * The idle rule.  A WebSocket has no request to be timed out, so the
 * no-progress rule that governs every other connection would end a session
 * because nobody typed.  The question it should be asking is whether the far
 * end is still there, and a ping is how RFC 6455 5.5.2 asks it: one after
 * `timeout` seconds of silence, and the connection goes if the next `timeout`
 * passes with no answer.
 */
BOOL http_term_sock_idle(HttpTermSock *t, ULONG now, ULONG timeout);

#endif /* AMINETXDUO_HTTPTERM_H */
