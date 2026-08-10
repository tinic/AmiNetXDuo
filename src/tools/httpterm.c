/*
 * httpterm, an AmigaDOS Shell on the other end of a pipe.  See httpterm.h for
 * what this is, why it needs a second Process, and why anyone who can reach
 * the port gets a shell.
 *
 * SPDX-License-Identifier: MIT
 */

#include "httpterm.h"

#include <dos/dostags.h>
#include <dos/dosasl.h>

/*
 * The rings.
 *
 * The Shell's output is the bigger of the two because a command's answer
 * arrives faster than a LAN takes it away -- `list` of a full drawer is
 * kilobytes in one go -- and a ring smaller than one Write() turns it into
 * several packet round trips.  Typing is the other way round: a person
 * produces bytes far slower than anything drains them, and a whole pasted
 * line is still under a kilobyte.
 *
 * Neither is a limit on what can pass, only on what may be in flight.  A full
 * ring parks the Shell's ACTION_WRITE rather than dropping it, which is the
 * back pressure that stops a runaway command from costing memory: `type
 * S:Startup-Sequence` on a link that is not draining stops the command, not
 * the server.
 */
#define TERM_OUT_BUF    4096UL
#define TERM_IN_BUF     1024UL

/*
 * 64 KB for the runner, and see httpterm.h for why that is a floor and not a
 * measurement of what it does today.
 *
 * There is no number here for the SHELL, and that is Execute()'s doing: it
 * creates the Shell process itself and takes no tags, so the stack its
 * commands get is the system default and `stack 65536` typed into the session
 * is what changes it -- the same as in any other Shell on the machine.
 */
#define TERM_RUNNER_STACK   (64UL * 1024UL)

/* How long http_term_shutdown() will wait for a Shell to notice end of file.
   Ten seconds is far longer than the exit path takes and short enough that a
   Ctrl-C on the server does not look like a hang.  See where it is used for
   what happens when it runs out, which is deliberately not "free it anyway". */
#define TERM_STOP_TICKS     500     /* of 1/50 s                            */

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
 *
 * Static rather than allocated: nothing is ever freed while the runner might
 * still be reading it, and a record that cannot be freed is one that should
 * not have been taken from the heap.  The archived branch allocated one per
 * command and missing the free cost 576 bytes a command.
 */
typedef struct
{
    struct Task  *rn_Parent;
    BPTR          rn_In;            /* the Shell's stdin                    */
    BPTR          rn_Out;           /* the Shell's stdout                   */
    LONG          rn_Rc;
    LONG          rn_Err;           /* IoErr(), when no shell would start   */
    volatile LONG rn_Done;
} TermRunner;

static TermRunner term_runner;
static UBYTE      term_active;      /* a Shell has been started             */
static UBYTE      term_reaped;      /* and its exit code has been collected */
static LONG       term_rc = -1;

/*
 * Whoever is reading or writing our pipes, so http_term_break() has something
 * to signal.  Learned from the packets rather than from a name: a DosPacket
 * arrives with dp_Port naming the SENDER's reply port, and a Process's port
 * has that Process in mp_SigTask, so this is exact where FindTask() on a name
 * dos.library chose would be a guess.
 *
 * It is also the RIGHT target rather than merely a findable one.  A Shell runs
 * each command in its own process, so the task that is inside Read() at any
 * moment is the one a person pressing Ctrl-C means.
 */
static struct Task *term_shell_task;

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

static TermPipe *term_pipe_of(LONG id)
{
    if (id == TERM_ID_IN)  return &term_in;
    if (id == TERM_ID_OUT) return &term_out;
    return NULL;
}

