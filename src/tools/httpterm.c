/*
 * httpterm, an AmigaDOS Shell on the other end of a console handler.  See
 * httpterm.h for what this is, what it stopped being, why it needs a second
 * Process, and why anyone who can reach the port gets a shell.
 *
 * The console parts of this file:
 *
 *   the mode           RAW and COOKED, and which side echoes
 *   the write, scanned the window bounds request, which is not a packet
 *   ACTION_WAIT_CHAR   a timeout that is honoured rather than refused
 *   http_term_service  the packet switch, one comment per packet
 *
 * SPDX-License-Identifier: MIT
 */

#include "httpterm.h"

#include <dos/dostags.h>
#include <dos/dosasl.h>
#include <exec/execbase.h>          /* task lists, for runner lifetime      */
#include <exec/io.h>                /* struct IOStdReq, for ACTION_DISK_INFO */

/*
 * The rings.
 *
 * The Shell's output is the bigger of the two because a command's answer
 * arrives faster than a LAN takes it away -- `list` of a full drawer is
 * kilobytes in one go -- and a ring smaller than one Write() turns it into
 * several packet round trips.  Typing is the other way round.  A person
 * produces bytes far slower than anything drains them, and a whole pasted line
 * is still under a kilobyte.
 *
 * Neither is a limit on what can pass, only on what can be in flight.  A full
 * ring holds the Shell's ACTION_WRITE rather than dropping it, which is the
 * back pressure that stops a runaway command from costing memory.  `type
 * S:Startup-Sequence` on a link that is not draining stops the command, not
 * the server.
 */
#define TERM_OUT_BUF    4096UL
#define TERM_IN_BUF     1024UL

/*
 * 64 KB for the runner, and see httpterm.h for why that is a floor and not a
 * measurement of what it does today.
 *
 * There is no number here for the Shell, and that is Execute()'s doing.  It
 * creates the Shell process itself and takes no tags, so the stack its
 * commands get is the system default and `stack 65536` typed into the session
 * is what changes it, the same as in any other Shell on the machine.
 */
#define TERM_RUNNER_STACK   (64UL * 1024UL)

/*
 * And the stack the Shell gives the commands a person types, which is a
 * different number for a different reason.  See term_runner_main(), where it
 * is set and where the arithmetic and the cost are written down.
 */
#define TERM_SHELL_STACK    (16UL * 1024UL)

/*
 * What the Shell is told before it starts reading.
 *
 * `%S` is the current directory and `%N` is the process number.  The Shell's
 * own default is `%N> ` alone, which leaves out the current directory, and
 * that is what a person in a browser cannot otherwise discover.
 *
 * The number is kept rather than dropped, though there is only ever one
 * session.  It increments per Shell, so it shows whether a reconnect got a new
 * Shell or reattached to the old one.  That matters here more than in an
 * ordinary Shell, because a session can be taken over or abandoned underneath
 * the person and the number is the only visible difference.
 *
 * Sent as Execute()'s command string rather than set on the CLI, because
 * cli_Prompt is a BSTR in a buffer of a size this file does not own, while
 * `Prompt` is a Shell built-in (kickstart/shell/prompt.c) and setting it is
 * what the built-in is for.  Execute() runs the string before it starts
 * reading the handle -- "after the (possibly empty) commandString is
 * performed, subsequent input is read from the specified input file handle"
 * -- so it is in force from the first prompt the person sees.
 */
#define TERM_SHELL_SETUP    "prompt \"%N.%S> \""

/* When http_term_shutdown() reports that it is still waiting.  It cannot
   safely stop waiting: every runner executes out of this command's load
   segment, which DOS unloads when the parent returns. */
#define TERM_STOP_WARN_TICKS 500    /* of 1/50 s                            */

/*
 * How many passes of the server's loop a Shell gets to notice its end of file
 * before it is sent another Ctrl-C.
 *
 * Passes and not seconds because that is what this file can count.  The loop
 * ticks at least four times a second and oftener when anything is happening,
 * so twenty is a few seconds of a Shell that has not answered.  It repeats
 * rather than firing once, because a command polling for a break can be
 * between polls when the first one arrives.
 */
#define TERM_STOP_PASSES    20

/*
 * How long the person waiting waits, which is five seconds.
 *
 * A Shell that cannot be made to exit must not cost the machine its terminal.
 * A session ended while `ssh` was at a password prompt left a Shell inside a
 * command that answered neither its end of file nor two Ctrl-Cs, and the
 * terminal was then refused to everybody until the machine was restarted.
 *
 * So after this long the old session is abandoned rather than waited for.  Its
 * generation stops being routed (see term_gen), every packet it sends is
 * refused, and the next visitor gets a Shell.  What is left behind is one
 * Process, its stack, and the small runner record, holding
 * two file handles that fail everything, which is the state most commands do
 * eventually exit from.
 *
 * Five seconds because it is the wait a person sees.  A Shell doing nothing
 * exits in well under one, and a Shell still there after five does not exit
 * after ten either.
 */
#define TERM_ABANDON_TICKS  250     /* of 1/50 s                            */

/* ------------------------------------------------------------- the rings --- */

typedef struct TermPipe
{
    UBYTE            *buf;
    ULONG             size;
    ULONG             count;
    ULONG             rd;
    ULONG             wr;

    struct DosPacket *held;         /* one this pipe cannot answer yet      */

    UBYTE             dosread;      /* the Shell reads it (its stdin)       */
    UBYTE             doswrite;     /* the Shell writes it (its stdout)     */
    UBYTE             dosend;       /* ACTION_END has arrived on it         */
    UBYTE             closed;       /* this side is finished with it        */
} TermPipe;

static TermPipe term_in;            /* what the person types                */
static TermPipe term_out;           /* what the Shell prints                */

static ULONG ring_used(const TermPipe *p) { return p->count; }
static ULONG ring_free(const TermPipe *p) { return p->size - p->count; }

/* ------------------------------------------------------------ the mode --- */

/*
 * RAW or COOKED.
 *
 * ACTION_SCREEN_MODE is what SetMode() sends, and a console is the only kind
 * of handler that answers it: "For CON:, use 1 to go to RAW: mode, 0 for CON:
 * mode" (dos.library/SetMode).  In RAW a console stops echoing and hands over
 * each keystroke as it is typed.  In COOKED it echoes, edits the line, and
 * gives it up at the Return.
 *
 * The browser echoes, not the handler.  A console handler echoes because the
 * person and the handler are on the same machine.  Here they are not, and
 * every echoed byte would cross a LAN twice before the letter appeared.  So
 * the echo and the line editing stay in the page, where they already were, and
 * the mode is sent to the page rather than chosen by a query parameter, so the
 * page stops echoing at the instant the handler is told to.  That is the
 * password fix: ssh's getpass() calls SetMode(Input(), 1), this records it,
 * the page is told `mode raw`, and nothing draws what is typed next.
 *
 * What the handler owns is the delivery rule, which cannot be delegated.
 * COOKED holds an ACTION_READ until there is a line to answer it with, and RAW
 * answers it with the first byte.  A page that has not caught up with the mode
 * therefore cannot turn one keystroke into one command line.
 *
 * SetMode() carries no file handle -- dos.library's support.asm sends
 * sendpkt1(port, ACTION_SCREEN_MODE, mode) and nothing else -- so the mode is
 * the session's and not one handle's.  With one Shell and one page that is
 * right, and it is the same reason ACTION_WAIT_CHAR below can be answered
 * about the input ring without being told which handle is being asked about.
 */
static UBYTE term_raw;              /* the handler is in RAW mode           */
static UBYTE term_mode_pending;     /* and the page has not been told yet   */

/*
 * How big the page says its window is.  Reported to a program that asks, and
 * see term_write_scan() for the asking.  80x25 until a page says otherwise,
 * which is what an Amiga console is on a PAL screen and what nearly every
 * program falls back to on its own.
 */
static UWORD term_cols = 80;
static UWORD term_rows = 25;

/*
 * A read that must be answered whatever the mode says.
 *
 * Set when the handler itself puts bytes in the input ring, which happens only
 * for a window bounds report.  Those bytes answer a program that is waiting
 * for them and no Return is coming to release a COOKED read.  Cleared when the
 * ring empties.
 */
static UBYTE term_in_urgent;

/* ------------------------------------------------------------- counting --- */

/*
 * What the outside cannot see, counted here.
 *
 * A drill on the far end of the socket can count frames, their sizes and the
 * bytes on the wire.  What it cannot see is how many ACTION_WRITE packets the
 * program on this side sent to produce them, and that ratio is the question
 * when something is slow.  One frame per write means each cursor move and each
 * few characters printed costs a packet, a wake, a frame and a round trip, so
 * coalescing is worth doing.  Frames far fewer than writes means the
 * coalescing already happens and the time is going somewhere else.
 *
 * Free when nobody asks: four counters incremented on paths that were already
 * running, and a word to read them back.  No printing, because a tool_printf()
 * per packet is what the TRACE does and it changes the thing it measures.
 */
static ULONG term_st_writes;        /* ACTION_WRITE packets answered        */
static ULONG term_st_wbytes;        /* bytes taken from them                */
static ULONG term_st_frames;        /* binary frames handed to the socket   */
static ULONG term_st_fbytes;        /* payload bytes in them                */
static UBYTE term_st_pending;       /* a `stats` reply is owed to the page  */

static ULONG ring_put(TermPipe *p, const UBYTE *src, ULONG n)
{
    ULONG done = 0;

    if (n > ring_free(p))
        n = ring_free(p);

    while (done < n)
    {
        ULONG run = p->size - p->wr;

        if (run > n - done)
            run = n - done;

        CopyMem((APTR)(src + done), p->buf + p->wr, run);
        p->wr = (p->wr + run) % p->size;
        done += run;
    }
    p->count += done;

    return done;
}

static ULONG ring_get(TermPipe *p, UBYTE *dst, ULONG n)
{
    ULONG done = 0;

    if (n > ring_used(p))
        n = ring_used(p);

    while (done < n)
    {
        ULONG run = p->size - p->rd;

        if (run > n - done)
            run = n - done;

        CopyMem(p->buf + p->rd, (APTR)(dst + done), run);
        p->rd = (p->rd + run) % p->size;
        done += run;
    }
    p->count -= done;

    return done;
}

/*
 * The record is handed over through the child's tc_UserData with SIGF_SINGLE
 * as the handshake, because the child starts running the moment
 * CreateNewProc() returns and a plain global would race it.  The child waits
 * before it looks.
 */
typedef struct TermRunner
{
    struct TermRunner *rn_Next;     /* every runner whose code is still live */
    struct Task  *rn_Task;          /* CreateNewProc(), until Exec removes it */
    struct Task  *rn_Parent;
    BPTR          rn_In;            /* the Shell's stdin                    */
    BPTR          rn_Out;           /* the Shell's stdout                   */
    LONG          rn_Rc;
    LONG          rn_Err;           /* IoErr(), when no shell would start   */
    volatile LONG rn_Done;
} TermRunner;

/*
 * One per session, from the heap, and freed only when its runner has
 * finished with it.
 *
 * It was a single static, because a record that is never freed does not need
 * the heap.  That held while the only end to a session was its Shell exiting.
 * A session whose Shell does not exit is now abandoned instead (see
 * TERM_ABANDON_TICKS), and the abandoned runner goes on owning its record,
 * writing rn_Done into it whenever it finally returns.  One static shared with
 * the next session would mean the old runner's last act reaps the new session.
 *
 * Kept on a list even while abandoned, because shutdown must know whether code
 * from this command's load segment is still executing.  A completed record is
 * reclaimed at the next start; one whose Shell never exits remains the small
 * fixed bookkeeping cost of making the terminal recoverable.
 */
static TermRunner *term_runner;     /* the current session's record        */
static TermRunner *term_runners;    /* including abandoned live runners    */
static UBYTE      term_active;      /* a Shell has been started             */
static UBYTE      term_reaped;      /* and its exit code has been collected */
static UBYTE      term_stopping;    /* it has been asked to go              */
static UBYTE      term_trace;       /* say what the Shell is doing          */
static UWORD      term_traced;      /* how many packets have been reported  */
static UBYTE      term_said_rc;     /* Execute()'s answer has been printed   */
static UWORD      term_stop_passes;
static LONG       term_rc = -1;

