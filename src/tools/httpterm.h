/*
 * httpterm, an AmigaDOS Shell on the other end of a CONSOLE.
 *
 * This was a pipe: a MsgPort answering ACTION_READ and ACTION_WRITE out of two
 * rings.  A pipe carries bytes and nothing else, and an AmigaOS program that
 * wants anything else asks for it with a packet a pipe cannot answer.  `ssh`
 * called SetMode(Input(), 1) to stop the echo for a password prompt, the pipe
 * answered ERROR_ACTION_NOT_KNOWN, and the password was drawn on the screen as
 * it was typed.  Ed and More could not run at all.
 *
 * So the port answers the packets a console answers.  ACTION_SCREEN_MODE is
 * the one that matters and the rest follow from it: ACTION_WAIT_CHAR with a
 * timeout that is honoured, ACTION_CHANGE_SIGNAL for where a Ctrl-C goes,
 * ACTION_DISK_INFO for what kind of stream this is, and the window bounds
 * report, which is not a packet at all but a sequence in the write stream,
 * answered into the read stream.  See src/tools/httpterm.c for each.
 *
 * Anyone who can reach the port gets a Shell.  There is no credential, no
 * challenge and nothing to configure.  The endpoint exists only when httpd was
 * given TERMINAL, and where it exists it is open, exactly as the WebDAV write
 * methods on the same port are.  That is the deliberate shape of this server
 * and not an omission.  The LAN it is reachable on is the boundary.
 *
 * AmigaOS has no pty and no fork().  A command's input and output are DOS file
 * handles, and a file handle is a MsgPort somebody answers packets on.  Read()
 * sends ACTION_READ to fh_Type and sleeps until a reply comes back.  Nothing
 * says that port has to belong to a filesystem, so this answers them itself
 * out of a ring buffer, which makes a live conversation possible without a
 * PIPE: handler on the boot volume.  src/bsdsocket/tcp_handler.c does the same
 * for TCP:.
 *
 * SystemTagList() does not return until the Shell has finished, and the Shell
 * cannot finish until this process answers its Read().  So the synchronous
 * call goes to a Process of its own whose only job is to hold it, and httpd
 * stays in its event loop.
 *
 * Both halves are recovered from the archived anx-ssh-server branch
 * (refs/deleted/attic/anx-ssh-server, tip 6300804), where they carried scp's
 * request and response exchange.  Two things learned there are kept verbatim:
 * a live pipe's port stays in the wait mask whether or not the caller
 * currently wants to read it, and 64 KB is the floor for a spawned Process.
 *
 * The 64 KB is measured, not guessed.  A bsdsocket LVO call runs NetX Duo on
 * the caller's stack, since ami_netstack_enter() descends from there, so any
 * Process that might touch a socket carries the whole TCP/IP call depth.  The
 * archived branch shipped 16 KB for the runner and 8 KB for its console reader
 * and had to raise both.  8 KB is where docs/RESEARCH.md 16.9's F-line trap
 * and reboot loop came from, and a reboot loop truncates the transcript, so it
 * reads as a timeout with no evidence.  Neither Process here calls a socket
 * vector today, which is the wrong thing to size for, because the Shell on the
 * far end runs whatever the person types and `ping` is a command.
 *
 * One terminal at a time, and a second upgrade is refused.  Everything here is
 * a module-level object taken once: two rings, one message port and one runner
 * record.  Two sessions would need two of each and a table to find them by,
 * and the number of Shells a 68020 can usefully run at once is one.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_HTTPTERM_H
#define AMINETXDUO_HTTPTERM_H

#include "tools.h"

/*
 * Take the rings and the port.  Called once at startup, and only when a
 * terminal was asked for.  It is about 5 KB, and in httpd a feature nobody
 * enabled costs nothing.  FALSE having said why.
 */
BOOL  http_term_init(VOID);

/* Give them back.  Waits, bounded, for a Shell that is still running. */
VOID  http_term_shutdown(VOID);

/*
 * Report at startup where the terminal is and that it has no password.  Also
 * whether the address has taken something over.  The address is decided before
 * a path is resolved, so an entry of that name in the served drawer becomes
 * unreachable, and a file that stops answering without a word gets diagnosed
 * as a network fault.
 */
