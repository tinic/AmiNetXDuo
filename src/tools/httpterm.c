/*
 * httpterm, an AmigaDOS Shell on the other end of a console handler.
 * SPDX-License-Identifier: MIT
 */

#include "httpterm.h"

#include <dos/dostags.h>
#include <dos/dosasl.h>
#include <exec/execbase.h>          /* task lists, for runner lifetime      */
#include <exec/io.h>                /* struct IOStdReq, for ACTION_DISK_INFO */

#define TERM_OUT_BUF    4096UL
#define TERM_IN_BUF     1024UL

#define TERM_RUNNER_STACK   (64UL * 1024UL)

#define TERM_SHELL_STACK    (16UL * 1024UL)

#define TERM_SHELL_SETUP    "prompt \"%N.%S> \""

/* When http_term_shutdown() reports that it is still waiting.  It cannot
   safely stop waiting: every runner executes out of this command's load
   segment, which DOS unloads when the parent returns. */
#define TERM_STOP_WARN_TICKS 500    /* of 1/50 s                            */

#define TERM_STOP_PASSES    20

#define TERM_ABANDON_TICKS  250     /* of 1/50 s                            */

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

static UBYTE term_raw;              /* the handler is in RAW mode           */
static UBYTE term_mode_pending;     /* and the page has not been told yet   */

static UWORD term_cols = 80;
static UWORD term_rows = 25;

static UBYTE term_in_urgent;

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

static UBYTE      term_abandoned;
static ULONG      term_stop_at;     /* fiftieths, when stopping began       */

static LONG       term_err;

static struct Task *term_shell_task;

static struct MsgPort *term_break_port;

#define TERM_DISK_CON     0x434F4E00L     /* 'CON\0' */
#define TERM_DISK_RAWCON  0x52415700L     /* 'RAW\0' */

static struct IOStdReq term_ioreq;

static struct MsgPort *term_port;

#define TERM_ID_IN      1
#define TERM_ID_OUT     2

#define TERM_ID_CON     3

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

static BOOL term_packet_current(const struct DosPacket *pkt)
{
    if (!term_active || pkt->dp_Port == NULL)
        return FALSE;

    if (term_break_port != NULL)
        return (pkt->dp_Port == term_break_port) ? TRUE : FALSE;

    if (term_shell_task == NULL)
        term_shell_task = pkt->dp_Port->mp_SigTask;

    return (term_shell_task != NULL &&
            pkt->dp_Port->mp_SigTask == term_shell_task) ? TRUE : FALSE;
}

#define TERM_SEQ_MAX    24      /* a runaway parameter list is not a sequence */

static UBYTE term_seq[TERM_SEQ_MAX];
static UBYTE term_seq_n;                /* 0 when not inside a sequence      */
static UBYTE term_seq_esc;              /* it began with ESC and wants a '[' */

static UBYTE term_want_resize;

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
        if (p->closed)
        {
            /* Nobody reads it now.  -1 with a real error, so a command writing
               into a channel that has gone fails rather than looping on a
               short write for ever. */
            p->held = NULL;
            term_reply(pkt, -1, ERROR_INVALID_LOCK);
            return;
        }
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

static struct DosPacket *term_wait_pkt;
static ULONG             term_wait_from;   /* fiftieths, see term_ticks()   */
static ULONG             term_wait_until;