/*
 * The session has been let go of without its Shell having gone.  See
 * TERM_ABANDON_TICKS: from here the terminal is available again and the old
 * Shell's handles route nowhere.
 */
static UBYTE      term_abandoned;
static ULONG      term_stop_at;     /* fiftieths, when stopping began       */

/*
 * IoErr() from the last Shell that did not start, kept here and not read back
 * out of the runner record.  The record belongs to the runner and can have
 * been abandoned with it.  This is the one field anybody outside wants after
 * the session has ended.
 */
static LONG       term_err;

/*
 * Whoever is reading or writing these pipes, so http_term_break() has
 * something to signal.  Learned from the packets rather than from a name.  A
 * DosPacket arrives with dp_Port naming the sender's reply port, and a
 * Process's port has that Process in mp_SigTask, so this is exact where
 * FindTask() on a name dos.library chose would be a guess.
 *
 * It is also the right target.  A Shell runs each command in its own process,
 * so the task that is inside Read() at any moment is the one a person pressing
 * Ctrl-C means.
 */
static struct Task *term_shell_task;

/*
 * And the port dos.library named, when it named one.  ACTION_CHANGE_SIGNAL
 * carries it, and it is preferred over the inference above because it is still
 * right while a command is running.  dos.library re-sends it around every
 * Execute().
 */
static struct MsgPort *term_break_port;

/*
 * What Info() calls a console.  These are the con-handler's own two constants.
 * They are private to it and are in no NDK header, so they are written out
 * here with the bytes they are made of.
 */
#define TERM_DISK_CON     0x434F4E00L     /* 'CON\0' */
#define TERM_DISK_RAWCON  0x52415700L     /* 'RAW\0' */

/*
 * The IORequest ACTION_DISK_INFO hands out.  Zero throughout, so io_Unit is
 * NULL and that is the answer.  See the packet for why it has to exist and why
 * it has to stay empty.  Static because it is read by other processes and is
 * never freed.
 */
static struct IOStdReq term_ioreq;

/* ---------------------------------------------------------- the DOS side --- */

static struct MsgPort *term_port;

/*
 * The index a FileHandle carries in fh_Arg1, so a packet can be routed back to
 * one of the two pipes.  There is one port for both, which is all that is
 * needed here: only READ, WRITE and END carry fh_Arg1 at all, and nothing else
 * this answers needs to know which handle it was asked about.
 * src/bsdsocket/tcp_handler.c gives a port to each of its sessions because its
 * other packets do.
 */
#define TERM_ID_IN      1
#define TERM_ID_OUT     2

/*
 * ...and a third, which is what `*` means.
 *
 * The Shell's two handles are one-way each, because that is how Execute() was
 * given them.  A console is not.  A program that opens `*` gets one handle and
 * both reads and writes it, and programs do: Ed's default window is a RAW: of
 * its own, and `Ed WINDOW=* file` asks it to use the console it was started
 * from instead.
 *
 * So a FIND packet gets a handle with this id, and the routing below sends its
 * reads to the input ring and its writes to the output one.  It is the same
 * session, and there is nothing else it can be.
 */
#define TERM_ID_CON     3

/*
 * ...and which session it belongs to.
 *
 * fh_Arg1 carries a generation above the id.  A session that was abandoned
 * rather than reaped -- see TERM_ABANDON_TICKS -- leaves a Shell alive with
 * two handles still naming this port, and those handles must never again reach
 * a ring the next session is using.  The generation is what stops them.
 * term_pipe_of() answers NULL for anything that is not the current session,
 * every packet from the old Shell is refused, and the rings belong to the
 * person now typing.
 *
 * Sixteen bits of it, which wraps after 65536 sessions and is harmless when it
 * does, because a handle would have to survive that many to collide.
 */
static UWORD term_gen = 1;

static LONG term_handle_arg(LONG id)
{
    return (LONG)(((ULONG)term_gen << 8) | (ULONG)id);
}

static TermPipe *term_pipe_of(LONG arg, LONG type)
{
    LONG id;

    if ((ULONG)arg >> 8 != (ULONG)term_gen)
        return NULL;                /* a session that has been let go of    */

    id = arg & 0xFF;

    /* The one handle that is both.  Which ring it means is the packet. */
    if (id == TERM_ID_CON)
        return (type == ACTION_READ) ? &term_in : &term_out;

    if (id == TERM_ID_IN)  return &term_in;
    if (id == TERM_ID_OUT) return &term_out;
    return NULL;
}

/*
 * Reply a packet.  Not ReplyPkt(), because dp_Port has to name the port the
 * packet comes back on, which is this one, and ReplyPkt() stamps it with the
 * current process's pr_MsgPort.  The same reason tcp_handler.c has its own.
 */
static VOID term_reply(struct DosPacket *pkt, LONG res1, LONG res2)
{
    struct MsgPort *reply = pkt->dp_Port;

    pkt->dp_Res1 = res1;
    pkt->dp_Res2 = res2;
    pkt->dp_Port = term_port;
    PutMsg(reply, pkt->dp_Link);
}

/* Whether an untagged console packet came from the process dos.library most
   recently named with ACTION_CHANGE_SIGNAL.

   SetMode(), WaitForChar() and Open("*") do not carry the originating
   handle's fh_Arg1, so their generation cannot be checked directly.  Their
   dp_Port is the sender's reply port, though, and cli_init.c/execute.c first
   give that same port to ACTION_CHANGE_SIGNAL on the tagged input handle.
   This is what keeps a process left behind by an abandoned generation from
   operating the replacement session through packets with no handle argument. */
static BOOL term_packet_current(const struct DosPacket *pkt)
{
    if (!term_active || pkt->dp_Port == NULL)
        return FALSE;

    if (term_break_port != NULL)
        return (pkt->dp_Port == term_break_port) ? TRUE : FALSE;

    return (term_shell_task != NULL &&
            pkt->dp_Port->mp_SigTask == term_shell_task) ? TRUE : FALSE;
}

/* ------------------------------------------------- the write, scanned --- */

/*
 * What a program writes that is not output.
 *
 *   " q  0   aWSR  WINDOW STATUS REQUEST (private Amiga sequence)
 *     r  4   aWBR  WINDOW BOUNDS REPORT  (private Amiga Read sequence)"
 *                                          -- console.device autodoc
 *
 * That is how an AmigaOS program asks how big its window is, and there is no
 * DOS packet for it.  The question goes out through ACTION_WRITE and the
 * answer comes back through ACTION_READ.  Ed sends `CSI SP q` before it draws
 * anything and then sits in `while (rdch() != 0x9B)`, so an unanswered request
 * is not a wrong size, it is Ed never starting.
 *
 * So the write stream is scanned.  A CSI sequence is held while it is being
 * assembled and then either swallowed, if it turns out to be one of the three
 * a console consumes, or passed through byte for byte if it is anything else.
 *
 * The three that are swallowed:
 *
 *   q   the window status request, answered into the input ring.
 *   {   SET RAW EVENTS.  Ed asks for class 12, IECLASS_SIZEWINDOW, which is
 *       how it hears about a resize while it is running.
 *   }   RESET RAW EVENTS, the other half of the pair.
 *
 * All three are instructions to a console and mean nothing to a browser.
 * Everything else -- colours, cursor moves, clears -- is display and goes
 * straight out, which is why the page rewrites 0x9B into ESC [ rather than
 * this doing anything about it.
 *
 * The common case costs nothing measurable and nothing visible.  A byte is
 * held only while it is inside a sequence, and a sequence ends at its final
 * byte, so output is stranded only when a program stops writing in the middle
 * of one.  A newline, a letter and anything below 0x20 all end the sequence
 * and flush it.  `Echo "*e"` prints ESC and then LF, and both arrive.
 */
#define TERM_SEQ_MAX    24      /* a runaway parameter list is not a sequence */

static UBYTE term_seq[TERM_SEQ_MAX];
static UBYTE term_seq_n;                /* 0 when not inside a sequence      */
static UBYTE term_seq_esc;              /* it began with ESC and wants a '[' */

/* A program asked to hear about resizes: see IECLASS_SIZEWINDOW below. */
static UBYTE term_want_resize;

/* Whether the input ring holds a whole line.  Only the input ring is ever
   read, and only COOKED mode asks. */
static BOOL term_has_line(const TermPipe *p)
{
    ULONG i;
    ULONG at = p->rd;

    for (i = 0; i < p->count; i++)
    {
        if (p->buf[at] == (UBYTE)'\n')
            return TRUE;
        at++;
        if (at == p->size)
            at = 0;
    }

    return FALSE;
}

/*
 * Whether an ACTION_READ on this pipe can be answered at once, which is where
 * RAW and COOKED differ.
 *
 * COOKED waits for a Return because that is what a line-mode console does, and
 * because it guards against the mode changing under a page that has not caught
 * up.  A keystroke that arrived while the page still thought it was in RAW is
 * held here rather than handed to the Shell as a command of its own.  A full
 * ring releases anyway, so that a paste bigger than the ring with no newline
 * in it cannot deadlock, and so does a report the handler wrote itself.
 */
static BOOL term_readable(const TermPipe *p)
{
    if (ring_used(p) == 0UL)
        return FALSE;

    if (p != &term_in || term_raw || term_in_urgent)
        return TRUE;

    if (ring_free(p) == 0UL)
        return TRUE;

    return term_has_line(p);
}

static VOID term_kick(VOID);

/* A decimal number, because there is no printf here and this is the whole of
   the formatting a console report needs. */
static ULONG term_num(UBYTE *dst, ULONG v)
{
    UBYTE tmp[8];
    ULONG n = 0;
    ULONG i;

    if (v == 0UL)
    {
        dst[0] = (UBYTE)'0';
        return 1UL;
    }

    while (v > 0UL && n < sizeof(tmp))
    {
        tmp[n++] = (UBYTE)('0' + (v % 10UL));
        v /= 10UL;
    }

    for (i = 0; i < n; i++)
        dst[i] = tmp[n - 1UL - i];

    return n;
}

/*
 * Put something of the handler's own into the input ring.
 *
 * A console's answers to a program come back the way a keystroke does, out of
 * Read(), which is why this goes in beside what was typed rather than out
 * beside what was printed.  term_in_urgent releases it whatever the mode says.
 * No Return is coming for a report, and a COOKED read waiting for one would
 * hold the answer to a question the program is blocked on.
 *
 * Dropped when there is no room.  The ring is a kilobyte and a report is
 * twenty bytes, so no room means a ring already full of unread typing, and the
 * program asks again.
 */
static VOID term_inject(const UBYTE *b, ULONG n)
{
    if (ring_free(&term_in) < n)
        return;

    (VOID)ring_put(&term_in, b, n);
    term_in_urgent = 1;
    term_kick();
}

/* CSI 1;1;<rows>;<cols> SP r -- top, left, bottom, right, which is what the
   console.device autodoc's aWBR is and what Ed subtracts to get its size. */
static VOID term_bounds_report(VOID)
{
    UBYTE b[24];
    ULONG n = 0;

    b[n++] = 0x9B;
    b[n++] = (UBYTE)'1';
    b[n++] = (UBYTE)';';
    b[n++] = (UBYTE)'1';
    b[n++] = (UBYTE)';';
    n += term_num(&b[n], (ULONG)term_rows);
    b[n++] = (UBYTE)';';
    n += term_num(&b[n], (ULONG)term_cols);
    b[n++] = (UBYTE)' ';
    b[n++] = (UBYTE)'r';

    term_inject(b, n);
}

/*
 * CSI 12;...| -- aIER, an input event report, class 12 IECLASS_SIZEWINDOW.
 *
 * Eight parameters because that is what the sequence has.  Ed reads the first
 * and ignores the rest, and a program that reads all eight gets zeroes for the
 * ones that describe a mouse.
 */
static VOID term_resize_event(VOID)
{
    static const char TAIL[] = "12;0;0;0;0;0;0;0|";
    UBYTE b[24];
    ULONG n = 0;
    ULONG i;

    b[n++] = 0x9B;
    for (i = 0; TAIL[i] != '\0'; i++)
        b[n++] = (UBYTE)TAIL[i];

    term_inject(b, n);
}