/*
 * Reply a packet.  Not ReplyPkt(): dp_Port has to name the port the packet
 * comes back to us on, which is ours, and ReplyPkt() stamps it with the
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
        if (ring_used(p) > 0UL)
        {
            n = (LONG)ring_get(p, (UBYTE *)pkt->dp_Arg2, (ULONG)pkt->dp_Arg3);
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
        /* Closed FIRST, and not "closed once the ring is full".  This side
           gives up on a session by closing both pipes, and a Shell that then
           went on writing successfully into 4 KB nobody will read takes that
           much longer to notice it should stop. */
        if (p->closed)
        {
            /* Nobody will ever read it.  -1 with a real error, so a command
               writing into a channel that has gone fails rather than looping
               on a short write for ever. */
            p->held = NULL;
            term_reply(pkt, -1, ERROR_INVALID_LOCK);
            return;
        }
        else if (ring_free(p) > 0UL)
        {
            n = (LONG)ring_put(p, (const UBYTE *)pkt->dp_Arg2,
                               (ULONG)pkt->dp_Arg3);
        }
        else
        {
            return;
        }
    }

    p->held = NULL;
    term_reply(pkt, n, 0);
}

VOID http_term_service(VOID)
{
    struct Message *msg;

    if (term_port == NULL)
        return;

    /*
     * Notice a Shell that has gone, before the packets are looked at.
     *
     * WHAT "GONE" IS, AND WHY IT IS NOT THE RUNNER RETURNING
     *
     *   Execute() may create the Shell and return at once -- the autodoc says
     *   it makes "a new interactive Shell process just like those created with
     *   the NewShell command" -- so the runner publishing rn_Done says nothing
     *   about whether the Shell is still there.  What does say it is the Shell
     *   CLOSING the two handles, which arrives here as ACTION_END on each, and
     *   that answer is the same whichever way Execute() behaved.
     *
     *   The one case rn_Done decides is failure: Execute() returns a BOOLEAN,
     *   and a false one means no Shell was started at all.  Then nothing will
     *   ever send an ACTION_END and the session has to end on the flag.
     */
    if (term_active && !term_reaped)
    {
        BOOL failed = (term_runner.rn_Done != 0 && term_runner.rn_Rc == 0)
                          ? TRUE : FALSE;
        BOOL ended  = (term_in.dosend && term_out.dosend) ? TRUE : FALSE;

        if (failed)
        {
            tool_error("no Shell would start for the terminal (IoErr %ld)",
                       (LONG)term_runner.rn_Err);
            term_rc     = -1;
            term_reaped = 1;
        }
        else if (ended)
        {
            term_rc     = 0;
            term_reaped = 1;
        }

        /* Not `term_active = 0` yet: output the Shell wrote before it exited
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

        switch (pkt->dp_Type)
        {
            case ACTION_READ:
            case ACTION_WRITE:
            case ACTION_END:
                break;

            /*
             * A pipe is not a filesystem, and dos.library's IsInteractive()
             * asks exactly this: a handler that says it is NOT a filesystem is
             * an interactive stream.  That answer is what makes the Shell on
             * the far end print a prompt, so it is the difference between a
             * terminal and a batch script reader.
             */
            case ACTION_IS_FILESYSTEM:
                term_reply(pkt, DOSFALSE, 0);
                continue;

            case ACTION_SEEK:
                term_reply(pkt, -1, ERROR_SEEK_ERROR);
                continue;

            /* One port serves both handles, so this cannot say WHICH one is
               being asked about.  "No character waiting" makes the caller come
               back with a Read(), which is answered properly. */
            case ACTION_WAIT_CHAR:
                term_reply(pkt, DOSFALSE, 0);
                continue;

            case ACTION_FLUSH:
            case ACTION_SET_FILE_SIZE:
                term_reply(pkt, DOSTRUE, 0);
                continue;

            default:
                term_reply(pkt, DOSFALSE, ERROR_ACTION_NOT_KNOWN);
                continue;
        }

        p = term_pipe_of(pkt->dp_Arg1);
        if (p == NULL)
        {
            term_reply(pkt, DOSFALSE, ERROR_INVALID_LOCK);
            continue;
        }

        /* Read BEFORE term_reply() overwrites dp_Port with ours. */
        if (pkt->dp_Port != NULL)
            term_shell_task = pkt->dp_Port->mp_SigTask;

        switch (pkt->dp_Type)
        {
            case ACTION_READ:
            case ACTION_WRITE:
                /* One held packet per pipe is enough: the Shell is one
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
 * a packet.  DOS frees the handle when it is closed, so this must not free it.
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
    fh->fh_Arg1 = id;

    if (shell_reads)
        p->dosread = 1;
    else
        p->doswrite = 1;

    return MKBADDR(fh);
}

/* ---------------------------------------------------------- the runner --- */

static VOID term_runner_main(VOID)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    TermRunner     *r;
    struct Task    *parent;

    /* The parent is filling tc_UserData; it signals when it is done. */
    (VOID)Wait(SIGF_SINGLE);

    r = (TermRunner *)me->pr_Task.tc_UserData;
    if (r == NULL)
        return;

    /*
     * Execute() and NOT SystemTagList(), and that is the whole of this file's
     * one real discovery.  dos.library's own autodoc for SystemTagList says
     * it in a single line -- "Similar to Execute(), but does not read commands
     * from the input filehandle" -- so System() with an empty command line has
     * nothing to do and returns 0 at once.  Measured exactly that way: the
     * upgrade completed, the runner published rc 0 within milliseconds, and
     * the browser saw a session that opened and closed with not one byte in
     * it.
     *
     * Execute()'s contract is the one this needs: "If the input file handle is
     * nonzero then after the (possibly empty) commandString is performed
     * subsequent input is read from the specified input file handle until end
     * of that file is reached."  An empty string and a live input handle is
     * the documented way to put a Shell on a serial port, and this is the same
     * thing with a pipe where the port would be.
     *
     * What it costs is the tags: Execute() creates the Shell process itself,
     * so there is no NP_StackSize to give it and no NP_Name to find it by.
     * The stack the Shell gives its commands is the system default and a
     * person who needs more types `stack 65536`, exactly as they would in any
     * other Shell.  The name is not needed either -- see term_shell_task,
     * which learns the Shell's Task from the packets it sends rather than by
     * guessing what dos.library called it.
     */
    r->rn_Rc = (LONG)Execute((CONST_STRPTR)"", r->rn_In, r->rn_Out);

    if (r->rn_Rc == 0)
    {
        /*
         * Nothing took the handles, so they are still ours to give back.  On
         * the success path they are NOT closed here: the Shell owns them from
         * that moment, and this side learns it has finished with them from the
         * ACTION_END it sends when it closes them -- which works whether
         * Execute() returned at once (an interactive Shell, like NewShell) or
         * held until the Shell exited.  Closing them here on success would
         * take them out from under a Shell that is still using them.
         */
        r->rn_Err = IoErr();
        Close(r->rn_In);
        Close(r->rn_Out);
    }

    /* rn_Done is published last and nothing in the record is read after it. */
    parent = r->rn_Parent;
    r->rn_Done = 1;
    Signal(parent, SIGBREAKF_CTRL_E);
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

    /* "DH0:" already ends in its separator; "DH0:Work" does not, and the
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
    return (term_port != NULL && !term_active) ? TRUE : FALSE;
}

BOOL http_term_running(VOID)
{
    if (!term_active)
        return FALSE;

    /* Still running, or finished with output nobody has read yet.  A session
       that ends with the last line of `list` still in the ring must not be
       reported as over: that line is the answer. */
    if (!term_reaped)
        return TRUE;

    return (ring_used(&term_out) > 0UL) ? TRUE : FALSE;
}

BOOL http_term_start(VOID)
{
    struct TagItem  tags[4];
    struct Process *proc;
    BPTR            sh_in  = (BPTR)0;
    BPTR            sh_out = (BPTR)0;

    if (!http_term_available())
        return FALSE;

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
        if (sh_in  != (BPTR)0) Close(sh_in);
        if (sh_out != (BPTR)0) Close(sh_out);
        return FALSE;
    }

    term_runner.rn_Parent = FindTask(NULL);
    term_runner.rn_In     = sh_in;
    term_runner.rn_Out    = sh_out;
    term_runner.rn_Rc     = 0;
    term_runner.rn_Err    = 0;
    term_runner.rn_Done   = 0;

    tags[0].ti_Tag = NP_Entry;     tags[0].ti_Data = (ULONG)term_runner_main;
    tags[1].ti_Tag = NP_Name;      tags[1].ti_Data = (ULONG)"httpd terminal runner";
    tags[2].ti_Tag = NP_StackSize; tags[2].ti_Data = TERM_RUNNER_STACK;
    tags[3].ti_Tag = TAG_END;      tags[3].ti_Data = 0;

    proc = CreateNewProc(tags);
    if (proc == NULL)
    {
        Close(sh_in);
        Close(sh_out);
        return FALSE;
    }

    /* The runner is parked in Wait(SIGF_SINGLE) until this pair happens. */
    proc->pr_Task.tc_UserData = (APTR)&term_runner;
    Signal((struct Task *)proc, SIGF_SINGLE);

    term_active = 1;
    term_reaped = 0;
    term_rc     = -1;

    return TRUE;
}