static ULONG term_ticks(VOID)
{
    struct DateStamp ds;

    DateStamp(&ds);

    return (ULONG)ds.ds_Minute * 3000UL + (ULONG)ds.ds_Tick;
}

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

    if (term_trace && term_active && !term_said_rc &&
        term_runner != NULL && term_runner->rn_Done != 0)
    {
        term_said_rc = 1;
        tool_printf("httpd: terminal: Execute() returned %ld (IoErr %ld)\n",
                    term_runner->rn_Rc, (LONG)term_runner->rn_Err);
        (VOID)Flush(Output());
    }

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

        if (term_stop_passes % TERM_STOP_PASSES == 0)
            http_term_break();

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
        BOOL ended  = (done ||
                       (term_in.dosend && term_out.dosend)) ? TRUE : FALSE;

        if (failed)
        {
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

        /* Must be cleared here: a late `break`, or http_term_stop() during the
           drain, must not signal through a freed MsgPort or Task. */
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

            case ACTION_IS_FILESYSTEM:
                term_reply(pkt, DOSFALSE, 0);
                continue;

            case ACTION_SEEK:
                term_reply(pkt, -1, ERROR_SEEK_ERROR);
                continue;

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

            case ACTION_CHANGE_SIGNAL:
            {
                struct MsgPort *was = term_break_port;
                LONG            id  = pkt->dp_Arg1 & 0xFF;

                /* dos.library passes fh_Arg1 here, so its generation must be
                   honoured: without the check an abandoned Shell can redirect
                   Ctrl-C in the replacement session to one of its own tasks. */
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

        if (pkt->dp_Type == ACTION_END &&
            (pkt->dp_Arg1 & 0xFF) == TERM_ID_CON)
        {
            term_reply(pkt, DOSTRUE, 0);
            continue;
        }

        p = term_pipe_of(pkt->dp_Arg1, pkt->dp_Type);
        if (p == NULL)
        {
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
                if (pkt->dp_Arg3 <= 0)
                {
                    term_reply(pkt, 0, 0);
                    break;
                }

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

    fh->fh_Port = term_port;

    if (shell_reads)
        p->dosread = 1;
    else
        p->doswrite = 1;

    return MKBADDR(fh);
}

/*
 * Dispose of a handle that has not been handed to the runner.  Must NOT use
 * Close(): fh_Type names term_port and the caller is the process that services
 * it, so it would block forever waiting for its own ACTION_END reply.
 */
static VOID term_handle_discard(BPTR handle)
{
    if (handle != (BPTR)0)
        FreeDosObject(DOS_FILEHANDLE, (APTR)BADDR(handle));
}

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

    {
        struct CommandLineInterface *cli = Cli();

        if (cli != NULL)
            cli->cli_DefaultStack = TERM_SHELL_STACK / 4UL;
    }

    r->rn_Rc = (LONG)Execute((CONST_STRPTR)TERM_SHELL_SETUP,
                             r->rn_In, r->rn_Out);

    if (r->rn_Rc == 0)
        r->rn_Err = IoErr();

    Close(r->rn_In);
    Close(r->rn_Out);

    /* rn_Done is published last and nothing in the record is read after it. */
    parent = r->rn_Parent;
    r->rn_Done = 1;
    Signal(parent, SIGBREAKF_CTRL_E);
}

/* Whether Exec can still schedule this runner.  Must be called under Disable(),
   not Forbid(): interrupts move tasks between the scheduler lists.  The pointer
   is compared only, never dereferenced. */
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

    term_gen++;

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

    return TRUE;
}

BOOL http_term_raw(VOID)
{
    return term_raw ? TRUE : FALSE;
}

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

static VOID sock_word(const char *w)
{
    const char *rest;

    rest = sock_after(w, "break");
    if (rest != NULL && *rest == '\0')
    {
        http_term_break();
        return;
    }

    rest = sock_after(w, "eof");
    if (rest != NULL && *rest == '\0')
    {
        http_term_eof();
        return;
    }

    rest = sock_after(w, "stats");
    if (rest != NULL)
    {
        if (*rest == '\0')
        {
            term_st_pending = 1;
            return;
        }

        rest = sock_after(rest, "reset");
        if (rest != NULL && *rest == '\0')
        {
            term_st_writes = 0;
            term_st_wbytes = 0;
            term_st_frames = 0;
            term_st_fbytes = 0;
            term_st_pending = 1;
        }
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

        while (*rest == ' ')
            rest++;
        if (*rest != '\0')
            return;

        http_term_resize(cols, rows);
    }
}

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

        case HTTP_WS_EV_TEXT:
            for (i = 0; i < len; i++)
            {
                if (t->word_n < (UBYTE)sizeof(t->word) - 1)
                    t->word[t->word_n++] = (char)data[i];
                else
                    t->word_over = 1;
            }

            if (final)
            {
                t->word[t->word_n] = '\0';
                if (!t->word_over)
                    sock_word(t->word);
                t->word_n = 0;
                t->word_over = 0;
            }
            break;

        case HTTP_WS_EV_PING:
            /* RFC 6455 5.5.3: a pong carries the ping's payload back.  A close
               already queued must not be displaced by it. */
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
    t->word_over = 0;
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