/* Give up on the held sequence, which is ordinary output after all.  `extra`
   is the byte that ended it, or NULL.  The caller has made room for both. */
static VOID term_seq_flush(const UBYTE *extra)
{
    if (term_seq_n > 0)
        (VOID)ring_put(&term_out, term_seq, (ULONG)term_seq_n);

    term_seq_n   = 0;
    term_seq_esc = 0;

    if (extra != NULL)
        (VOID)ring_put(&term_out, extra, 1UL);
}

/* Whether the held sequence's parameters include this number, so that
   `CSI 12 {` is told from `CSI 2 {`. */
static BOOL term_seq_has(ULONG want)
{
    ULONG i   = term_seq_esc ? 2UL : 1UL;
    ULONG v   = 0;
    BOOL  any = FALSE;

    for (;;)
    {
        if (i < (ULONG)term_seq_n &&
            term_seq[i] >= (UBYTE)'0' && term_seq[i] <= (UBYTE)'9')
        {
            v = v * 10UL + (ULONG)(term_seq[i] - (UBYTE)'0');
            any = TRUE;
            i++;
            continue;
        }

        if (any && v == want)
            return TRUE;

        if (i >= (ULONG)term_seq_n)
            return FALSE;

        v   = 0;
        any = FALSE;
        i++;
    }
}

/*
 * The Shell's output, scanned on its way into the ring.  Returns how many of
 * the caller's bytes were taken, which is not the same as how many reached the
 * ring, because a swallowed sequence is taken and never appears.
 *
 * See the section header above for what is swallowed and why nothing else is.
 */
static ULONG term_out_put(const UBYTE *src, ULONG len)
{
    ULONG done = 0;

    while (done < len)
    {
        UBYTE b = src[done];

        /* Room for everything held plus this byte, checked before the byte is
           taken.  That is the invariant that lets term_seq_flush() write
           without checking, so a sequence can never end up half in the ring. */
        if (ring_free(&term_out) < (ULONG)term_seq_n + 1UL)
            break;

        done++;

        if (term_seq_n == 0)
        {
            if (b == 0x9B || b == 0x1B)
            {
                term_seq[0]  = b;
                term_seq_n   = 1;
                term_seq_esc = (UBYTE)((b == 0x1B) ? 1 : 0);
            }
            else
            {
                (VOID)ring_put(&term_out, &b, 1UL);
            }
            continue;
        }

        /* ESC introduces a control sequence only when a '[' follows it.  ESC c
           is a reset and ESC anything-else is not ours. */
        if (term_seq_esc && term_seq_n == 1)
        {
            if (b == (UBYTE)'[')
                term_seq[term_seq_n++] = b;
            else
                term_seq_flush(&b);
            continue;
        }

        /* Parameters (0x30..0x3F) and intermediates (0x20..0x2F). */
        if (b >= 0x20 && b <= 0x3F)
        {
            term_seq[term_seq_n++] = b;

            /* Longer than any real sequence: it was never one. */
            if (term_seq_n == (UBYTE)TERM_SEQ_MAX)
                term_seq_flush(NULL);

            continue;
        }

        /* Anything else ends the sequence, final byte or not. */
        switch (b)
        {
            case (UBYTE)'q':                /* aWSR, window status request  */
                term_seq_n   = 0;
                term_seq_esc = 0;
                term_bounds_report();
                break;

            case (UBYTE)'{':                /* aSRE, set raw events         */
                if (term_seq_has(12UL))
                    term_want_resize = 1;
                term_seq_n   = 0;
                term_seq_esc = 0;
                break;

            case (UBYTE)'}':                /* aRRE, reset raw events       */
                if (term_seq_has(12UL))
                    term_want_resize = 0;
                term_seq_n   = 0;
                term_seq_esc = 0;
                break;

            default:
                term_seq_flush(&b);
                break;
        }
    }

    return done;
}

/* Try to answer whatever this pipe has parked.  Called after either side
   moves bytes or closes, and from the service loop. */
static VOID term_retry(TermPipe *p)
{
    struct DosPacket *pkt = p->held;
    LONG              n;

    if (pkt == NULL)
        return;

    if (pkt->dp_Type == ACTION_READ)
    {
        if (term_readable(p))
        {
            n = (LONG)ring_get(p, (UBYTE *)pkt->dp_Arg2, (ULONG)pkt->dp_Arg3);

            if (p == &term_in && ring_used(p) == 0UL)
                term_in_urgent = 0;
        }
        else if (p->closed)
        {
            n = 0;                  /* end of file: the person stopped      */
        }
        else
        {
            return;                 /* still nothing; keep waiting          */
        }
    }
    else                            /* ACTION_WRITE                         */
    {
        /* Closed is checked first, and not once the ring is full.  This side
           gives up on a session by closing both pipes, and a Shell that then
           went on writing successfully into 4 KB nobody reads takes that much
           longer to stop. */
        if (p->closed)
        {
            /* Nobody reads it now.  -1 with a real error, so a command writing
               into a channel that has gone fails rather than looping on a
               short write for ever. */
            p->held = NULL;
            term_reply(pkt, -1, ERROR_INVALID_LOCK);
            return;
        }
        /*
         * Room for a whole held sequence plus a byte, rather than for a byte.
         * term_out_put() holds a control sequence while it assembles it and
         * must be able to give the whole thing back if it turns out to be
         * ordinary output, so it needs that headroom to make progress.  A
         * write answered with zero bytes taken makes the caller loop.
         */
        else if (ring_free(p) > (ULONG)TERM_SEQ_MAX)
        {
            n = (p == &term_out)
                    ? (LONG)term_out_put((const UBYTE *)pkt->dp_Arg2,
                                         (ULONG)pkt->dp_Arg3)
                    : (LONG)ring_put(p, (const UBYTE *)pkt->dp_Arg2,
                                     (ULONG)pkt->dp_Arg3);

            if (n == 0)
                return;

            if (p == &term_out)
            {
                term_st_writes++;
                term_st_wbytes += (ULONG)n;
            }
        }
        else
        {
            return;
        }
    }

    p->held = NULL;
    term_reply(pkt, n, 0);
}

/* -------------------------------------------------- ACTION_WAIT_CHAR --- */

/*
 * WaitForChar(), which a console has to answer truthfully.
 *
 *   "If a character is available to be read from 'file' within the time (in
 *   microseconds) indicated by 'timeout', WaitForChar() returns -1 (TRUE)."
 *   -- dos.library/WaitForChar
 *
 * A pipe answered DOSFALSE to every one of these, which passed only because
 * nothing that reads a pipe asks.  A console client does.  ssh's console
 * reader is `while (!quit) { if (!WaitForChar(h, 100000)) continue; Read(h,
 * &c, 1); }`, and an instant DOSFALSE turns that loop into a spin that eats
 * the machine.  So the packet is held until either a character arrives or the
 * timeout it was given has passed.
 *
 * The packet carries no file handle -- dos.library sends sendpkt1(port,
 * ACTION_WAIT_CHAR, timeout) -- so it cannot say which of the two handles it
 * means.  Here it can only mean the one that is read.
 */
static struct DosPacket *term_wait_pkt;
static ULONG             term_wait_from;   /* fiftieths, see term_ticks()   */
static ULONG             term_wait_until;

/*
 * A clock in fiftieths of a second, for the timeout above and nothing else.
 *
 * DateStamp()'s minute-and-tick pair, which counts 0..4319999 and starts again
 * at midnight.  The wrap is handled by remembering where the wait began.  A
 * clock reading earlier than that means midnight has happened, and the wait is
 * answered with no character one tick early, once a day, on a call whose whole
 * contract is a poll.  The alternative is a timer.device request per
 * WaitForChar(), which is a unit and an IORequest for a number that is allowed
 * to be approximate.
 */
static ULONG term_ticks(VOID)
{
    struct DateStamp ds;

    DateStamp(&ds);

    return (ULONG)ds.ds_Minute * 3000UL + (ULONG)ds.ds_Tick;
}

/* A Read() would return at once, because there is either input or an end of
   file, and WaitForChar() reports on both.  The archived pipe answered
   DOSFALSE at end of file, which left the caller waiting for a character that
   was never coming. */
static BOOL term_char_ready(VOID)
{
    return (term_readable(&term_in) || term_in.closed || term_in.dosend)
               ? TRUE : FALSE;
}

/* Answer a parked WaitForChar() if it can be answered yet. */
static VOID term_wait_service(VOID)
{
    struct DosPacket *pkt = term_wait_pkt;
    ULONG             now;

    if (pkt == NULL)
        return;

    if (term_char_ready())
    {
        term_wait_pkt = NULL;
        term_reply(pkt, DOSTRUE, 0);
        return;
    }

    now = term_ticks();

    if (now < term_wait_from || now >= term_wait_until)
    {
        term_wait_pkt = NULL;
        term_reply(pkt, DOSFALSE, 0);
    }
}

/* Input moved or ended, so whoever was waiting for it can now go. */
static VOID term_kick(VOID)
{
    term_retry(&term_in);
    term_wait_service();
}

VOID http_term_trace(BOOL on)
{
    term_trace = on ? 1 : 0;
}

/* What a packet is called, for the trace.  Only the ones a Shell on a pipe
   sends.  Anything else is printed as its number, which is what a person then
   looks up. */
static const char *term_action(LONG type)
{
    switch (type)
    {
        case ACTION_READ:           return "READ";
        case ACTION_WRITE:          return "WRITE";
        case ACTION_END:            return "END";
        case ACTION_IS_FILESYSTEM:  return "IS_FILESYSTEM";
        case ACTION_SEEK:           return "SEEK";
        case ACTION_WAIT_CHAR:      return "WAIT_CHAR";
        case ACTION_FLUSH:          return "FLUSH";
        case ACTION_SET_FILE_SIZE:  return "SET_FILE_SIZE";
        case ACTION_SCREEN_MODE:    return "SCREEN_MODE";
        case ACTION_CHANGE_SIGNAL:  return "CHANGE_SIGNAL";
        case ACTION_DISK_INFO:      return "DISK_INFO";
        default:                    return NULL;
    }
}

