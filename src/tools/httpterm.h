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

#endif /* AMINETXDUO_HTTPTERM_H */