VOID  http_term_announce(const char *root, const char *dotted, UWORD port,
                         const char *url);

/*
 * Say what the Shell is doing: Execute()'s answer, and the first packets of a
 * session by name.  A Shell that started and then did nothing and a Shell that
 * never started look identical from the far end of a socket, and the
 * difference between them is which packets arrive.
 */
VOID  http_term_trace(BOOL on);

/* TRUE when a session can be started: init happened and none is running. */
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
 * read.  The archived branch computed it from the descriptors in the current
 * select() call and deadlocked.  The far side was by then inside Close() on
 * the other pipe waiting for an ACTION_END only this process can answer, and
 * with the port out of the mask nothing woke to answer it.
 */
ULONG http_term_sigmask(VOID);

/* Answer whatever the Shell has sent.  Cheap when there is nothing. */
VOID  http_term_service(VOID);

/* Keystrokes to the Shell.  Returns the bytes taken, which can be short when
   the Shell is not reading.  The rest is the caller's to keep. */
LONG  http_term_write(const UBYTE *data, LONG len);

/* The Shell's output.  0 when there is none yet. */
LONG  http_term_read(UBYTE *buf, LONG len);

/*
 * How much output is waiting.  Asked rather than discovered by reading,
 * because the caller has to decide whether to put its socket in the writable
 * set before it has anywhere to put the bytes.  A socket offered as writable
 * with nothing to write turns the server's wait into a spin.
 */
ULONG http_term_pending(VOID);

/* The person stopped typing: the Shell reads end of file and exits. */
VOID  http_term_eof(VOID);

/* Ctrl-C, to the Shell's own process. */
VOID  http_term_break(VOID);

/* ------------------------------------------------------------ the mode --- */

/*
 * RAW, as ACTION_SCREEN_MODE last said.
 *
 * This is the reason the module answers the packets a console answers and not
 * the packets a pipe answers.  SetMode(fh, 1) is how an AmigaOS program asks
 * for no echo and one keystroke at a time.  ssh's getpass() calls it around a
 * password prompt, Ed calls it for the whole edit, More calls it to read a key
 * at a prompt.  A pipe cannot answer it, so none of those worked and the
 * password was drawn on the screen as it was typed.
 */
BOOL  http_term_raw(VOID);

/*
 * The mode as a word for the page, or NULL when the page already knows.
 *
 * A peek.  The caller decides whether it wants to be offered a writable socket
 * before it has anywhere to put the frame, the same reason http_term_pending()
 * is asked rather than discovered.  http_term_mode_sent() is the take.
 */
const char *http_term_mode_word(VOID);
VOID        http_term_mode_sent(VOID);

/*
 * The write/frame counters, when the page has asked for them with `stats`.
 *
 * They answer the one question the far end cannot: how many ACTION_WRITE
 * packets a program sent to produce the frames that arrived.  That ratio says
 * whether slowness is structural, a frame and a round trip for each cursor
 * move and each few characters printed, or is the speed of the guest.  Same
 * peek-then-take pair as the mode word.
 */
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
 * Everything above is the Shell.  This is the other half of the same subject:
 * a socket that has been upgraded, and what one pass of the server's event
 * loop does with it.
 *
 * It is here and not in httpd.c on purpose.  httpd.c is 6300 lines and routes
 * twelve methods, and more than one thing is added to it at a time.  A feature
 * whose logic sits in that file collides with the next one.  A feature whose
 * logic sits in its own translation unit is a routing entry.  So httpd.c keeps
 * what is genuinely HTTP -- the headers it reads, the refusals it writes, and
 * the 101 -- and everything after the 101 is here.
 *
 * The cost of that is this header: the session has to be told the socket, the
 * library to call it through, and a buffer to build frames in.  The buffer is
 * borrowed from the caller's connection rather than owned, so nothing here
 * adds memory to a server that has no terminal.
 */

#include "httpws.h"
#include "toolsock.h"