VOID http_term_service(VOID)
{
    struct Message *msg;

    if (term_port == NULL)
        return;

    /*
     * What Execute() said, printed from here and not from the runner.  The
     * runner is another Process and its Output() is the NIL: CreateNewProc
     * gave it, so anything it printed would go nowhere.  pr_CES had the same
     * fault one round trip earlier.
     */
    if (term_trace && term_active && !term_said_rc &&
        term_runner != NULL && term_runner->rn_Done != 0)
    {
        term_said_rc = 1;
        tool_printf("httpd: terminal: Execute() returned %ld (IoErr %ld)\n",
                    term_runner->rn_Rc, (LONG)term_runner->rn_Err);
        (VOID)Flush(Output());
    }

    /*
     * Notice a Shell that has gone, before the packets are looked at.  Gone is
     * not the runner returning.
     *
     * Execute() can create the Shell and return at once -- the autodoc says it
     * makes "a new interactive Shell process just like those created with the
     * NewShell command" -- so the runner publishing rn_Done says nothing about
     * whether the Shell is still there.  What does say it is the Shell closing
     * the two handles, which arrives here as ACTION_END on each, and that
     * answer is the same whichever way Execute() behaved.
     *
     * The one case rn_Done decides is failure.  Execute() returns a BOOLEAN,
     * and a false one means no Shell was started at all.  Then nothing sends
     * an ACTION_END and the session has to end on the flag.
     */
    /* While it is going, keep the output ring empty and keep answering, so a
       Shell in the middle of a command can reach the end of file waiting for
       it. */
    if (term_stopping && !term_reaped)
    {
        term_stop_passes++;

        if (ring_used(&term_out) > 0UL)
        {
            term_out.count = 0;
            term_out.rd    = 0;
            term_out.wr    = 0;
        }

        term_kick();
        term_retry(&term_out);

        /*
         * More Ctrl-Cs, at intervals, rather than one more.  A command polls
         * for a break between other things and can be somewhere else when one
         * arrives.  Repeating costs a Signal() every few seconds and catches
         * the ones a single shot misses.
         */
        if (term_stop_passes % TERM_STOP_PASSES == 0)
            http_term_break();

        /*
         * And the deadline.  A Shell that answers neither its end of file nor
         * any number of Ctrl-Cs used to keep the session until it answered,
         * which on the machine that hit it was for ever.  See
         * TERM_ABANDON_TICKS.
         */
        if (!term_abandoned)
        {
            ULONG now = term_ticks();

            if (now < term_stop_at ||
                now - term_stop_at >= (ULONG)TERM_ABANDON_TICKS)
            {
                term_abandoned = 1;
                tool_printf("httpd: the terminal's Shell will not stop. "
                            "It is abandoned, so the next visitor gets one\n");
                (VOID)Flush(Output());
            }
        }
    }

    if (term_active && !term_reaped)
    {
        BOOL done   = (term_runner != NULL && term_runner->rn_Done != 0)
                          ? TRUE : FALSE;
        BOOL failed = (done && term_runner->rn_Rc == 0) ? TRUE : FALSE;
        /*
         * The runner publishes rn_Done only after Execute() has returned and
         * both handles have been closed, and Execute() returns only when the
         * Shell has exited.  So one flag says the whole thing.  dosend is kept
         * beside it because it is the same news by another route, and a
         * session that ended without the runner noticing would otherwise be a
         * session nobody reaps.
         */
        BOOL ended  = (done ||
                       (term_in.dosend && term_out.dosend)) ? TRUE : FALSE;

        if (failed)
        {
            /*
             * tool_printf() and not tool_error(), which is the tree's
             * convention everywhere else and is wrong here.  tool_error()
             * writes to pr_CES, and a server started from a Startup-Sequence
             * with `>DH0:stdout.txt` has a pr_CES that is the boot console and
             * not the redirected stream.  So this line, the one that says why
             * the terminal did not work, was missing from the transcript that
             * was supposed to explain it.  Cost a guest run.
             */
            term_err = term_runner->rn_Err;
            tool_printf("httpd: no Shell started for the terminal "
                        "(IoErr %ld)\n", (LONG)term_err);
            (VOID)Flush(Output());
            term_rc     = -1;
            term_reaped = 1;
        }
        else if (ended)
        {
            term_rc     = 0;
            term_reaped = 1;
        }

        /* The session stays active while its last output drains, but the
           Process that owned these pointers is gone once it is reaped.  A
           late `break` from the page, or http_term_stop() during that drain,
           must not signal through a freed MsgPort or Task. */
        if (term_reaped)
        {
            term_shell_task = NULL;
            term_break_port = NULL;
        }

        /* Not `term_active = 0` yet.  Output the Shell wrote before it exited
           is still in the ring and is the answer to the last command.
           http_term_running() is what tells the two apart. */
    }

    if (term_active && term_reaped && ring_used(&term_out) == 0UL)
    {
        term_active     = 0;
        term_shell_task = NULL;
    }

    while ((msg = GetMsg(term_port)) != NULL)
    {
        struct DosPacket *pkt = (struct DosPacket *)msg->mn_Node.ln_Name;
        TermPipe         *p;

        /*
         * The first packets of a session, under TRACE.  Bounded, because a
         * live session is a packet per keystroke and per line printed, and an
         * unbounded trace of that is a log nobody reads.
         *
         * A Shell that started and then did nothing and a Shell that never
         * started look identical from the far end of a socket, and the
         * difference between them is which packets arrive here.
         */
        if (term_trace && term_traced < 200)
        {
            const char *name = term_action(pkt->dp_Type);

            term_traced++;
            if (name != NULL)
                tool_printf("httpd: terminal: %s on %ld\n", (LONG)name,
                            pkt->dp_Arg1);
            else
                tool_printf("httpd: terminal: packet %ld\n",
                            (LONG)pkt->dp_Type);
            (VOID)Flush(Output());
        }

        switch (pkt->dp_Type)
        {
            case ACTION_READ:
            case ACTION_WRITE:
            case ACTION_END:
                break;

            /*
             * A pipe is not a filesystem.  Answered, and never asked.  This is
             * how 1.3 decided whether a stream was interactive, and V37 and
             * later read fh_Port instead.  See term_handle(), which is where
             * the prompt comes from.
             */
            case ACTION_IS_FILESYSTEM:
                term_reply(pkt, DOSFALSE, 0);
                continue;

            case ACTION_SEEK:
                term_reply(pkt, -1, ERROR_SEEK_ERROR);
                continue;

            /*
             * RAW on, RAW off.  The one packet that makes this a console and
             * not a pipe.  See the mode section at the top for what each half
             * of the pair owns, and http_term_mode_word() for how the page is
             * told.
             */
            case ACTION_SCREEN_MODE:
            {
                UBYTE want = (pkt->dp_Arg1 != 0) ? 1 : 0;

                if (!term_packet_current(pkt))
                {
                    term_reply(pkt, DOSFALSE, ERROR_INVALID_LOCK);
                    continue;
                }

                if (want != term_raw)
                {
                    term_raw          = want;
                    term_mode_pending = 1;

                    /* The rule for answering a parked read has just changed
                       under it, and a keystroke the old rule was holding can
                       already be waiting. */
                    term_kick();
                }

                term_reply(pkt, DOSTRUE, 0);
                continue;
            }

            /*
             * Where to send a Ctrl-C, handed to us rather than guessed.
             *
             * dos.library sends this when it gives a Shell an interactive
             * input handle (cli_init.c) and again around each Execute()
             * (execute.c), with dp_Arg2 the MsgPort of the process that now
             * receives breaks.  A port and not a Task, so the target is
             * mp_SigTask.  term_shell_task below infers the same thing from
             * whoever last sent a packet, which is right almost always.  This
             * is the answer instead of the inference, and it is right while a
             * command is running as well.
             */
            case ACTION_CHANGE_SIGNAL:
            {
                struct MsgPort *was = term_break_port;
                LONG            id  = pkt->dp_Arg1 & 0xFF;

                /* Unlike SetMode() and WaitForChar(), dos.library does pass
                   fh_Arg1 here: cli_init.c and execute.c both send it as the
                   first argument.  Honour its generation.  Without this, a
                   Shell abandoned on the old generation can later redirect
                   Ctrl-C in the replacement session to one of its own tasks,
                   defeating the isolation the generated handles provide. */
                if (((ULONG)pkt->dp_Arg1 >> 8) != (ULONG)term_gen ||
                    (id != TERM_ID_IN && id != TERM_ID_CON))
                {
                    term_reply(pkt, DOSFALSE, ERROR_INVALID_LOCK);
                    continue;
                }

                term_break_port = (struct MsgPort *)pkt->dp_Arg2;
                term_reply(pkt, (LONG)was, 0);
                continue;
            }

            /*
             * Info(), and a console answers it with two fields that are not
             * about a disk at all.  The caller is asking which window it is
             * in, and CON: answers by overloading the struct.  id_VolumeNode
             * is a struct Window * (not a BPTR), and id_InUse is its
             * console.device IORequest, whose io_Unit is the ConUnit a caller
             * then reads cu_XMax/cu_YMax out of.
             *
             * Every field here is chosen against what More does.
             *
             *   DOSTRUE, always.  findWindow() (workbench/utilities/more) ends
             *   with `cleanexit(MSG_MO_NOPACKET)` when this packet answers
             *   false, having tried twice.  DOSFALSE is not a fallback, it is
             *   More refusing to run at all.
             *
             *   id_InUse is a real IORequest of this file's and never zero,
             *   because the same function does
             *
             *       mo->conUnit = ((struct IOStdReq *)id->id_InUse)->io_Unit;
             *
             *   with nothing between it and the dereference.  Zero there is a
             *   read of low memory and then a ConUnit pointer made of whatever
             *   was in it, which More later writes a cursor row through.
             *
             *   io_Unit inside it is NULL, and that is the load-bearing one.
             *   More reads a NULL conUnit as no real console, falls back to
             *   its own 80x24 dummy, and then tracks the cursor row itself --
             *   `if (!RealConUnit) conUnit->cu_YCP += 1` -- which is what makes
             *   it paginate here.  A fabricated ConUnit instead makes More
             *   believe console.device is keeping cu_YCP for it, nothing does,
             *   and More prints the whole file without a pause.  The AUX: path
             *   is the right path for a console with no Intuition window under
             *   it, and this is how it is asked for.
             *
             *   id_VolumeNode is zero, by More's own description of the case:
             *   "mo->conWindow == 0 if AUX".  Every use of it in More is
             *   guarded, and every use of it in Ed is guarded except one:
             *
             *       window = (struct Window *)infodata->id_VolumeNode;
             *       if (window->Flags & SIMPLE_REFRESH) fatal(...);
             *
             *   which reads absolute address 0x14 -- Window.Flags at offset 20
             *   of a null pointer, which on a 68000 is exception vector 5.
             *   That is a ROM address under any Kickstart, it is fixed for a
             *   given one, and 40.68 does not have bit 6 set in it, so Ed
             *   runs.  Measured, not assumed.
             *
             *   Handing out a zeroed struct Window instead, so the read is
             *   deterministic, is worse.  A non-null window turns on Ed's
             *   mouse handling, which dereferences window->RPort->Font on an
             *   input event report of class 2.  That report is bytes, and
             *   bytes come from the browser.  Null is the value that makes
             *   Ed's own guards work.
             *
             * The size a program gets this way is therefore 80x24 and not the
             * page's.  term_write_scan() is the route that answers with the
             * real one, and it is the route Ed takes.
             */
            case ACTION_DISK_INFO:
            {
                struct InfoData *id = (struct InfoData *)BADDR(pkt->dp_Arg1);

                if (id != NULL)
                {
                    id->id_NumSoftErrors = 0;
                    id->id_UnitNumber    = 0;
                    id->id_DiskState     = ID_VALIDATED;
                    id->id_NumBlocks     = 0;
                    id->id_NumBlocksUsed = 0;
                    id->id_BytesPerBlock = 0;
                    id->id_DiskType      = term_raw ? TERM_DISK_RAWCON
                                                    : TERM_DISK_CON;
                    id->id_VolumeNode    = (BPTR)0;
                    id->id_InUse         = (LONG)&term_ioreq;
                }

                term_reply(pkt, DOSTRUE, 0);
                continue;
            }

            /*
             * WaitForChar().  Held rather than refused.  See term_ticks()
             * above for why an instant DOSFALSE is a spin and not an answer.
             *
             * A second one while the first is held is refused, not queued.
             * The caller is one process and is inside one WaitForChar() at a
             * time, the same invariant the held READ relies on.
             */
            case ACTION_WAIT_CHAR:
                if (!term_packet_current(pkt))
                {
                    term_reply(pkt, DOSFALSE, ERROR_INVALID_LOCK);
                    continue;
                }

                if (term_char_ready())
                {
                    term_reply(pkt, DOSTRUE, 0);
                }
                else if (pkt->dp_Arg1 <= 0 || term_wait_pkt != NULL)
                {
                    term_reply(pkt, DOSFALSE, 0);
                }
                else
                {
                    /* 20000 microseconds to the fiftieth, rounded up, so a
                       timeout shorter than one tick still waits a tick rather
                       than expiring before it has begun. */
                    term_wait_pkt   = pkt;
                    term_wait_from  = term_ticks();
                    term_wait_until = term_wait_from
                                    + ((ULONG)pkt->dp_Arg1 / 20000UL) + 1UL;
                }
                continue;

            /*
             * Open("*"), which is a program asking for the console it was
             * started from.
             *
             * dos.library sends the FIND packet to pr_ConsoleTask, and
             * pr_ConsoleTask is this port -- cli_init.c sets it to fh_Type of
             * the Shell's interactive input.  So this is the only way one of
             * these can arrive here.  This handler is mounted as nothing and
             * named as nothing, and there is no other path to the port.  The
             * name in dp_Arg3 is therefore not examined.
             *
             * dp_Arg1 is a FileHandle DOS has already allocated and frees
             * again, and filling it in is the whole of the answer.  fh_Type is
             * already this port.  fh_Port is the interactive flag, and it has
             * to be set for the same reason it is set on the Shell's own
             * handles.  Ed reads IsInteractive() before it decides the stream
             * is a console at all.
             */
            case ACTION_FINDINPUT:
            case ACTION_FINDOUTPUT:
            case ACTION_FINDUPDATE:
            {
                struct FileHandle *fh =
                    (struct FileHandle *)BADDR(pkt->dp_Arg1);

                if (fh == NULL || !term_packet_current(pkt))
                {
                    term_reply(pkt, DOSFALSE, ERROR_OBJECT_NOT_FOUND);
                    continue;
                }

                fh->fh_Type = term_port;
                fh->fh_Port = term_port;
                fh->fh_Arg1 = term_handle_arg(TERM_ID_CON);

                term_reply(pkt, DOSTRUE, 0);
                continue;
            }

            case ACTION_FLUSH:
            case ACTION_SET_FILE_SIZE:
                term_reply(pkt, DOSTRUE, 0);
                continue;

            default:
                term_reply(pkt, DOSFALSE, ERROR_ACTION_NOT_KNOWN);
                continue;
        }

        /*
         * Closing a `*` handle is not the end of the session.  The Shell's own
         * two handles are the session and their ACTION_END is how it ends.  A
         * handle a command opened and gave back is only a handle.
         */
        if (pkt->dp_Type == ACTION_END &&
            (pkt->dp_Arg1 & 0xFF) == TERM_ID_CON)
        {
            term_reply(pkt, DOSTRUE, 0);
            continue;
        }

        p = term_pipe_of(pkt->dp_Arg1, pkt->dp_Type);
        if (p == NULL)
        {
            /*
             * A handle from a session that has been let go of, and the answer
             * is chosen to make its Shell stop rather than only to be an
             * error.  End of file on a read is what a Shell exits on.  A real
             * failure on a write is what a command printing into nothing gives
             * up on.  A close is allowed to succeed, because a Shell that
             * cannot close its handles cannot finish exiting.
             */
            if (pkt->dp_Type == ACTION_READ)
                term_reply(pkt, 0, 0);
            else if (pkt->dp_Type == ACTION_END)
                term_reply(pkt, DOSTRUE, 0);
            else
                term_reply(pkt, -1, ERROR_INVALID_LOCK);
            continue;
        }

        /* Read before term_reply() overwrites dp_Port with this port. */
        if (pkt->dp_Port != NULL)
            term_shell_task = pkt->dp_Port->mp_SigTask;

        switch (pkt->dp_Type)
        {
            case ACTION_READ:
            case ACTION_WRITE:
                /* Nothing asked for is nothing to wait for.  A filesystem
                   answers a zero-length read or write with zero at once, and
                   holding one instead blocks the caller for ever.  term_retry()
                   below has no way to make progress on it, because taking no
                   bytes is how it recognises a full ring.  Dropbear's
                   writechannel_fallback() does exactly this write on every
                   pass where the channel buffer is empty, which is what hung
                   an interactive ssh session the instant the far end spoke. */
                if (pkt->dp_Arg3 <= 0)
                {
                    term_reply(pkt, 0, 0);
                    break;
                }

                /* One held packet per pipe is enough, because the Shell is one
                   process and is inside one Read() at a time.  A second is a
                   protocol error on its side and is refused rather than
                   overwriting the first. */
                if (p->held != NULL)
                {
                    term_reply(pkt, -1, ERROR_OBJECT_IN_USE);
                    break;
                }
                p->held = pkt;
                term_retry(p);
                break;

            case ACTION_END:
                p->dosend = 1;
                if (p->held != NULL)
                {
                    struct DosPacket *held = p->held;

                    p->held = NULL;
                    term_reply(held, -1, ERROR_INVALID_LOCK);
                }
                term_reply(pkt, DOSTRUE, 0);
                break;

            default:                /* unreachable: filtered above          */
                term_reply(pkt, DOSFALSE, ERROR_ACTION_NOT_KNOWN);
                break;
        }
    }

    /* Last, because the packets above can be what makes it answerable, and
       because a timeout that has run out has to be noticed on a pass where
       nothing else happened. */
    term_wait_service();
}