LONG http_term_write(const UBYTE *data, LONG len)
{
    LONG n;

    if (!term_active || len <= 0 || term_in.closed)
        return 0;

    /* The Shell has closed its stdin: it is on its way out and nothing more
       will be read.  Report the bytes taken rather than stalling; a session
       with data pending for ever never closes. */
    if (term_in.dosend)
        return len;

    n = (LONG)ring_put(&term_in, data, (ULONG)len);

    if (n > 0)
        term_retry(&term_in);       /* a Shell blocked in Read() can go     */

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
       that is never coming and the runner never returns. */
    term_retry(&term_in);
}

VOID http_term_break(VOID)
{
    if (!term_active || term_shell_task == NULL)
        return;

    Signal(term_shell_task, SIGBREAKF_CTRL_C);
}

LONG http_term_rc(VOID)
{
    return term_rc;
}

VOID http_term_stop(VOID)
{
    if (!term_active)
        return;

    /* End of file in both directions.  The Shell exits, the runner closes the
       two handles, and this side learns both through ACTION_END. */
    term_in.closed  = 1;
    term_out.closed = 1;

    term_retry(&term_in);
    term_retry(&term_out);

    /* Whatever the Shell had left to say is not going anywhere now, so the
       ring is emptied rather than held: it is what http_term_service() waits
       on before it lets the next session start. */
    term_out.count = 0;
    term_out.rd    = 0;
    term_out.wr    = 0;

    http_term_service();
}