/*
 * The wire, in full.  Both halves of it are here and in
 * src/tools/web/client/wire.ts, and the two must be read together.
 *
 * Binary frames are bytes in both directions.  From the page: what was typed,
 * Latin-1, no framing of its own.  To the page: what the Shell printed.  This
 * never changes and is the only channel with any volume on it.
 *
 * A text frame is one word, and what it asks for is not a keystroke.
 *
 *   From the page:
 *
 *     break            Ctrl-C.  On an Amiga that is a SIGNAL and not a byte,
 *                      so there is no in-band way to send one.
 *     eof              the person stopped typing; the Shell reads end of file
 *                      and exits.
 *     size <c> <r>     the page's window is <c> columns by <r> rows.  Sent on
 *                      connect and on every resize.  Decimal, one space.
 *
 *   To the page:
 *
 *     mode raw         the handler has been put in RAW mode: stop echoing,
 *                      stop editing, send each keystroke as it is typed.
 *     mode cooked      and back again.
 *
 *   Sent once when the session opens and then on every change, so a page that
 *   reconnects never has to guess the mode.  A wrong guess leaks a password.
 *
 * The wire grows by adding words, which is the compatibility rule.  An
 * unrecognised word is ignored at both ends, never an error and never a close.
 * So an older page against a newer server still types and still breaks, a
 * newer page against an older server never hears `mode` and behaves as it did
 * before, and tests that assert only on `break`, `eof` and the byte stream
 * (tests/tools/httpd-drill.py --terminal, tests/tools/run-wsterm.sh) keep
 * passing unchanged.
 *
 * The one thing a client must do is tell a server text frame from Shell
 * output.  It already can, because before this the server sent no text frames
 * at all, so every text frame from the server is a word by construction.
 */

/*
 * What a client has typed that the Shell has not taken yet.  The socket is not
 * read at all while any of it is left, which is the back pressure a frame
 * decoder cannot express on its own, because its sink is handed bytes and has
 * no way to refuse them.
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
 * which is already the first frames.  A browser that opens the socket and
 * types in the same segment is ordinary, and dropping that would drop the
 * first thing said.  The storage remains borrowed until the session ends;
 * the caller's request buffer is otherwise idle after the upgrade.
 */
VOID http_term_sock_begin(HttpTermSock *t, struct Library *sb, LONG sock,
                          UBYTE *out, ULONG out_size,
                          const UBYTE *first, ULONG first_len, ULONG now);

/* Whether this socket wants to be offered as writable.  Asked rather than
   assumed, because a socket offered with nothing to write turns the server's
   wait into a spin. */
BOOL http_term_sock_wants_write(const HttpTermSock *t);

/* One pass.  Each returns FALSE when the connection is finished with. */
BOOL http_term_sock_read(HttpTermSock *t, ULONG now);
BOOL http_term_sock_write(HttpTermSock *t, ULONG now);

/*
 * The idle rule.  A WebSocket has no request to be timed out, so the
 * no-progress rule that governs every other connection would end a session
 * because nobody typed.  What has to be asked is whether the far end is still
 * there, and a ping is how RFC 6455 5.5.2 asks it: one after `timeout` seconds
 * of silence, and the connection goes if the next `timeout` passes with no
 * answer.
 */
BOOL http_term_sock_idle(HttpTermSock *t, ULONG now, ULONG timeout);

/*
 * Whether the far end is still there, asked from outside the session.
 *
 * The same rule http_term_sock_idle() ends a session on, with no side effect,
 * so a second client asking for the terminal can find out whether the one
 * holding it is still answering.
 *
 * One session at a time and a client that vanishes without a close together
 * made the terminal unrecoverable.  A tab that goes away when the network does
 * sends no FIN and no RFC 6455 close, so nothing downstream of the socket ever
 * concludes anything, and every later visitor was told somebody else had it.
 * The machine had to be restarted.
 */
BOOL http_term_sock_stale(const HttpTermSock *t, ULONG now, ULONG timeout);

/*
 * Take this session off its client.
 *
 * A close frame with a code and a reason goes out first, in one blocking-free
 * send, so a tab that is still there says why it stopped rather than showing a
 * bare 1006.  Best effort by design.  This exists for a peer that is not
 * reading, and waiting for it to read is what must not happen.  The caller
 * closes the connection afterwards, which is what stops the Shell.
 */
VOID http_term_sock_evict(HttpTermSock *t, UWORD code);

#endif /* AMINETXDUO_HTTPTERM_H */