ULONG http_term_sigmask(VOID)
{
    if (term_port == NULL)
        return 0;

    return 1UL << (ULONG)term_port->mp_SigBit;
}

/*
 * A DOS file handle on one end of a pipe.  fh_Arg1 is the pipe's id and
 * fh_Type this process's port, which is all http_term_service() needs to route
 * a packet.  Once the handle has been handed to the runner, DOS frees it when
 * it is closed, so that path must not free it directly.
 */
static BPTR term_handle(TermPipe *p, LONG id, BOOL shell_reads)
{
    struct FileHandle *fh;

    if (term_port == NULL)
        return (BPTR)0;

    fh = (struct FileHandle *)AllocDosObject(DOS_FILEHANDLE, NULL);
    if (fh == NULL)
        return (BPTR)0;

    fh->fh_Type = term_port;
    fh->fh_Arg1 = term_handle_arg(id);

    /*
     * fh_Port is dos.library's interactive flag, and setting it is what makes
     * the Shell on the far end print a prompt.
     *
     * IsInteractive() in V37 and later reads this field.  It does not send
     * ACTION_IS_FILESYSTEM, which is the 1.3 way, and the case for it below is
     * answered but never asked.  Measured: with fh_Port left at zero the Shell
     * started, decided its input was a script, and read four times without
     * ever writing a byte.  Four READs and no WRITE in the packet trace is
     * what a missing prompt looks like from this side.
     *
     * src/bsdsocket/tcp_handler.c sets it on a TCP: handle for the same reason
     * and says the same thing.  A stream with no length and no seek is what
     * interactive means to DOS.
     */
    fh->fh_Port = term_port;

    if (shell_reads)
        p->dosread = 1;
    else
        p->doswrite = 1;

    return MKBADDR(fh);
}

/*
 * Dispose of a handle that has not been handed to the runner.
 *
 * Close() is deliberately wrong here.  fh_Type names term_port, so Close()
 * sends ACTION_END to that port and waits for a reply.  The caller is the
 * httpd process that services term_port, and it cannot service the packet
 * while it is blocked inside Close().  Before CreateNewProc succeeds nobody
 * else owns or has used the handle, so give the AllocDosObject allocation
 * straight back instead.
 */
static VOID term_handle_discard(BPTR handle)
{
    if (handle != (BPTR)0)
        FreeDosObject(DOS_FILEHANDLE, (APTR)BADDR(handle));
}

/* ---------------------------------------------------------- the runner --- */

static VOID term_runner_main(VOID)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    TermRunner     *r;
    struct Task    *parent;

    /* The parent is filling tc_UserData and signals when it is done. */
    (VOID)Wait(SIGF_SINGLE);

    r = (TermRunner *)me->pr_Task.tc_UserData;
    if (r == NULL)
        return;

    /*
     * The stack the Shell gives its commands, and the prompt it prints.
     *
     * Both are set on this process's CLI and inherited, because that is how a
     * Shell gets them.  cli_init.c, building the new CLI:
     *
     *     prompt = (char *) BADDR(oclip->cli_Prompt);
     *     if (defstk == 0) defstk = oclip->cli_DefaultStack;
     *
     * where oclip is the CLI of whoever asked for the Shell, which is this
     * process, and that is what NP_Cli below buys.  There is no packet for
     * either and no tag on Execute().  The caller's CLI is the channel.
     *
     * Sixteen kilobytes, and what it costs:
     *
     *   CLI_INITIAL_STACK is 4096 bytes and that is what a Shell hands a
     *   command by default.  It is not enough for anything that reaches the
     *   network.  A bsdsocket LVO runs NetX Duo on the caller's stack, since
     *   ami_netstack_enter() descends from there, so a command that opens a
     *   socket carries the whole TCP/IP call depth on whatever the Shell gave
     *   it.  Typing `stack` before every command is the alternative.
     *
     *   The field is in longwords, not bytes.  cli_init.h says so:
     *   `#define CLI_INITIAL_STACK (4096 >> 2)`.  Writing 16384 here would ask
     *   for 64 KB.
     *
     *   The cost is per command and not per session.  The Shell allocates this
     *   when it runs one and gives it back afterwards, and it runs one at a
     *   time.  So on the 1 MB machine this project cares about it is 16 KB, or
     *   1.6%, held only while a command is running, against 4 KB before.  A
     *   session sitting at a prompt costs nothing extra.
     *
     *   It is a floor and not a guarantee.  TCP_SESSION_STACK is 16 KB and
     *   BSD_STARTUP_STACK is 64 KB for the same reason, and a command that
     *   needs more can still say `stack`.  It does not size ssh: dbclient
     *   swaps onto a stack of its own at __wrap_main
     *   (clients/compat/amiga_argv.c) and leaves this one behind.
     */
    {
        struct CommandLineInterface *cli = Cli();

        if (cli != NULL)
            cli->cli_DefaultStack = TERM_SHELL_STACK / 4UL;
    }

    /*
     * Execute() and not SystemTagList().  dos.library's own autodoc for
     * SystemTagList says it in a single line -- "Similar to Execute(), but
     * does not read commands from the input filehandle" -- so System() with an
     * empty command line has nothing to do and returns 0 at once.  Measured
     * that way: the upgrade completed, the runner published rc 0 within
     * milliseconds, and the browser saw a session that opened and closed with
     * not one byte in it.
     *
     * Execute()'s contract is the one this needs: "If the input file handle is
     * nonzero then after the (possibly empty) commandString is performed
     * subsequent input is read from the specified input file handle until end
     * of that file is reached."  A live input handle is the documented way to
     * put a Shell on a serial port, and this is the same thing with a handler
     * where the port would be.
     *
     * The string is no longer empty.  It is TERM_SHELL_SETUP, which sets the
     * prompt, and the same sentence is what says that is safe.  The command is
     * performed first and the handle is read afterwards, so the Shell is still
     * the interactive one and every line typed still arrives.  "(possibly
     * empty)" is the parenthesis that makes it a choice.
     *
     * What it costs is the tags.  Execute() creates the Shell process itself,
     * so there is no NP_StackSize to give it and no NP_Name to find it by.
     * The stack the Shell gives its commands is the system default, and
     * `stack 65536` changes it as in any other Shell.  The name is not needed
     * either.  See term_shell_task, which learns the Shell's Task from the
     * packets it sends rather than by guessing what dos.library called it.
     */
    r->rn_Rc = (LONG)Execute((CONST_STRPTR)TERM_SHELL_SETUP,
                             r->rn_In, r->rn_Out);

    if (r->rn_Rc == 0)
        r->rn_Err = IoErr();

    /*
     * Both handles, always, which is what the archived branch did and a
     * rewrite of it did not.
     *
     * The autodoc leaves it open -- Execute() "may also be used to create a
     * new interactive Shell process just like those created with the NewShell
     * command", which would return at once and leave the handles to the
     * Shell -- so the first version of this waited for the Shell to close them
     * and never closed them here.  Measured, on the guest, with the packet
     * trace: Execute() returns only when the Shell has exited, and that Shell
     * does not close the handles it was given.  No ACTION_END ever arrived,
     * the session was never reaped, and every upgrade after the first was
     * answered 503, 196 of them in one run.
     *
     * These two closes are the session's end of file in both directions, and
     * the server's process answers them.  Close() on a handle whose fh_Type is
     * this port sends ACTION_END there and waits, the event loop services it,
     * and this Process wakes.  That is the shape the archived
     * amiga_dropbear.c had and the reason it had it.
     */
    Close(r->rn_In);
    Close(r->rn_Out);

    /* rn_Done is published last and nothing in the record is read after it. */
    parent = r->rn_Parent;
    r->rn_Done = 1;
    Signal(parent, SIGBREAKF_CTRL_E);
}

/* Whether Exec can still schedule this runner.  The running task and the two
   scheduler lists are the complete live set for this port.  Disable(), not
   Forbid(): interrupts move tasks between the lists.  The pointer is compared
   only and never dereferenced after CreateNewProc() may have freed it. */