VOID http_term_shutdown(VOID)
{
    LONG waited = 0;

    if (term_port == NULL)
        return;

    if (term_active)
    {
        http_term_stop();

        /*
         * The runner still holds two FileHandles that name this process's
         * port, and the Shell may still be inside a command.  Keep answering
         * packets until it has gone, and keep draining the output ring: a
         * Shell blocked in Write() cannot notice that its input has ended.
         */
        while (!term_reaped && waited < TERM_STOP_TICKS)
        {
            UBYTE scratch[256];

            http_term_service();

            while (http_term_read(scratch, (LONG)sizeof(scratch)) > 0)
                ;

            Delay(1);
            waited++;
        }

        if (!term_reaped)
        {
            /*
             * Deliberately not freed.  The runner's Process is still alive and
             * both FileHandles still name term_port, so giving any of it back
             * would hand a live pointer to whatever allocates next.  On
             * AmigaOS a process that exits reclaims nothing anyway, so the
             * choice is between a leak this program is about to end with and a
             * write into somebody else's memory.
             */
            tool_error("the terminal's Shell is still running; its memory is "
                       "not being given back");
            return;
        }

        term_active = 0;
    }

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
 * section is about ONE upgraded socket; the Shell above it is the module's
 * own and is shared, because there is one of it.
 */

/*
 * Queue one control frame.  There is room for exactly one: a pong and a close
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

/*
 * What arrived.  Keystrokes are HELD rather than written straight to the
 * Shell, because the Shell may not be reading and a decoder's sink has nowhere
 * to refuse bytes to.  The socket is not read again until the hold is empty,
 * which is what turns "the Shell is busy" into TCP back pressure instead of
 * lost input.
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
         * Text is the OTHER channel: one word, and what it asks for is
         * something that is not a keystroke.
         *
         * Ctrl-C on an Amiga is a signal and not a byte -- a console handler
         * turns the key into SIGBREAKF_CTRL_C and the command polls for it --
         * so there is no in-band way to send one down a pipe.  Stealing 0x03
         * out of the input stream would have worked and would have made that
         * byte untypeable for ever; a second opcode costs nothing and takes
         * nothing away.  The page sends keystrokes as binary and never as
         * text, so the two cannot be confused.
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

                if (t->word[0] == 'b' && t->word[1] == 'r' &&
                    t->word[2] == 'e' && t->word[3] == 'a' &&
                    t->word[4] == 'k' && t->word[5] == '\0')
                    http_term_break();
                else if (t->word[0] == 'e' && t->word[1] == 'o' &&
                         t->word[2] == 'f' && t->word[3] == '\0')
                    http_term_eof();

                t->word_n = 0;
            }
            break;

        case HTTP_WS_EV_PING:
            /* RFC 6455 5.5.3: a pong carries the ping's payload back. */
            sock_control(t, HTTP_WS_EV_PONG, data, (ULONG)len);
            break;

        case HTTP_WS_EV_PONG:
            t->pinged = 0;
            break;

        case HTTP_WS_EV_CLOSE:
            /* RFC 6455 5.5.1: answer with a close of our own and then stop.
               The code is echoed, which is what a client that sent 1000
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

/* Move what was typed into the Shell.  Short writes are normal: the Shell's
   ring is small and a paste is bigger than one command line. */
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
    t->ctl_n    = 0;
    t->ctl_at   = 0;
    t->word_n   = 0;
    t->pinged   = 0;
    t->closing  = 0;
    t->why      = 0;
    t->progress = now;

    http_ws_reset(&t->in);

    if (first_len > 0UL)
        (VOID)http_ws_feed(&t->in, first, (long)first_len, sock_sink, t);
}