static BOOL term_task_on_list(struct List *list, struct Task *task)
{
    struct Node *node;

    for (node = list->lh_Head; node->ln_Succ != NULL; node = node->ln_Succ)
    {
        if ((struct Task *)node == task)
            return TRUE;
    }

    return FALSE;
}

static BOOL term_task_alive(struct Task *task)
{
    BOOL alive;

    if (task == NULL)
        return FALSE;

    Disable();
    alive = (SysBase->ThisTask == task ||
             term_task_on_list(&SysBase->TaskReady, task) ||
             term_task_on_list(&SysBase->TaskWait, task));
    Enable();

    return alive;
}

/* Reclaim runners which have made their final publication.  The current
   record stays until it is replaced or shutdown has finished reading its
   result.  rn_Done is last, so a true value means the runner will never touch
   the record again. */
static VOID term_runners_collect(VOID)
{
    TermRunner **link = &term_runners;

    while (*link != NULL)
    {
        TermRunner *r = *link;

        if (r != term_runner && r->rn_Done != 0 &&
            !term_task_alive(r->rn_Task))
        {
            *link = r->rn_Next;
            ami_free(r);
        }
        else
        {
            link = &r->rn_Next;
        }
    }
}

static BOOL term_runners_done(VOID)
{
    TermRunner *r;

    for (r = term_runners; r != NULL; r = r->rn_Next)
    {
        if (r->rn_Done == 0 || term_task_alive(r->rn_Task))
            return FALSE;
    }

    return TRUE;
}

/* ------------------------------------------------------------- the shell --- */

BOOL http_term_init(VOID)
{
    if (term_port != NULL)
        return TRUE;

    term_in.buf  = (UBYTE *)ami_alloc(TERM_IN_BUF);
    term_out.buf = (UBYTE *)ami_alloc(TERM_OUT_BUF);

    if (term_in.buf == NULL || term_out.buf == NULL)
    {
        ami_free(term_in.buf);
        ami_free(term_out.buf);
        term_in.buf  = NULL;
        term_out.buf = NULL;
        tool_error("not enough memory for a terminal");
        return FALSE;
    }

    term_in.size  = TERM_IN_BUF;
    term_out.size = TERM_OUT_BUF;

    term_port = CreateMsgPort();
    if (term_port == NULL)
    {
        ami_free(term_in.buf);
        ami_free(term_out.buf);
        term_in.buf  = NULL;
        term_out.buf = NULL;
        tool_error("cannot make a port for a terminal");
        return FALSE;
    }

    return TRUE;
}

VOID http_term_announce(const char *root, const char *dotted, UWORD port,
                        const char *url)
{
    char  probe[256];
    ULONG n = 0;
    BPTR  lock;

    tool_printf("A Shell, with no password, at http://%s:%ld%s\n",
                (LONG)dotted, (LONG)port, (LONG)url);

    while (root[n] != '\0' && n + 1UL < sizeof(probe))
    {
        probe[n] = root[n];
        n++;
    }

    /* "DH0:" already ends in its separator and "DH0:Work" does not.  The
       caller has taken any trailing slash off already. */
    if (n > 0UL && probe[n - 1] != ':' && n + 1UL < sizeof(probe))
        probe[n++] = '/';

    {
        const char *leaf = url;

        if (*leaf == '/')
            leaf++;

        while (*leaf != '\0' && n + 1UL < sizeof(probe))
            probe[n++] = *leaf++;
    }
    probe[n] = '\0';

    lock = Lock((CONST_STRPTR)probe, ACCESS_READ);
    if (lock != (BPTR)0)
    {
        UnLock(lock);
        tool_printf("  (that address now shadows %s)\n", (LONG)probe);
    }
}

BOOL http_term_available(VOID)
{
    if (term_port == NULL)
        return FALSE;

    /* Abandoned counts as available.  The old Shell is still alive somewhere
       and is no longer this terminal's, because its handles route nowhere.
       See TERM_ABANDON_TICKS. */
    return (!term_active || term_abandoned) ? TRUE : FALSE;
}

BOOL http_term_running(VOID)
{
    if (!term_active)
        return FALSE;

    /* Still running, or finished with output nobody has read yet.  A session
       that ends with the last line of `list` still in the ring must not be
       reported as over, because that line is the answer. */
    if (!term_reaped)
        return TRUE;

    return (ring_used(&term_out) > 0UL) ? TRUE : FALSE;
}

BOOL http_term_start(VOID)
{
    struct TagItem  tags[5];
    struct Process *proc;
    BPTR            sh_in  = (BPTR)0;
    BPTR            sh_out = (BPTR)0;

    if (!http_term_available())
        return FALSE;

    /* Every session gets a generation of its own, not only one that replaces
       an abandoned Shell.  A command started asynchronously can retain a `*`
       handle after an otherwise clean Shell exit.  Reusing that generation
       would let the old handle enter the next session's rings directly. */
    term_gen++;

    /*
     * Cut the last session loose, if it is still there.
     *
     * The generation moved above, so nothing the old Shell sends from here on
     * finds a ring.  Then anything it had held is answered, because a packet
     * held on a pipe that no longer belongs to it leaves a process asleep for
     * ever.  A read gets end of file and a write gets an error, which between
     * them are what makes most commands give up.
     */
    if (term_active)
    {
        if (term_in.held != NULL)
        {
            struct DosPacket *held = term_in.held;

            term_in.held = NULL;
            term_reply(held, 0, 0);             /* end of file              */
        }

        if (term_out.held != NULL)
        {
            struct DosPacket *held = term_out.held;

            term_out.held = NULL;
            term_reply(held, -1, ERROR_INVALID_LOCK);
        }

        if (term_wait_pkt != NULL)
        {
            struct DosPacket *held = term_wait_pkt;

            term_wait_pkt = NULL;
            term_reply(held, DOSTRUE, 0);       /* a Read() will return now */
        }

    }

    term_in.count   = 0;
    term_in.rd      = 0;
    term_in.wr      = 0;
    term_in.held    = NULL;
    term_in.dosend  = 0;
    term_in.closed  = 0;

    term_out.count  = 0;
    term_out.rd     = 0;
    term_out.wr     = 0;
    term_out.held   = NULL;
    term_out.dosend = 0;
    term_out.closed = 0;

    sh_in  = term_handle(&term_in,  TERM_ID_IN,  TRUE);
    sh_out = term_handle(&term_out, TERM_ID_OUT, FALSE);

    if (sh_in == (BPTR)0 || sh_out == (BPTR)0)
    {
        term_handle_discard(sh_in);
        term_handle_discard(sh_out);
        return FALSE;
    }

    /* The previous record is no longer current, but it remains on the runner
       list until rn_Done says its process has returned.  In particular an
       abandoned runner must remain visible to shutdown, because it still
       executes code from this command's load segment. */
    term_runner = NULL;
    term_runners_collect();

    term_runner = (TermRunner *)ami_alloc(sizeof(*term_runner));
    if (term_runner == NULL)
    {
        term_handle_discard(sh_in);
        term_handle_discard(sh_out);
        tool_error("not enough memory to start a Shell");
        return FALSE;
    }

    term_runner->rn_Parent = FindTask(NULL);
    term_runner->rn_Next   = NULL;
    term_runner->rn_Task   = NULL;
    term_runner->rn_In     = sh_in;
    term_runner->rn_Out    = sh_out;
    term_runner->rn_Rc     = 0;
    term_runner->rn_Err    = 0;
    term_runner->rn_Done   = 0;

    /*
     * NP_Cli is required.  Execute() asks the calling process for its CLI,
     * which is where the Shell it creates inherits its command paths and its
     * idea of what a command is, and CreateNewProc() makes a process with
     * pr_CLI NULL unless told otherwise.  Without it, Execute() returned FALSE
     * with nothing else wrong, which reads from outside as a session that
     * opens and closes.
     *
     * SystemTagList() did not need it because it builds the CLI itself, which
     * is why the first version of this file appeared to get further than it
     * did.
     */
    tags[0].ti_Tag = NP_Entry;     tags[0].ti_Data = (ULONG)term_runner_main;
    tags[1].ti_Tag = NP_Name;      tags[1].ti_Data = (ULONG)"httpd terminal runner";
    tags[2].ti_Tag = NP_StackSize; tags[2].ti_Data = TERM_RUNNER_STACK;
    tags[3].ti_Tag = NP_Cli;       tags[3].ti_Data = TRUE;
    tags[4].ti_Tag = TAG_END;      tags[4].ti_Data = 0;

    proc = CreateNewProc(tags);
    if (proc == NULL)
    {
        ami_free(term_runner);
        term_runner = NULL;
        term_handle_discard(sh_in);
        term_handle_discard(sh_out);
        return FALSE;
    }

    term_runner->rn_Task = (struct Task *)proc;
    term_runner->rn_Next = term_runners;
    term_runners         = term_runner;

    /* The runner is in Wait(SIGF_SINGLE) until this pair happens. */
    proc->pr_Task.tc_UserData = (APTR)term_runner;
    Signal((struct Task *)proc, SIGF_SINGLE);

    term_active      = 1;
    term_reaped      = 0;
    term_stopping    = 0;
    term_abandoned   = 0;
    term_stop_passes = 0;
    term_traced      = 0;
    term_said_rc     = 0;
    term_rc          = -1;
    term_err         = 0;

    /*
     * A session begins COOKED, which is what a Shell wants, and the page is
     * told so rather than left to assume it.  A page that reconnects into a
     * session it did not open has no other way to know, and a wrong assumption
     * leaks a password.
     */
    term_raw          = 0;
    term_mode_pending = 1;
    term_in_urgent    = 0;
    term_shell_task   = NULL;
    term_break_port   = NULL;
    term_cols         = 80;
    term_rows         = 25;
    term_seq_n        = 0;
    term_seq_esc      = 0;
    term_want_resize  = 0;

    /* Not term_wait_pkt.  It is already NULL, because ending a session closes
       the input and that makes every held WaitForChar() answerable, and
       clearing the pointer would strand the process waiting on one.  Left
       alone, a stray one is answered on its own timeout, which is the right
       answer to a question about a session that has gone. */

    return TRUE;
}

BOOL http_term_raw(VOID)
{
    return term_raw ? TRUE : FALSE;
}

/*
 * The mode, for the page, when the page has not been told it yet.
 *
 * A peek and not a take.  The caller has to be able to ask whether there is
 * anything to send before it has a buffer to put it in, the same reason
 * http_term_pending() exists.  http_term_mode_sent() is the take.
 */
const char *http_term_mode_word(VOID)
{
    if (!term_active || !term_mode_pending)
        return NULL;

    return term_raw ? "mode raw" : "mode cooked";
}

VOID http_term_mode_sent(VOID)
{
    term_mode_pending = 0;
}

/*
 * The counters, as a word, when somebody has asked for them.
 *
 * Same peek-then-take shape as the mode word and for the same reason.  The
 * caller decides whether to want the socket writable before it has anywhere to
 * put the frame.
 */
static char term_st_buf[96];

static ULONG term_st_put(ULONG at, const char *label, ULONG v)
{
    while (*label != '\0')
        term_st_buf[at++] = *label++;

    at += term_num((UBYTE *)&term_st_buf[at], v);
    return at;
}

const char *http_term_stats_word(VOID)
{
    ULONG at = 0;

    if (!term_st_pending)
        return NULL;

    at = term_st_put(at, "stats writes=", term_st_writes);
    at = term_st_put(at, " wbytes=",      term_st_wbytes);
    at = term_st_put(at, " frames=",      term_st_frames);
    at = term_st_put(at, " fbytes=",      term_st_fbytes);
    term_st_buf[at] = '\0';

    return term_st_buf;
}

VOID http_term_stats_sent(VOID)
{
    term_st_pending = 0;
}

/*
 * The page's window, as the page measures it.  Kept rather than acted on.
 * Nothing on this side has a window, and the only thing that reads these is a
 * program that asks.  See term_write_scan().
 */
VOID http_term_resize(UWORD cols, UWORD rows)
{
    /* A terminal component still laying out reports zero, and a program told
       it has no columns divides by it.  Dropped rather than stored. */
    if (cols == 0 || rows == 0)
        return;

    if (cols == term_cols && rows == term_rows)
        return;

    term_cols = cols;
    term_rows = rows;

    /*
     * And tell the program, if it asked to be told.  A console sends a resize
     * as an input event only to a program that turned that event class on with
     * `CSI 12 {`, and Ed is the program that does.  Sending it unasked would
     * put bytes in front of a Shell that would read them as a command.
     */
    if (term_active && term_want_resize)
        term_resize_event();
}

LONG http_term_write(const UBYTE *data, LONG len)
{
    LONG n;

    if (!term_active || len <= 0 || term_in.closed)
        return 0;

    /* The Shell has closed its stdin, so it is on its way out and nothing more
       is read.  Report the bytes taken rather than stalling, because a session
       with data pending for ever never closes. */
    if (term_in.dosend)
        return len;

    n = (LONG)ring_put(&term_in, data, (ULONG)len);

    if (n > 0)
        term_kick();                /* a Shell blocked in Read(), or in
                                       WaitForChar(), can go                */

    return n;
}

ULONG http_term_pending(VOID)
{
    return term_active ? ring_used(&term_out) : 0UL;
}

LONG http_term_read(UBYTE *buf, LONG len)
{
    LONG n;

    if (!term_active || len <= 0)
        return 0;

    n = (LONG)ring_get(&term_out, buf, (ULONG)len);

    if (n > 0)
        term_retry(&term_out);      /* room appeared for a blocked Write()  */

    return n;
}

VOID http_term_eof(VOID)
{
    if (!term_active)
        return;

    term_in.closed = 1;

    /* A Shell asleep on this pipe has to be woken, or it waits for a reply
       that is never coming and the runner never returns.  So does one asleep
       in WaitForChar(), because end of file is what it reports on too. */
    term_kick();
}

VOID http_term_break(VOID)
{
    struct Task *target = term_shell_task;

    if (!term_active)
        return;

    /* What dos.library named, if it named one.  ACTION_CHANGE_SIGNAL is the
       answer where the packet sender is the inference. */
    if (term_break_port != NULL && term_break_port->mp_SigTask != NULL)
        target = term_break_port->mp_SigTask;

    if (target == NULL)
        return;

    Signal(target, SIGBREAKF_CTRL_C);
}

LONG http_term_rc(VOID)
{
    return term_rc;
}

LONG http_term_err(VOID)
{
    return term_err;
}

/*
 * End the session.
 *
 * Execute()'s contract names the ending as well as the beginning.  With a
 * non-zero input handle, "subsequent input is read from the specified input
 * file handle until end of that file is reached".  So closing this side of the
 * input pipe is the documented way for the Shell to finish, and a pipe can
 * report end of file where the console the same autodoc talks about never can.
 *
 * Ctrl-C first, because end of file is only seen by a Shell that is asking for
 * a line, and a Shell inside a command is not.  The task to signal is whoever
 * last sent a packet, which is the process a person pressing Ctrl-C means.
 *
 * `endcli` was tried here and is worse.  It is a command, so it needs C:EndCLI
 * to exist on the machine, and this must not depend on what is installed.
 */
VOID http_term_stop(VOID)
{
    if (!term_active || term_stopping)
        return;

    term_stopping    = 1;
    term_stop_passes = 0;
    term_stop_at     = term_ticks();

    if (term_shell_task != NULL)
        Signal(term_shell_task, SIGBREAKF_CTRL_C);

    term_in.closed  = 1;
    term_out.closed = 1;

    /*
     * Whatever the Shell had left to say has nobody to say it to.  Discarding
     * it rather than holding it also unblocks a Shell asleep in Write(), which
     * is a Shell that cannot get as far as reading its end of file.
     */
    term_out.count = 0;
    term_out.rd    = 0;
    term_out.wr    = 0;

    term_kick();
    term_retry(&term_out);

    http_term_service();
}

VOID http_term_shutdown(VOID)
{
    LONG waited = 0;
    BOOL warned = FALSE;

    if (term_port == NULL)
        return;

    if (term_active)
    {
        http_term_stop();



        /*
         * The runner still holds two FileHandles that name this process's
         * port, and the Shell can still be inside a command.  Keep answering
         * packets until it has gone, and keep draining the output ring,
         * because a Shell blocked in Write() cannot notice that its input has
         * ended.
         */
        while (!term_reaped)
        {
            UBYTE scratch[256];

            http_term_service();

            while (http_term_read(scratch, (LONG)sizeof(scratch)) > 0)
                ;

            Delay(1);
            waited++;

            if (!warned && waited >= TERM_STOP_WARN_TICKS)
            {
                warned = TRUE;
                tool_error("the terminal's Shell is still running; waiting "
                           "because its runner still uses httpd's code");
            }
        }

        term_active = 0;
    }

    /* This includes runners from sessions abandoned earlier.  Their handles
       are generation-isolated, but their Process still returns through
       term_runner_main().  Returning from httpd while any one remains would
       let DOS unload the code beneath it.  There is no safe bounded version
       of this wait; keep servicing the shared port so stale handles can close
       and make their runners finish. */
    while (!term_runners_done())
    {
        http_term_service();
        Delay(1);
        waited++;

        if (!warned && waited >= TERM_STOP_WARN_TICKS)
        {
            warned = TRUE;
            tool_error("a terminal runner is still active; waiting because "
                       "httpd cannot unload live runner code");
        }
    }

    while (term_runners != NULL)
    {
        TermRunner *next = term_runners->rn_Next;

        ami_free(term_runners);
        term_runners = next;
    }
    term_runner = NULL;

    DeleteMsgPort(term_port);
    term_port = NULL;

    ami_free(term_in.buf);
    ami_free(term_out.buf);
    term_in.buf  = NULL;
    term_out.buf = NULL;
}

/* ------------------------------------------------- the socket, once it is --
 *                                                     no longer HTTP
 *
 * See httpterm.h for why this is here and not in httpd.c.  Everything in this
 * section is about one upgraded socket.  The Shell above it is the module's
 * own and is shared, because there is one of it.
 */

/*
 * Queue one control frame.  There is room for exactly one.  A pong and a close
 * never both need to be in flight, and a client that pings faster than the LAN
 * drains is answered on the ping it sent last, which is the only one it is
 * still waiting for.
 */
static VOID sock_control(HttpTermSock *t, HttpWsEvent ev,
                         const UBYTE *payload, ULONG len)
{
    unsigned long head;
    ULONG         i;

    if (len > (unsigned long)HTTP_WS_CTL_MAX)
        len = (unsigned long)HTTP_WS_CTL_MAX;

    head = http_ws_head(t->ctl, sizeof(t->ctl), ev, len, 1);
    if (head == 0UL)
        return;

    for (i = 0; i < len; i++)
        t->ctl[head + i] = payload[i];

    t->ctl_n  = (UWORD)(head + len);
    t->ctl_at = 0;
}

static VOID sock_close(HttpTermSock *t, UWORD code)
{
    if (t->closing)
        return;

    t->ctl_n   = (UWORD)http_ws_close_frame(t->ctl, sizeof(t->ctl), code,
                                            http_ws_close_reason(code));
    t->ctl_at  = 0;
    t->closing = 1;
    t->why     = code;
}

/* Whether `s` begins with `word` followed by a space or the end. */
static const char *sock_after(const char *s, const char *word)
{
    while (*word != '\0')
    {
        if (*s != *word)
            return NULL;
        s++;
        word++;
    }

    if (*s == '\0')
        return s;
    if (*s != ' ')
        return NULL;

    while (*s == ' ')
        s++;

    return s;
}

/* One unsigned number, and where it ended.  No sign and no overflow, because
   the caller is a window dimension and five digits is already out of range. */
static const char *sock_number(const char *s, UWORD *out)
{
    ULONG v = 0;
    int   any = 0;

    while (*s >= '0' && *s <= '9')
    {
        if (v < 100000UL)
            v = v * 10UL + (ULONG)(*s - '0');
        s++;
        any = 1;
    }

    if (!any || v > 9999UL)
        return NULL;

    *out = (UWORD)v;
    return s;
}

/*
 * One text frame from the page.  See httpterm.h for the vocabulary.
 *
 * Anything unrecognised is ignored and not an error, which is what makes the
 * channel extensible in both directions.  The word a newer page sends is a
 * word an older server can afford not to know.
 */
static VOID sock_word(const char *w)
{
    const char *rest;

    if (sock_after(w, "break") != NULL)
    {
        http_term_break();
        return;
    }

    if (sock_after(w, "eof") != NULL)
    {
        http_term_eof();
        return;
    }

    /*
     * `stats` and `stats reset`.  Instrumentation, and it stays because the
     * question it answers -- how many ACTION_WRITEs became how many frames --
     * cannot be answered from the far end of the socket, and is the first
     * question to ask when this is slow.
     */
    if (sock_after(w, "stats") != NULL)
    {
        if (sock_after(w, "stats reset") != NULL)
        {
            term_st_writes = 0;
            term_st_wbytes = 0;
            term_st_frames = 0;
            term_st_fbytes = 0;
        }
        term_st_pending = 1;
        return;
    }

    rest = sock_after(w, "size");
    if (rest != NULL)
    {
        UWORD cols = 0;
        UWORD rows = 0;

        rest = sock_number(rest, &cols);
        if (rest == NULL)
            return;

        while (*rest == ' ')
            rest++;

        rest = sock_number(rest, &rows);
        if (rest == NULL)
            return;

        http_term_resize(cols, rows);
    }
}

/*
 * What arrived.  Keystrokes are held rather than written straight to the
 * Shell, because the Shell can be not reading and a decoder's sink has no way
 * to refuse bytes.  The socket is not read again until the hold is empty,
 * which turns a busy Shell into TCP back pressure instead of lost input.
 */
static VOID sock_sink(void *ctx, HttpWsEvent ev, const UBYTE *data,
                      long len, int final)
{
    HttpTermSock *t = (HttpTermSock *)ctx;
    long          i;

    switch (ev)
    {
        case HTTP_WS_EV_BINARY:
            for (i = 0; i < len; i++)
            {
                if (t->pend_n < (UWORD)sizeof(t->pend))
                    t->pend[t->pend_n++] = data[i];
            }
            break;

        /*
         * Text is the other channel.  It carries a word, and what the word
         * asks for is not a keystroke.
         *
         * Ctrl-C on an Amiga is a signal and not a byte -- a console handler
         * turns the key into SIGBREAKF_CTRL_C and the command polls for it --
         * so there is no in-band way to send one down a pipe.  Stealing 0x03
         * out of the input stream would have worked and would have made that
         * byte untypeable for ever.  A second opcode costs nothing and takes
         * nothing away.  The page sends keystrokes as binary and never as
         * text, so the two cannot be confused.
         *
         * See httpterm.h for the whole vocabulary, both directions.  It grows
         * by adding words.  A client that only knows `break` and `eof` still
         * works, and a server that does not know `size` ignores it.
         */
        case HTTP_WS_EV_TEXT:
            for (i = 0; i < len; i++)
            {
                if (t->word_n < (UBYTE)sizeof(t->word) - 1)
                    t->word[t->word_n++] = (char)data[i];
            }

            if (final)
            {
                t->word[t->word_n] = '\0';
                sock_word(t->word);
                t->word_n = 0;
            }
            break;

        case HTTP_WS_EV_PING:
            /* RFC 6455 5.5.3: a pong carries the ping's payload back.  A
               close already queued is more important, though.  Reads are
               serviced before writes, so a ping can arrive while that close
               is waiting for a full socket to drain.  Replacing it with a
               pong would make the writer see `closing` after the pong and
               drop the socket without ever sending the close frame. */
            if (!t->closing)
                sock_control(t, HTTP_WS_EV_PONG, data, (ULONG)len);
            break;

        case HTTP_WS_EV_PONG:
            t->pinged = 0;
            break;

        case HTTP_WS_EV_CLOSE:
            /* RFC 6455 5.5.1: answer with a close of this side's own and then
               stop.  The code is echoed, which is what a client that sent 1000
               expects to see before it lets go of the socket. */
            if (!t->closing)
            {
                UWORD code = HTTP_WS_CLOSE_NORMAL;

                if (len >= 2)
                    code = (UWORD)(((UWORD)data[0] << 8) | (UWORD)data[1]);

                sock_close(t, code);
            }
            break;

        default:
            break;
    }
}