BOOL http_term_sock_wants_write(const HttpTermSock *t)
{
    if (t->out_sent < t->out_len || t->ctl_at < t->ctl_n || t->closing)
        return TRUE;

    if (http_term_pending() > 0UL)
        return TRUE;

    /* The Shell has gone and the close has not been sent yet. */
    return http_term_running() ? FALSE : TRUE;
}

BOOL http_term_sock_read(HttpTermSock *t, ULONG now)
{
    UBYTE scratch[HTTP_TERM_READ];
    LONG  got;

    sock_feed_shell(t);

    /* Not read at all while the Shell has not taken what came last.  This is
       the whole of the flow control in this direction and it is deliberate:
       the alternative is a buffer that grows with whatever a browser pastes. */
    if (t->pend_at < t->pend_n)
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
           close frame and stop reading; anything after it is not a frame. */
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
         * The Shell's output, framed IN PLACE.  Read into the buffer past the
         * longest header there is and then write the header BACKWARDS from
         * there, so the frame is contiguous and nothing is copied twice: the
         * send cursor simply starts wherever the header turned out to begin.
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

                for (i = 0; i < hn; i++)
                    t->out[10 - hn + i] = head[i];

                t->out_sent = 10UL - hn;
                t->out_len  = 10UL + (ULONG)n;
                continue;
            }
        }

        /*
         * Nothing left to send.  If the Shell has gone too, so has the
         * session: the close is sent here rather than when it exited, so the
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

BOOL http_term_sock_idle(HttpTermSock *t, ULONG now, ULONG timeout)
{
    if (timeout == 0UL || now < t->progress)
        return TRUE;

    if (now - t->progress < timeout)
        return TRUE;

    if (t->pinged)
        return FALSE;               /* no answer to the last one            */

    if (t->ctl_at >= t->ctl_n)
    {
        sock_control(t, HTTP_WS_EV_PING, (const UBYTE *)"", 0);
        t->pinged   = 1;
        t->progress = now;
    }

    return TRUE;
}