/* Move what was typed into the Shell.  Short writes are normal, because the
   Shell's ring is small and a paste is bigger than one command line. */
static VOID sock_feed_shell(HttpTermSock *t)
{
    while (t->pend_at < t->pend_n)
    {
        LONG took = http_term_write(&t->pend[t->pend_at],
                                    (LONG)(t->pend_n - t->pend_at));

        if (took <= 0)
            return;

        t->pend_at = (UWORD)(t->pend_at + took);
    }

    t->pend_n  = 0;
    t->pend_at = 0;
}

/*
 * Decode bytes that arrived behind the HTTP upgrade without outrunning the
 * Shell.  That handoff can be almost the whole 2 KB request buffer, unlike a
 * normal socket read, which is capped at the 512 bytes pend[] can hold.  The
 * decoder's sink cannot refuse a byte, so each feed is capped at the pending
 * buffer and another one is not made until the last one has drained.
 */
static VOID sock_feed_first(HttpTermSock *t)
{
    sock_feed_shell(t);

    while (t->pend_at >= t->pend_n && t->first_at < t->first_len &&
           !t->closing)
    {
        ULONG left = t->first_len - t->first_at;
        LONG  take = (left > (ULONG)HTTP_TERM_READ)
                         ? (LONG)HTTP_TERM_READ : (LONG)left;
        LONG  used;

        used = http_ws_feed(&t->in, &t->first[t->first_at], take,
                            sock_sink, t);
        if (used > 0)
            t->first_at += (ULONG)used;

        if (t->in.failed != 0)
        {
            sock_close(t, (UWORD)t->in.failed);
            t->pend_n   = 0;
            t->pend_at  = 0;
            t->first_at = t->first_len;
            break;
        }

        sock_feed_shell(t);

        /* A live decoder consumes input.  Fail closed rather than spinning
           the server if that contract changes underneath this pump. */
        if (used <= 0)
        {
            sock_close(t, HTTP_WS_CLOSE_PROTOCOL);
            t->first_at = t->first_len;
            break;
        }
    }

    if (t->first_at >= t->first_len || t->closing)
    {
        t->first     = NULL;
        t->first_len = 0;
        t->first_at  = 0;
    }
}

VOID http_term_sock_begin(HttpTermSock *t, struct Library *sb, LONG sock,
                          UBYTE *out, ULONG out_size,
                          const UBYTE *first, ULONG first_len, ULONG now)
{
    t->sb       = sb;
    t->sock     = sock;
    t->out      = out;
    t->out_size = out_size;
    t->out_len  = 0;
    t->out_sent = 0;
    t->pend_n   = 0;
    t->pend_at  = 0;
    t->first    = first;
    t->first_len = first_len;
    t->first_at = 0;
    t->ctl_n    = 0;
    t->ctl_at   = 0;
    t->word_n   = 0;
    t->pinged   = 0;
    t->closing  = 0;
    t->why      = 0;
    t->progress = now;

    http_ws_reset(&t->in);

    sock_feed_first(t);
}

BOOL http_term_sock_wants_read(const HttpTermSock *t)
{
    if (t->closing)
        return FALSE;

    /* The decoder's sink cannot refuse payload.  Do not ask the kernel for
       another byte until everything already decoded, or borrowed from the
       HTTP upgrade, has reached the Shell.  Otherwise queued socket data
       keeps WaitSelect() returning while http_term_sock_read() deliberately
       takes none of it. */
    return (t->pend_at >= t->pend_n && t->first_at >= t->first_len)
               ? TRUE : FALSE;
}

BOOL http_term_sock_wants_write(const HttpTermSock *t)
{
    if (t->out_sent < t->out_len || t->ctl_at < t->ctl_n || t->closing)
        return TRUE;

    if (http_term_mode_word() != NULL || http_term_stats_word() != NULL)
        return TRUE;

    if (http_term_pending() > 0UL)
        return TRUE;

    /* A writable wake is also the local pump for bytes retained from the
       upgrade.  Ask only when it can make progress, or a full input ring
       would turn this into a spin while the Shell is not reading. */
    if ((t->pend_at < t->pend_n && ring_free(&term_in) > 0UL) ||
        (t->pend_at >= t->pend_n && t->first_at < t->first_len))
        return TRUE;

    /* The Shell has gone and the close has not been sent yet. */
    return http_term_running() ? FALSE : TRUE;
}

BOOL http_term_sock_read(HttpTermSock *t, ULONG now)
{
    UBYTE scratch[HTTP_TERM_READ];
    LONG  got;

    sock_feed_first(t);

    /* Not read at all while the Shell has not taken what came last.  This is
       the whole of the flow control in this direction, and the alternative is
       a buffer that grows with whatever a browser pastes. */
    if (t->pend_at < t->pend_n || t->first_at < t->first_len)
        return TRUE;

    got = tool_sock_recv(t->sb, t->sock, scratch, (LONG)sizeof(scratch));

    if (got == 0)
        return FALSE;               /* the browser hung up                 */

    if (got < 0)
    {
        LONG err = tool_sock_errno(t->sb);

        return (err == TOOL_EWOULDBLOCK || err == TOOL_EINTR) ? TRUE : FALSE;
    }

    t->progress = now;
    t->pinged   = 0;                /* anything at all is a live peer      */

    (VOID)http_ws_feed(&t->in, scratch, got, sock_sink, t);

    if (t->in.failed != 0)
    {
        /* The framing is lost and cannot be resynchronised.  Say why in a
           close frame and stop reading, because anything after it is not a
           frame. */
        sock_close(t, (UWORD)t->in.failed);
        t->pend_n  = 0;
        t->pend_at = 0;
        return TRUE;
    }

    sock_feed_shell(t);

    return TRUE;
}

BOOL http_term_sock_write(HttpTermSock *t, ULONG now)
{
    /* The event loop reaches this after a terminal-port wake as well as a
       socket wake.  That is when a Shell read may have made room for retained
       upgrade bytes, even if the peer has sent nothing new. */
    sock_feed_first(t);

    for (;;)
    {
        /* Whatever is already framed goes first. */
        if (t->out_sent < t->out_len)
        {
            LONG sent = tool_sock_send(t->sb, t->sock, &t->out[t->out_sent],
                                       (LONG)(t->out_len - t->out_sent));

            if (sent < 0)
            {
                LONG err = tool_sock_errno(t->sb);

                return (err == TOOL_EWOULDBLOCK || err == TOOL_EINTR)
                           ? TRUE : FALSE;
            }

            t->out_sent += (ULONG)sent;
            t->progress  = now;

            if (t->out_sent < t->out_len)
                return TRUE;        /* the socket is full for now          */
        }

        t->out_len  = 0;
        t->out_sent = 0;

        /* A pong or a close does not wait behind the Shell's output. */
        if (t->ctl_at < t->ctl_n)
        {
            ULONG i;

            for (i = 0; i + t->ctl_at < t->ctl_n; i++)
                t->out[i] = t->ctl[t->ctl_at + i];

            t->out_len  = i;
            t->out_sent = 0;
            t->ctl_at   = t->ctl_n;
            continue;
        }

        /* The close has gone out.  RFC 6455 7.1.1 lets the server be the one
           that shuts the socket once it has both sent and received one, and
           this server is always the one that sent last. */
        if (t->closing)
            return FALSE;

        /*
         * The mode, ahead of the Shell's output and not behind it.
         *
         * RAW goes on because a program is about to ask for a secret, and the
         * page has to stop echoing before the person can answer, so the word
         * overtakes whatever prompt is still in the ring rather than queueing
         * behind it.  The cost is that the prompt can arrive a frame after the
         * page went quiet, which nobody can see.  The cost of the other order
         * is the password.
         */
        {
            const char *word = http_term_mode_word();
            BOOL        ismode = TRUE;

            if (word == NULL)
            {
                word   = http_term_stats_word();
                ismode = FALSE;
            }

            if (word != NULL)
            {
                UBYTE         head[10];
                unsigned long n = 0;
                unsigned long hn;
                unsigned long i;

                while (word[n] != '\0')
                {
                    t->out[10 + n] = (UBYTE)word[n];
                    n++;
                }

                hn = http_ws_head(head, sizeof(head), HTTP_WS_EV_TEXT, n, 1);

                for (i = 0; i < hn; i++)
                    t->out[10 - hn + i] = head[i];

                t->out_sent = 10UL - hn;
                t->out_len  = 10UL + n;

                if (ismode)
                    http_term_mode_sent();
                else
                    http_term_stats_sent();
                continue;
            }
        }

        /*
         * The Shell's output, framed in place.  Read into the buffer past the
         * longest header there is, then write the header backwards from there,
         * so the frame is contiguous and nothing is copied twice.  The send
         * cursor starts wherever the header turned out to begin.
         */
        {
            LONG n = http_term_read(&t->out[10], (LONG)(t->out_size - 10UL));

            if (n > 0)
            {
                UBYTE         head[10];
                unsigned long hn = http_ws_head(head, sizeof(head),
                                                HTTP_WS_EV_BINARY,
                                                (unsigned long)n, 1);
                unsigned long i;

                term_st_frames++;
                term_st_fbytes += (ULONG)n;

                for (i = 0; i < hn; i++)
                    t->out[10 - hn + i] = head[i];

                t->out_sent = 10UL - hn;
                t->out_len  = 10UL + (ULONG)n;
                continue;
            }
        }

        /*
         * Nothing left to send.  If the Shell has gone too, so has the
         * session.  The close is sent here rather than when it exited, so the
         * last line it printed is on the wire ahead of it.
         */
        if (!http_term_running())
        {
            sock_close(t, HTTP_WS_CLOSE_NORMAL);
            continue;
        }

        return TRUE;
    }
}

BOOL http_term_sock_stale(const HttpTermSock *t, ULONG now, ULONG timeout)
{
    /* A ping has been out unanswered for its half of the budget.  Anything at
       all from the peer clears t->pinged, so this reports a peer that was
       asked and did not answer, not a peer that is quiet.  See
       http_ws_live_stale(). */
    return (BOOL)(http_ws_live_stale(t->progress, (int)t->pinged, now, timeout)
                      ? TRUE : FALSE);
}

BOOL http_term_sock_idle(HttpTermSock *t, ULONG now, ULONG timeout)
{
    if (http_term_sock_stale(t, now, timeout))
        return FALSE;

    if (http_ws_live_ping_due(t->progress, (int)t->pinged, now, timeout) &&
        t->ctl_at >= t->ctl_n)
    {
        sock_control(t, HTTP_WS_EV_PING, (const UBYTE *)"", 0);
        t->pinged   = 1;
        t->progress = now;
    }

    return TRUE;
}

VOID http_term_sock_evict(HttpTermSock *t, UWORD code)
{
    UBYTE         frame[HTTP_TERM_CTL];
    unsigned long n;
    unsigned long at = 0;

    if (t->closing)
        return;

    t->closing = 1;
    t->why     = code;

    /* A close can only begin at a frame boundary.  Takeover closes the socket
       on the caller's next line, so there is no later pass to finish whatever
       Shell frame is already partly out.  Push its remainder without waiting;
       if the socket will not take all of it, omit the close rather than splice
       a control-frame header into the middle of a data payload. */
    while (t->out_sent < t->out_len)
    {
        LONG sent = tool_sock_send(t->sb, t->sock, &t->out[t->out_sent],
                                   (LONG)(t->out_len - t->out_sent));

        if (sent <= 0)
            return;

        t->out_sent += (ULONG)sent;
    }

    n = http_ws_close_frame(frame, sizeof(frame), code,
                            "the terminal was taken over from another browser");

    /* Best effort, still without waiting.  Positive short writes can make
       progress immediately, while zero or an error means the FIN is the only
       truthful ending this peer can be given now. */
    while (at < n)
    {
        LONG sent = tool_sock_send(t->sb, t->sock, &frame[at], (LONG)(n - at));

        if (sent <= 0)
            break;

        at += (unsigned long)sent;
    }
}
