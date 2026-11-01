/*
 * AmiNetXDuo, eight applications inside the stack at once.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdarg.h>

#include <exec/types.h>
#include <exec/semaphores.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dostags.h>

#include <proto/exec.h>
#include <inline/macros.h>
#include <proto/dos.h>

#include "aminetxduo/health.h"
#include "aminetxduo/netstatus.h"

#ifndef CT_PAIRS
#define CT_PAIRS        4               /* 4 servers + 4 clients == 8 bases  */
#endif
#ifndef CT_BYTES
#define CT_BYTES        32768UL         /* per pair, each way                */
#endif
#define CT_APPS         (2 * CT_PAIRS)
#define CT_PORT_BASE    7420
#define CT_CHUNK        2048UL

#define CT_STACK        (64UL * 1024UL)

#ifndef CT_DEADLINE_SECS
#define CT_DEADLINE_SECS  60
#endif
#define CT_BOOT_SECS      90
#define CT_KILL_SECS      30            /* the removed-task phase, bounded   */
#define CT_BUDGET_SECS    (CT_BOOT_SECS + 2 * CT_DEADLINE_SECS + CT_KILL_SECS + 30)

#define CT_TIMEOUT_TICKS  (CT_DEADLINE_SECS * 50)   /* 50 ticks/s            */
#define CT_POLL_TICKS     10
#define CT_BEAT_TICKS     100           /* heartbeat every 2 s               */

#define C_AF_INET       2
#define C_SOCK_STREAM   1
#define C_SOL_SOCKET    0xFFFF
#define C_SO_REUSEADDR  0x0004

typedef struct CtAddr
{
    UBYTE   sin_len;
    UBYTE   sin_family;
    UWORD   sin_port;
    ULONG   sin_addr;
    UBYTE   sin_zero[8];
} CtAddr;

static LONG c_socket(struct Library *base, LONG dom, LONG type, LONG proto)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = dom;
    register LONG            d1  __asm("d1") = type;
    register LONG            d2  __asm("d2") = proto;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");

    __asm __volatile ("jsr a6@(-30:W)"
                      : "=r" (res), "=r" (_clob_d1)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (d2)
                      : "a0", "a1", "cc", "memory");
    return res;
}

static LONG c_bind(struct Library *base, LONG s, CtAddr *sa)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = sa;
    register LONG            d1  __asm("d1") = (LONG)sizeof(CtAddr);
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-36:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1)
                      : "a1", "cc", "memory");
    return res;
}

static LONG c_listen(struct Library *base, LONG s, LONG backlog)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register LONG            d1  __asm("d1") = backlog;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");

    __asm __volatile ("jsr a6@(-42:W)"
                      : "=r" (res), "=r" (_clob_d1)
                      : "r" (a6), "r" (d0), "r" (d1)
                      : "a0", "a1", "cc", "memory");
    return res;
}

static LONG c_accept(struct Library *base, LONG s)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = (APTR)0;
    register APTR            a1  __asm("a1") = (APTR)0;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-48:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (a1)
                      : "cc", "memory");
    return res;
}

static LONG c_connect(struct Library *base, LONG s, CtAddr *sa)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = sa;
    register LONG            d1  __asm("d1") = (LONG)sizeof(CtAddr);
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-54:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1)
                      : "a1", "cc", "memory");
    return res;
}

static LONG c_send(struct Library *base, LONG s, APTR buf, LONG len)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = buf;
    register LONG            d1  __asm("d1") = len;
    register LONG            d2  __asm("d2") = 0;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-66:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1), "r" (d2)
                      : "a1", "cc", "memory");
    return res;
}

static LONG c_recv(struct Library *base, LONG s, APTR buf, LONG len)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = buf;
    register LONG            d1  __asm("d1") = len;
    register LONG            d2  __asm("d2") = 0;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-78:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1), "r" (d2)
                      : "a1", "cc", "memory");
    return res;
}

static LONG c_setsockopt(struct Library *base, LONG s, LONG level, LONG name,
                         APTR val, LONG len)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register LONG            d1  __asm("d1") = level;
    register LONG            d2  __asm("d2") = name;
    register APTR            a0  __asm("a0") = val;
    register LONG            d3  __asm("d3") = len;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-90:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (d2), "r" (a0),
                        "r" (d3)
                      : "a1", "cc", "memory");
    return res;
}

static LONG c_close(struct Library *base, LONG s)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");

    __asm __volatile ("jsr a6@(-120:W)"
                      : "=r" (res), "=r" (_clob_d1)
                      : "r" (a6), "r" (d0)
                      : "a0", "a1", "cc", "memory");
    return res;
}

static LONG c_netstackcontrol(struct Library *base, ULONG op, APTR arg,
                              ULONG size)
{
    register struct Library *a6  __asm("a6") = base;
    register ULONG           d0  __asm("d0") = AMI_NETSTATUS_MAGIC;
    register ULONG           d1  __asm("d1") = op;
    register APTR            a0  __asm("a0") = arg;
    register ULONG           d2  __asm("d2") = size;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-876:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (a0), "r" (d2)
                      : "a1", "cc", "memory");
    return res;
}

static LONG c_waitselect(struct Library *base, LONG nfds, APTR rd,
                         APTR timeout, ULONG *sigs)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = nfds;
    register APTR            a0  __asm("a0") = rd;
    register APTR            a1  __asm("a1") = NULL;
    register APTR            a2  __asm("a2") = NULL;
    register APTR            a3  __asm("a3") = timeout;
    register ULONG          *d1  __asm("d1") = sigs;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-126:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (a1), "r" (a2),
                        "r" (a3), "r" (d1)
                      : "cc", "memory");
    return res;
}

static APTR c_gethostbyname(struct Library *base, const char *name)
{
    register struct Library *a6  __asm("a6") = base;
    register const char     *a0  __asm("a0") = name;
    register APTR            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-210:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0)
                      : "a1", "cc", "memory");
    return res;
}

static LONG c_errno(struct Library *base)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");

    __asm __volatile ("jsr a6@(-162:W)"
                      : "=r" (res), "=r" (_clob_d1)
                      : "r" (a6)
                      : "a0", "a1", "cc", "memory");
    return res;
}

typedef struct CtApp
{
    UWORD           ca_Id;
    UWORD           ca_IsServer;
    UWORD           ca_Port;
    volatile UWORD  ca_Started;
    volatile UWORD  ca_Done;
    volatile LONG   ca_Failures;
    volatile ULONG  ca_Bytes;
    volatile LONG   ca_Errno;         /* first errno that went wrong        */
    volatile LONG   ca_Where;         /* which call, for the report         */
    volatile LONG   ca_Phase;         /* how far it got, for a killed run   */
    struct Process *ca_Proc;
} CtApp;

static CtApp ct_app[CT_APPS];

static ULONG ct_checks;
static ULONG ct_failures;

#define CT_W_SOCKET   1
#define CT_W_BIND     2
#define CT_W_LISTEN   3
#define CT_W_ACCEPT   4
#define CT_W_CONNECT  5
#define CT_W_SEND     6
#define CT_W_RECV     7
#define CT_W_VERIFY   8
#define CT_W_LIBRARY  9

/* Phases, in the order an application passes through them. Reported on every
   heartbeat, so a run killed mid-flight says how far each one got. */
#define CT_P_SPAWNED   0
#define CT_P_WOKE      1
#define CT_P_OPENED    2
#define CT_P_SOCKET    3
#define CT_P_BOUND     4
#define CT_P_LISTEN    5
#define CT_P_ACCEPTED  6
#define CT_P_CONNECTED 7
#define CT_P_XFER      8
#define CT_P_CLOSED    9
#define CT_P_EXIT     10

static const char *const ct_phase_name[] = {
    "spawned", "woke", "opened", "socket", "bound", "listening",
    "accepted", "connected", "transferring", "closed", "exited"
};

#ifndef RawPutChar
#  define RawPutChar(c) \
      LP1NR(0x204, RawPutChar, UBYTE, (c), d0, , EXEC_BASE_NAME)
#endif

static VOID ct_put(register UBYTE c      __asm("d0"),
                   register APTR  unused __asm("a3"))
{
    (VOID)unused;
    if (c != '\0')
        RawPutChar(c);
}

static VOID ct_trace(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);

    /* Nine Processes trace to one serial port, and a line is three RawDoFmt
       calls. Without this they interleave mid-line and the log stops being
       readable exactly when it matters. */
    Forbid();
    RawDoFmt((STRPTR)"[ct] ", (APTR)0, (void (*)())ct_put, (APTR)0);
    RawDoFmt((STRPTR)fmt, (APTR)ap, (void (*)())ct_put, (APTR)0);
    RawDoFmt((STRPTR)"\n", (APTR)0, (void (*)())ct_put, (APTR)0);
    Permit();

    va_end(ap);
}

static VOID ct_log(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    (VOID)VPrintf((STRPTR)fmt, (LONG *)ap);
    va_end(ap);
    (VOID)Flush(Output());
}

/* Every phase transition, not just failures: a run killed mid-flight has to
   say where it stopped, and every ct_check() is silent on success. */
static VOID ct_phase(CtApp *a, LONG phase)
{
    a->ca_Phase = phase;
    ct_trace("app %ld (%s) %s", (LONG)a->ca_Id,
             (LONG)(a->ca_IsServer ? "server" : "client"),
             (LONG)ct_phase_name[phase]);
}

static VOID ct_check(LONG ok, const char *what, LONG detail)
{
    ct_checks++;
    if (!ok)
    {
        ct_failures++;
        ct_log("  FAIL %s (%ld)\n", (LONG)what, detail);
        ct_trace("FAIL %s (%ld)", (LONG)what, detail);
    }
}

/* The payload byte a given pair owes at a given offset. Keyed to the pair, so
   a byte from another connection is detectable rather than plausible. */
static UBYTE ct_byte(UWORD pair, ULONG off)
{
    return (UBYTE)((ULONG)(pair * 31U + 7U) + off * 3UL);
}

static VOID ct_server_body(CtApp *a, struct Library *base)
{
    UBYTE  *buf;
    CtAddr  sa;
    LONG    ls;
    LONG    cs;
    LONG    one = 1;
    ULONG   done = 0;

    buf = (UBYTE *)AllocVec(CT_CHUNK, MEMF_PUBLIC);
    if (buf == NULL)
    {
        a->ca_Failures++;
        a->ca_Where = CT_W_LIBRARY;
        return;
    }

    ls = c_socket(base, C_AF_INET, C_SOCK_STREAM, 0);
    if (ls < 0)
    {
        a->ca_Failures++; a->ca_Where = CT_W_SOCKET; a->ca_Errno = c_errno(base);
        FreeVec(buf);
        return;
    }
    ct_phase(a, CT_P_SOCKET);

    (VOID)c_setsockopt(base, ls, C_SOL_SOCKET, C_SO_REUSEADDR,
                       (APTR)&one, (LONG)sizeof(one));

    sa.sin_len    = (UBYTE)sizeof(sa);
    sa.sin_family = C_AF_INET;
    sa.sin_port   = a->ca_Port;
    sa.sin_addr   = 0x7F000001UL;               /* 127.0.0.1 */

    if (c_bind(base, ls, &sa) < 0)
    {
        a->ca_Failures++; a->ca_Where = CT_W_BIND; a->ca_Errno = c_errno(base);
        (VOID)c_close(base, ls); FreeVec(buf);
        return;
    }
    ct_phase(a, CT_P_BOUND);

    if (c_listen(base, ls, 2) < 0)
    {
        a->ca_Failures++; a->ca_Where = CT_W_LISTEN; a->ca_Errno = c_errno(base);
        (VOID)c_close(base, ls); FreeVec(buf);
        return;
    }

    /* Listening: the client half may dial now. */
    ct_phase(a, CT_P_LISTEN);
    a->ca_Started = 1U;

    cs = c_accept(base, ls);
    if (cs < 0)
    {
        a->ca_Failures++; a->ca_Where = CT_W_ACCEPT; a->ca_Errno = c_errno(base);
        (VOID)c_close(base, ls); FreeVec(buf);
        return;
    }
    ct_phase(a, CT_P_ACCEPTED);

    /* Echo exactly what arrives, without inspecting it: the client owns the
       verdict on the bytes, and an echo that alters them would hide the
       cross-feed this test is looking for. */
    ct_phase(a, CT_P_XFER);
    while (done < CT_BYTES)
    {
        LONG got = c_recv(base, cs, (APTR)buf, (LONG)CT_CHUNK);
        LONG off = 0;

        if (got <= 0)
        {
            a->ca_Failures++; a->ca_Where = CT_W_RECV;
            a->ca_Errno = c_errno(base);
            break;
        }

        while (off < got)
        {
            LONG put = c_send(base, cs, (APTR)(buf + off), got - off);

            if (put <= 0)
            {
                a->ca_Failures++; a->ca_Where = CT_W_SEND;
                a->ca_Errno = c_errno(base);
                off = got;                  /* stop this chunk */
                done = CT_BYTES;            /* and the transfer */
                break;
            }
            off += put;
        }

        done += (ULONG)got;
        a->ca_Bytes = done;
    }

    (VOID)c_close(base, cs);
    (VOID)c_close(base, ls);
    FreeVec(buf);
    ct_phase(a, CT_P_CLOSED);
}

static VOID ct_client_body(CtApp *a, struct Library *base)
{
    UBYTE  *out;
    UBYTE  *in;
    CtAddr  sa;
    LONG    s;
    ULONG   sent = 0;
    ULONG   got  = 0;
    ULONG   i;

    out = (UBYTE *)AllocVec(CT_CHUNK, MEMF_PUBLIC);
    in  = (UBYTE *)AllocVec(CT_CHUNK, MEMF_PUBLIC);
    if (out == NULL || in == NULL)
    {
        a->ca_Failures++; a->ca_Where = CT_W_LIBRARY;
        if (out != NULL) FreeVec(out);
        if (in  != NULL) FreeVec(in);
        return;
    }

    s = c_socket(base, C_AF_INET, C_SOCK_STREAM, 0);
    if (s < 0)
    {
        a->ca_Failures++; a->ca_Where = CT_W_SOCKET; a->ca_Errno = c_errno(base);
        FreeVec(out); FreeVec(in);
        return;
    }
    ct_phase(a, CT_P_SOCKET);

    sa.sin_len    = (UBYTE)sizeof(sa);
    sa.sin_family = C_AF_INET;
    sa.sin_port   = a->ca_Port;
    sa.sin_addr   = 0x7F000001UL;

    if (c_connect(base, s, &sa) < 0)
    {
        a->ca_Failures++; a->ca_Where = CT_W_CONNECT; a->ca_Errno = c_errno(base);
        (VOID)c_close(base, s); FreeVec(out); FreeVec(in);
        return;
    }

    ct_phase(a, CT_P_CONNECTED);
    a->ca_Started = 1U;

    ct_phase(a, CT_P_XFER);
    while (sent < CT_BYTES)
    {
        ULONG want = CT_BYTES - sent;
        LONG  off  = 0;
        LONG  put;
        ULONG back = 0;

        if (want > CT_CHUNK)
            want = CT_CHUNK;

        for (i = 0; i < want; i++)
            out[i] = ct_byte(a->ca_Id % CT_PAIRS, sent + i);

        while ((ULONG)off < want)
        {
            put = c_send(base, s, (APTR)(out + off), (LONG)(want - (ULONG)off));
            if (put <= 0)
            {
                a->ca_Failures++; a->ca_Where = CT_W_SEND;
                a->ca_Errno = c_errno(base);
                goto done;
            }
            off += put;
        }

        while (back < want)
        {
            LONG n = c_recv(base, s, (APTR)(in + back), (LONG)(want - back));

            if (n <= 0)
            {
                a->ca_Failures++; a->ca_Where = CT_W_RECV;
                a->ca_Errno = c_errno(base);
                goto done;
            }
            back += (ULONG)n;
        }

        for (i = 0; i < want; i++)
        {
            if (in[i] != out[i])
            {
                a->ca_Failures++;
                a->ca_Where = CT_W_VERIFY;
                a->ca_Errno = (LONG)(sent + i);   /* the offset that differed */
                goto done;
            }
        }

        sent += want;
        got  += want;
        a->ca_Bytes = got;
    }

done:
    (VOID)c_close(base, s);
    FreeVec(out);
    FreeVec(in);
    ct_phase(a, CT_P_CLOSED);
}

static VOID ct_app_entry(VOID)
{
    struct Process *me = (struct Process *)FindTask((STRPTR)0);
    struct Library *base;
    CtApp          *a;

    Wait(SIGF_SINGLE);

    a = (CtApp *)me->pr_Task.tc_UserData;
    if (a == NULL)
    {
        ct_trace("child woke with no CtApp, SIGF_SINGLE was already set");
        return;
    }

    ct_phase(a, CT_P_WOKE);

    base = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4UL);
    if (base == NULL)
    {
        ct_trace("app %ld cannot open bsdsocket.library", (LONG)a->ca_Id);
        a->ca_Failures++;
        a->ca_Where = CT_W_LIBRARY;
        a->ca_Done  = 1U;
        return;
    }

    ct_phase(a, CT_P_OPENED);

    if (a->ca_IsServer != 0U)
        ct_server_body(a, base);
    else
        ct_client_body(a, base);

    CloseLibrary(base);
    ct_phase(a, CT_P_EXIT);
    a->ca_Done = 1U;
}

static struct Process *ct_spawn(CtApp *a, const char *name)
{
    struct Process *p;

    Forbid();

    p = CreateNewProcTags(NP_Entry,     (ULONG)ct_app_entry,
                          NP_Name,      (ULONG)name,
                          NP_Priority,  (ULONG)0,
                          NP_StackSize, CT_STACK,
                          NP_Cli,       (ULONG)FALSE,
                          TAG_DONE);

    if (p != NULL)
        p->pr_Task.tc_UserData = (APTR)a;

    Permit();

    if (p != NULL)
    {
        a->ca_Proc = p;
        ct_phase(a, CT_P_SPAWNED);
        Signal(&p->pr_Task, SIGF_SINGLE);
    }

    return p;
}

/* ------------------------------------- a Task removed inside a vector -- */

/*
 * An application removed by Exec while inside a socket call holds the ThreadX
 * baton for ever.  The library's one recovery, the 1 Hz task sweep, needs
 * sb_Lock, and the link jobs hold sb_Lock while a launched Process waits for
 * that baton: every program then hangs.  The fix reclaims the baton from the
 * tick, before the sweep, without the lock.
 *
 * Roles.  V loops socket()/CloseSocket() until it is removed.  W has the
 * library open and calls socket() once when told.  L has the library open and
 * takes the interface down when told, which is the sb_Lock holder that parks.
 * O opens the library while L holds the lock.
 *
 * Whether the removal landed inside the bracket is read from bs_Reclaimed,
 * not from how long W took: the reclaim runs at 1 Hz, and L's own job holds
 * the IP mutex for a few hundred ms, so W's time says nothing.  A miss is
 * W, L and O all back with the counter unchanged, and the attempt is
 * repeated.  A hit is the counter moved, with all three back inside 3 s; a
 * hit on a library without the reclaim is all three stalled past 3 s, which
 * is the defect.
 */
#define CK_VICTIM   1
#define CK_WAITER   2
#define CK_LINK     3
#define CK_OPENER   4

#define CK_ATTEMPTS 12
#define CK_FIX_TICKS   150              /* 3 s for everything to come back  */
#define CK_ARM_TICKS   250              /* 5 s to catch V holding the baton */

/* AmiBatonStats.bs_Reclaimed is the eighth ULONG (aminetxduo/netstack.h),
   read through the health mark: that header wants ThreadX's, which this
   harness must not include. */
#define CK_BS_RECLAIMED 7

typedef struct CtKill
{
    UWORD           ck_Role;
    UWORD           ck_Index;           /* CK_LINK: the interface           */
    volatile UWORD  ck_Opened;          /* library open, waiting for go     */
    volatile UWORD  ck_Returned;        /* the one call under test returned */
    volatile UWORD  ck_Done;
    volatile ULONG  ck_Loops;
    volatile LONG   ck_Result;
    volatile LONG   ck_Errno;
    struct Process *ck_Proc;
} CtKill;

static VOID ct_kill_entry(VOID)
{
    struct Process *me = (struct Process *)FindTask((STRPTR)0);
    struct Library *base;
    CtKill         *k;

    Wait(SIGF_SINGLE);
    k = (CtKill *)me->pr_Task.tc_UserData;
    if (k == NULL)
        return;

    base = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4UL);
    if (base == NULL)
    {
        k->ck_Result = -1;
        k->ck_Returned = 1U;
        k->ck_Done = 1U;
        return;
    }
    k->ck_Opened = 1U;

    switch (k->ck_Role)
    {
        case CK_VICTIM:
            for (;;)
            {
                LONG s = c_socket(base, C_AF_INET, C_SOCK_STREAM, 0);

                if (s < 0)
                {
                    k->ck_Result = -1;
                    k->ck_Errno  = c_errno(base);
                    break;
                }
                (VOID)c_close(base, s);
                k->ck_Loops++;
            }
            break;

        case CK_WAITER:
            Wait(SIGF_SINGLE);
            k->ck_Result = c_socket(base, C_AF_INET, C_SOCK_STREAM, 0);
            if (k->ck_Result < 0)
                k->ck_Errno = c_errno(base);
            k->ck_Returned = 1U;
            if (k->ck_Result >= 0)
                (VOID)c_close(base, k->ck_Result);
            break;

        case CK_LINK:
        {
            NetStatusControl ctl;
            UBYTE *p = (UBYTE *)&ctl;
            ULONG  i;

            for (i = 0; i < sizeof(ctl); i++)
                p[i] = 0;
            ctl.nsc_Magic   = AMI_NETSTATUS_MAGIC;
            ctl.nsc_Version = (UWORD)AMI_NETSTATUS_VERSION;
            ctl.nsc_Index   = k->ck_Index;

            Wait(SIGF_SINGLE);
            k->ck_Result = c_netstackcontrol(base, NETCTRL_INTERFACE_DOWN,
                                             (APTR)&ctl, (ULONG)sizeof(ctl));
            if (k->ck_Result != 0)
                k->ck_Errno = c_errno(base);
            k->ck_Returned = 1U;
            break;
        }

        default:
            k->ck_Returned = 1U;        /* the open was the call            */
            break;
    }

    CloseLibrary(base);
    k->ck_Done = 1U;
}

static struct Process *ct_kill_spawn(CtKill *k, UWORD role, const char *name,
                                     BYTE pri)
{
    struct Process *p;

    k->ck_Role     = role;
    k->ck_Opened   = 0U;
    k->ck_Returned = 0U;
    k->ck_Done     = 0U;
    k->ck_Loops    = 0UL;
    k->ck_Result   = 0;
    k->ck_Errno    = 0;
    k->ck_Proc     = NULL;

    Forbid();
    p = CreateNewProcTags(NP_Entry,     (ULONG)ct_kill_entry,
                          NP_Name,      (ULONG)name,
                          NP_Priority,  (ULONG)(LONG)pri,
                          NP_StackSize, CT_STACK,
                          NP_Cli,       (ULONG)FALSE,
                          TAG_DONE);
    if (p != NULL)
        p->pr_Task.tc_UserData = (APTR)k;
    Permit();

    if (p != NULL)
    {
        k->ck_Proc = p;
        Signal(&p->pr_Task, SIGF_SINGLE);
    }
    return p;
}

/* Ticks until *flag is set, or -1 past the deadline. */
static LONG ct_ticks_until(volatile UWORD *flag, ULONG deadline)
{
    ULONG waited = 0UL;

    while (*flag == 0U)
    {
        if (waited >= deadline)
            return -1;
        Delay(1);
        waited++;
    }
    return (LONG)waited;
}

/*
 * ARM THE KILL, THEN KILL, INSIDE ONE Forbid().
 *
 * The removal only proves anything if V holds the baton at the instant it
 * happens: a victim removed anywhere else leaves nothing for the reclaim to
 * find.  The old code removed V after a loop count and missed the bracket
 * about one run in four, and the harness then reported the miss as a reclaim
 * failure.
 *
 * hm_Holder is the port's live baton-holder word.  A thread that holds the
 * baton has necessarily been published, so reading V's address there confirms
 * both halves of that much at once.
 *
 * AND V MUST OWN NO BSDSOCKET LOCK.  A Task removed while owning sb_Lock wedges
 * the library for the rest of the boot, and no baton reclaim can or should
 * undo that -- it is a different defect.  Removing V there would make this test
 * report that defect as this one, so hm_SbLock's ss_Owner is part of the
 * precondition rather than part of the result.  Exec's own field, no bsdsocket
 * layout needed.
 *
 * Read and RemTask() happen under the same Forbid(), so neither can change in
 * between.
 *
 * TRUE only if V was removed while holding the baton and owning no lock.
 * FALSE means the precondition was never met inside the bound, which is an
 * UNMET PRECONDITION and not a result about the reclaim at all.
 */
static BOOL ct_kill_when_holding(CtKill *victim, ULONG bound_ticks,
                                 ULONG *waited_out)
{
    const AmiHealthMark    *mark;
    VOID * const           *holder = NULL;
    struct SignalSemaphore *sblock = NULL;
    struct Task            *target = &victim->ck_Proc->pr_Task;
    ULONG                   waited = 0UL;
    BOOL                    armed  = FALSE;

    Forbid();
    mark = (const AmiHealthMark *)FindSemaphore((STRPTR)AMI_HEALTH_NAME);
    if (mark != NULL &&
        mark->hm_Magic   == AMI_HEALTH_MAGIC &&
        mark->hm_Version == (UWORD)AMI_HEALTH_VERSION)
    {
        holder = (VOID * const *)mark->hm_Holder;
        sblock = (struct SignalSemaphore *)mark->hm_SbLock;
    }
    Permit();

    if (holder == NULL)
    {
        *waited_out = 0UL;
        return FALSE;
    }

    for (waited = 0UL; waited < bound_ticks; waited++)
    {
        Forbid();
        if (victim->ck_Done == 0U && *holder == (VOID *)target &&
            (sblock == NULL || sblock->ss_Owner != target))
        {
            RemTask(target);
            armed = TRUE;
        }
        Permit();

        if (armed)
            break;

        Delay(1);
    }

    *waited_out = waited;
    return armed;
}

static BOOL ct_health_reclaimed(ULONG *out)
{
    const AmiHealthMark *mark;
    ULONG                value = 0UL;
    BOOL                 ok    = FALSE;

    Forbid();
    mark = (const AmiHealthMark *)FindSemaphore((STRPTR)AMI_HEALTH_NAME);
    if (mark != NULL &&
        mark->hm_Magic   == AMI_HEALTH_MAGIC &&
        mark->hm_Version == (UWORD)AMI_HEALTH_VERSION &&
        mark->hm_Baton   != NULL)
    {
        value = ((const ULONG *)mark->hm_Baton)[CK_BS_RECLAIMED];
        ok    = TRUE;
    }
    Permit();

    *out = value;
    return ok;
}

static VOID ct_interface_up(struct Library *base)
{
    NetStatusControl ctl;
    UBYTE *p = (UBYTE *)&ctl;
    ULONG  i;

    for (i = 0; i < sizeof(ctl); i++)
        p[i] = 0;
    ctl.nsc_Magic   = AMI_NETSTATUS_MAGIC;
    ctl.nsc_Version = (UWORD)AMI_NETSTATUS_VERSION;
    ctl.nsc_Index   = 0;
    (VOID)c_netstackcontrol(base, NETCTRL_INTERFACE_UP, (APTR)&ctl,
                            (ULONG)sizeof(ctl));
}

static CtKill ck_victim;
static CtKill ck_waiter;
static CtKill ck_link;
static CtKill ck_opener;

static VOID ct_kill_phase(struct Library *base)
{
    ULONG  reclaimed_before = 0UL;
    ULONG  reclaimed_now    = 0UL;
    UWORD  attempt;
    BOOL   hit     = FALSE;
    BOOL   stalled = FALSE;
    BOOL   armed   = FALSE;
    ULONG  arm_ticks = 0UL;
    LONG   w_ticks = -1;
    LONG   l_ticks = -1;
    LONG   o_ticks = -1;

    ct_log("concurrent: removing an application inside a socket call\n");
    ct_trace("kill: start");

    ct_check(ct_health_reclaimed(&reclaimed_before),
             "the health mark is published", 0);
    /* A baseline, not a zero: the counter is the library's and anything
       earlier in this run is entitled to have moved it. */
    ct_log("concurrent: kill_reclaimed_before=%ld\n", (LONG)reclaimed_before);

    for (attempt = 1; attempt <= CK_ATTEMPTS && !hit && !stalled; attempt++)
    {
        /* W and L open first and wait, so the calls under test are the only
           thing between the removal and the stall. */
        ck_link.ck_Index = 0;
        if (ct_kill_spawn(&ck_waiter, CK_WAITER, "anxd-kill-W", 0) == NULL ||
            ct_kill_spawn(&ck_link, CK_LINK, "anxd-kill-L", 0) == NULL)
        {
            ct_check(0, "spawned W and L", (LONG)attempt);
            return;
        }
        if (ct_ticks_until(&ck_waiter.ck_Opened, 250UL) < 0 ||
            ct_ticks_until(&ck_link.ck_Opened, 250UL) < 0)
        {
            ct_check(0, "W and L opened the library", (LONG)attempt);
            return;
        }

        /* V below this Process, so a Delay() here returns on top of it
           wherever it happens to be inside its loop. */
        if (ct_kill_spawn(&ck_victim, CK_VICTIM, "anxd-kill-V", -1) == NULL)
        {
            ct_check(0, "spawned V", (LONG)attempt);
            return;
        }
        if (ct_ticks_until(&ck_victim.ck_Opened, 250UL) < 0)
        {
            ct_check(0, "V opened the library", (LONG)attempt);
            return;
        }
        /* Removed only once V is the baton holder; see ct_kill_when_holding().
           An attempt that cannot arm is not an attempt at the reclaim. */
        armed = ct_kill_when_holding(&ck_victim, CK_ARM_TICKS, &arm_ticks);
        if (!armed)
        {
            ct_trace("kill: attempt %ld UNMET PRECONDITION, V never held the "
                     "baton in %ld ticks (loops %ld)", (LONG)attempt,
                     (LONG)arm_ticks, (LONG)ck_victim.ck_Loops);
            (VOID)ct_ticks_until(&ck_waiter.ck_Done, 250UL);
            (VOID)ct_ticks_until(&ck_link.ck_Done, 250UL);
            break;
        }
        ct_trace("kill: attempt %ld removed V HOLDING the baton after %ld "
                 "arm ticks (loops %ld)", (LONG)attempt, (LONG)arm_ticks,
                 (LONG)ck_victim.ck_Loops);

        /* L first: it takes sb_Lock and launches the Process that parks
           behind a dead holder.  Then W's socket(), then O's open, which
           blocks on sb_Lock until L is done. */
        Signal(&ck_link.ck_Proc->pr_Task, SIGF_SINGLE);
        Signal(&ck_waiter.ck_Proc->pr_Task, SIGF_SINGLE);
        if (ct_kill_spawn(&ck_opener, CK_OPENER, "anxd-kill-O", 0) == NULL)
        {
            ct_check(0, "spawned O", (LONG)attempt);
            return;
        }

        w_ticks = ct_ticks_until(&ck_waiter.ck_Returned, CK_FIX_TICKS);
        l_ticks = ct_ticks_until(&ck_link.ck_Returned, CK_FIX_TICKS);
        o_ticks = ct_ticks_until(&ck_opener.ck_Opened, CK_FIX_TICKS);
        (VOID)ct_health_reclaimed(&reclaimed_now);

        if (w_ticks < 0 || l_ticks < 0 || o_ticks < 0)
        {
            stalled = TRUE;
            ct_log("concurrent: kill attempt %ld stalled W, L and O past 3 s\n",
                   (LONG)attempt);
            ct_trace("kill: attempt %ld stalled w=%ld l=%ld o=%ld reclaimed=%ld",
                     (LONG)attempt, w_ticks, l_ticks, o_ticks,
                     (LONG)reclaimed_now);
            break;
        }

        if (reclaimed_now != reclaimed_before)
        {
            hit = TRUE;
            ct_log("concurrent: kill attempt %ld hit the bracket\n",
                   (LONG)attempt);
            ct_trace("kill: attempt %ld hit, w=%ld l=%ld o=%ld reclaimed=%ld",
                     (LONG)attempt, w_ticks, l_ticks, o_ticks,
                     (LONG)reclaimed_now);
            break;
        }

        ct_trace("kill: attempt %ld missed the bracket, w=%ld l=%ld o=%ld",
                 (LONG)attempt, w_ticks, l_ticks, o_ticks);
        (VOID)ct_ticks_until(&ck_waiter.ck_Done, 250UL);
        (VOID)ct_ticks_until(&ck_link.ck_Done, 250UL);
        (VOID)ct_ticks_until(&ck_opener.ck_Done, 250UL);
        ct_interface_up(base);          /* back up for the next attempt */
    }

    /* A DISTINCT RESULT.  "V never held the baton" says nothing about the
       reclaim, so it is never reported as one failing. */
    if (!armed)
    {
        ct_check(0, "UNMET PRECONDITION: V never held the baton to be removed "
                 "from", (LONG)arm_ticks);
        ct_log("concurrent: kill_precondition=unmet kill_arm_ticks=%ld\n",
               (LONG)arm_ticks);
        return;
    }

    ct_check(hit || stalled, "a removal landed inside the bracket",
             (LONG)CK_ATTEMPTS);
    if (!hit && !stalled)
        return;

    ct_check(w_ticks >= 0, "W's socket() returned within 3 s", w_ticks);
    ct_check(w_ticks >= 0 && ck_waiter.ck_Result >= 0,
             "W's socket() succeeded", ck_waiter.ck_Errno);
    ct_check(l_ticks >= 0, "L's interface down returned within 3 s", l_ticks);
    ct_check(l_ticks >= 0 && ck_link.ck_Result == 0,
             "L's interface down succeeded", ck_link.ck_Errno);
    ct_check(o_ticks >= 0, "a fresh OpenLibrary() succeeded within 3 s",
             o_ticks);
    ct_check(reclaimed_now == reclaimed_before + 1UL,
             "exactly one baton was reclaimed",
             (LONG)(reclaimed_now - reclaimed_before));

    /* kill_reclaimed is the DELTA over the pre-kill snapshot, which is what
       the check above asserts; the absolute counter is the library's and
       anything earlier in the run is entitled to have moved it. */
    ct_log("concurrent: kill_w_ticks=%ld kill_l_ticks=%ld kill_o_ticks=%ld "
           "kill_reclaimed=%ld\n", w_ticks, l_ticks, o_ticks,
           (LONG)(reclaimed_now - reclaimed_before));
    ct_log("concurrent: kill_precondition=met kill_arm_ticks=%ld\n",
           (LONG)arm_ticks);

    if (hit)
    {
        (VOID)ct_ticks_until(&ck_waiter.ck_Done, 250UL);
        (VOID)ct_ticks_until(&ck_link.ck_Done, 250UL);
        (VOID)ct_ticks_until(&ck_opener.ck_Done, 250UL);
    }
}

/* ------------------------------ every adoption slot held by a reader -- */

/*
 * #383.  A Task blocked in recv() was suspended in NetX Duo and held one of
 * the port's adoption slots (tx_amiga_pool.c) until its data came.  Fifteen
 * such readers took every slot a program may have, and the sixteenth caller
 * of the stack -- any program, a browser's next connection, httpd -- waited
 * in the pool, deaf to Ctrl-C, for as long as the fifteen did.
 *
 * Here twenty readers block in recv() on loopback connections whose server
 * sends nothing, and a probe opens the library and makes a socket while they
 * wait.  All twenty must connect and the probe must finish inside the
 * deadline; then the server closes them all and every reader must see the
 * end of its stream.  Bounded at three deadlines, 15 s, inside the margin
 * run-concurrent.sh keeps; a passing run takes about two.
 */
#define CP_READERS      20
#define CP_PORT         7440
#define CP_STACK        (16UL * 1024UL)
#define CP_TICKS        250             /* 5 s per step                      */

#define C_SO_RCVTIMEO   0x1006
#define C_EWOULDBLOCK   35

typedef struct CpTimeval
{
    ULONG tv_secs;
    ULONG tv_micro;
} CpTimeval;

#define CP_SERVER       1
#define CP_READER       2
#define CP_PROBE        3

typedef struct CtPool
{
    UWORD           cp_Role;
    volatile UWORD  cp_Done;
    volatile LONG   cp_Result;          /* 0 clean, else the failing errno   */
    struct Library *cp_Base;            /* opened by another Process, or NULL */
} CtPool;

static CtPool          cp_server;
static CtPool          cp_reader[CP_READERS];
static CtPool          cp_probe;
static volatile UWORD  cp_listening;
static volatile UWORD  cp_accepted;
static volatile UWORD  cp_connected;
static volatile UWORD  cp_release;

static VOID cp_server_body(struct Library *base)
{
    LONG   ls;
    LONG   cs[CP_READERS];
    UWORD  n = 0;
    LONG   one = 1;
    CtAddr sa;

    ls = c_socket(base, C_AF_INET, C_SOCK_STREAM, 0);
    if (ls < 0)
    {
        cp_server.cp_Result = c_errno(base);
        return;
    }

    (VOID)c_setsockopt(base, ls, C_SOL_SOCKET, C_SO_REUSEADDR,
                       (APTR)&one, (LONG)sizeof(one));

    sa.sin_len    = (UBYTE)sizeof(sa);
    sa.sin_family = C_AF_INET;
    sa.sin_port   = CP_PORT;
    sa.sin_addr   = 0x7F000001UL;

    if (c_bind(base, ls, &sa) < 0 || c_listen(base, ls, CP_READERS) < 0)
    {
        cp_server.cp_Result = c_errno(base);
        (VOID)c_close(base, ls);
        return;
    }

    /* Half a second at a time, so a run whose readers never all arrive
       still reaches the release below and ends with a verdict. */
    {
        CpTimeval tv;

        tv.tv_secs  = 0;
        tv.tv_micro = 500000UL;
        (VOID)c_setsockopt(base, ls, C_SOL_SOCKET, C_SO_RCVTIMEO,
                           (APTR)&tv, (LONG)sizeof(tv));
    }

    cp_listening = 1U;

    while (n < CP_READERS && cp_release == 0U)
    {
        LONG s = c_accept(base, ls);

        if (s < 0)
        {
            if (c_errno(base) == C_EWOULDBLOCK)
                continue;
            cp_server.cp_Result = c_errno(base);
            break;
        }
        cs[n++] = s;
        cp_accepted = n;
    }

    /* Sends nothing: every reader stays blocked until this lets go. */
    while (cp_release == 0U)
        Delay(5);

    while (n > 0)
        (VOID)c_close(base, cs[--n]);
    (VOID)c_close(base, ls);
}

static VOID cp_reader_body(CtPool *p, struct Library *base)
{
    LONG   s;
    UBYTE  byte;
    CtAddr sa;

    s = c_socket(base, C_AF_INET, C_SOCK_STREAM, 0);
    if (s < 0)
    {
        p->cp_Result = c_errno(base);
        return;
    }

    sa.sin_len    = (UBYTE)sizeof(sa);
    sa.sin_family = C_AF_INET;
    sa.sin_port   = CP_PORT;
    sa.sin_addr   = 0x7F000001UL;

    if (c_connect(base, s, &sa) < 0)
    {
        p->cp_Result = c_errno(base);
        (VOID)c_close(base, s);
        return;
    }

    Forbid();
    cp_connected++;
    Permit();

    /* Blocks: the server sends nothing.  The end of the stream is 0. */
    if (c_recv(base, s, (APTR)&byte, 1) != 0)
        p->cp_Result = -1;

    (VOID)c_close(base, s);
}

static VOID cp_entry(VOID)
{
    struct Process *me = (struct Process *)FindTask((STRPTR)0);
    struct Library *base;
    CtPool         *p;

    Wait(SIGF_SINGLE);

    p = (CtPool *)me->pr_Task.tc_UserData;

    base = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4UL);
    if (base == NULL)
    {
        p->cp_Result = -2;
        p->cp_Done   = 1U;
        return;
    }

    if (p->cp_Role == CP_SERVER)
        cp_server_body(base);
    else if (p->cp_Role == CP_READER)
        cp_reader_body(p, base);
    else
    {
        LONG s = c_socket(base, C_AF_INET, C_SOCK_STREAM, 0);

        if (s < 0)
            p->cp_Result = c_errno(base);
        else
            (VOID)c_close(base, s);
    }

    CloseLibrary(base);
    p->cp_Done = 1U;
}

static BOOL cp_spawn(CtPool *p, UWORD role)
{
    struct Process *proc;

    p->cp_Role   = role;
    p->cp_Done   = 0U;
    p->cp_Result = 0;

    Forbid();
    proc = CreateNewProcTags(NP_Entry,     (ULONG)cp_entry,
                             NP_Name,      (ULONG)"anxd-conc-pool",
                             NP_Priority,  (ULONG)0,
                             NP_StackSize, CP_STACK,
                             NP_Cli,       (ULONG)FALSE,
                             TAG_DONE);
    if (proc != NULL)
        proc->pr_Task.tc_UserData = (APTR)p;
    Permit();

    if (proc == NULL)
    {
        p->cp_Done = 1U;
        return FALSE;
    }

    Signal(&proc->pr_Task, SIGF_SINGLE);
    return TRUE;
}

/* Ticks until `*count` reaches `want`, or CP_TICKS. */
static ULONG cp_wait(volatile UWORD *count, UWORD want)
{
    ULONG t = 0;

    while (*count < want && t < CP_TICKS)
    {
        Delay(5);
        t += 5;
    }
    return t;
}

static VOID ct_pool_phase(VOID)
{
    UWORD i;
    UWORD spawned = 0;
    UWORD done;
    ULONG t;

    ct_log("concurrent: %ld readers blocked in recv(), then one more caller\n",
           (LONG)CP_READERS);
    ct_trace("pool: start");

    cp_listening = cp_accepted = cp_connected = cp_release = 0U;

    ct_check(cp_spawn(&cp_server, CP_SERVER), "pool: spawn server", 0);
    (VOID)cp_wait(&cp_listening, 1U);
    ct_check(cp_listening != 0U, "pool: server listens", cp_server.cp_Result);

    for (i = 0; i < CP_READERS; i++)
        if (cp_spawn(&cp_reader[i], CP_READER))
            spawned++;
    ct_check(spawned == CP_READERS, "pool: spawn readers", (LONG)spawned);

    (VOID)cp_wait(&cp_accepted, CP_READERS);
    (VOID)cp_wait(&cp_connected, CP_READERS);
    ct_trace("pool: %ld accepted, %ld connected", (LONG)cp_accepted,
             (LONG)cp_connected);
    ct_check(cp_accepted == CP_READERS && cp_connected == CP_READERS,
             "pool: every reader connects while the others wait in recv()",
             (LONG)cp_connected);

    /* Let the last of them reach recv(). */
    Delay(50);

    ct_check(cp_spawn(&cp_probe, CP_PROBE), "pool: spawn probe", 0);
    t = cp_wait(&cp_probe.cp_Done, 1U);
    ct_trace("pool: probe %s after %ld ticks",
             (LONG)(cp_probe.cp_Done ? "done" : "STUCK"), (LONG)t);
    ct_check(cp_probe.cp_Done != 0U && cp_probe.cp_Result == 0,
             "pool: another program makes a socket while they wait", (LONG)t);

    cp_release = 1U;

    /* Every Process must be gone before the segment is: wait for all of
       them, however long, as the phases above do. */
    t = 0;
    for (;;)
    {
        done = (UWORD)(cp_server.cp_Done + cp_probe.cp_Done);
        for (i = 0; i < CP_READERS; i++)
            done = (UWORD)(done + cp_reader[i].cp_Done);

        if (done >= (UWORD)(CP_READERS + 2))
            break;

        if (t == (ULONG)CP_TICKS)
            ct_check(0, "pool: everything ends once the server closes",
                     (LONG)done);
        Delay(5);
        t += 5;
    }

    for (i = 0; i < CP_READERS; i++)
        ct_check(cp_reader[i].cp_Result == 0, "pool: reader sees the end",
                 cp_reader[i].cp_Result);
    ct_check(cp_server.cp_Result == 0, "pool: server clean",
             cp_server.cp_Result);
    ct_trace("pool: done");
}

/* ------------------------ a caller waiting for a slot hears its break -- */

/*
 * #383.  A full adoption pool parks the next caller in the port
 * (tx_amiga_pool.c) until a slot comes back.  The park waited on its private
 * signal only, so a program stopped with Ctrl-C there -- IBrowse quitting
 * with its transfers inside, NetShutdown's notify -- stayed until another
 * program let a slot go.
 *
 * Fifteen readers block in recv() with a receive timeout, each on a base this
 * Process opened for it.  Only a base's opener gives its slot back while it
 * waits (the park signals the opener, select.c), so these hold a slot for as
 * long as they wait.  A probe then makes a socket and waits for a slot.
 * Ctrl-C to the probe must end its call with EINTR.  Ctrl-C to the readers
 * then ends theirs, and the server closes.
 */
#define CB_READERS      15
#define CB_PORT         7441            /* and 7442: a backlog holds 8       */
#define CB_TIMEOUT_SECS 60

#define C_EINTR         4

static CtPool          cb_server;
static CtPool          cb_reader[CB_READERS];
static CtPool          cb_probe;
static struct Task    *cb_task[CB_READERS + 1];
static volatile UWORD  cb_listening;
static volatile UWORD  cb_connected;
static volatile UWORD  cb_release;

static VOID cb_server_body(struct Library *base)
{
    LONG   ls[2];
    LONG   one = 1;
    UWORD  i;
    CtAddr sa;

    ls[0] = ls[1] = -1;

    for (i = 0; i < 2; i++)
    {
        ls[i] = c_socket(base, C_AF_INET, C_SOCK_STREAM, 0);
        if (ls[i] < 0)
        {
            cb_server.cp_Result = c_errno(base);
            break;
        }

        (VOID)c_setsockopt(base, ls[i], C_SOL_SOCKET, C_SO_REUSEADDR,
                           (APTR)&one, (LONG)sizeof(one));

        sa.sin_len    = (UBYTE)sizeof(sa);
        sa.sin_family = C_AF_INET;
        sa.sin_port   = (UWORD)(CB_PORT + i);
        sa.sin_addr   = 0x7F000001UL;

        if (c_bind(base, ls[i], &sa) < 0 || c_listen(base, ls[i], 8) < 0)
        {
            cb_server.cp_Result = c_errno(base);
            break;
        }
    }

    if (cb_server.cp_Result == 0)
    {
        cb_listening = 1U;

        /* Never accepts: the handshakes complete in the queues and every
           reader waits in recv() on a connection nobody serves. */
        while (cb_release == 0U)
            Delay(5);
    }

    for (i = 0; i < 2; i++)
        if (ls[i] >= 0)
            (VOID)c_close(base, ls[i]);
}

static VOID cb_reader_body(CtPool *p, struct Library *base, UWORD port)
{
    LONG      s;
    UBYTE     byte;
    CtAddr    sa;
    CpTimeval tv;

    s = c_socket(base, C_AF_INET, C_SOCK_STREAM, 0);
    if (s < 0)
    {
        p->cp_Result = c_errno(base);
        return;
    }

    tv.tv_secs  = CB_TIMEOUT_SECS;
    tv.tv_micro = 0;
    (VOID)c_setsockopt(base, s, C_SOL_SOCKET, C_SO_RCVTIMEO,
                       (APTR)&tv, (LONG)sizeof(tv));

    sa.sin_len    = (UBYTE)sizeof(sa);
    sa.sin_family = C_AF_INET;
    sa.sin_port   = port;
    sa.sin_addr   = 0x7F000001UL;

    if (c_connect(base, s, &sa) < 0)
    {
        p->cp_Result = c_errno(base);
        (VOID)c_close(base, s);
        return;
    }

    Forbid();
    cb_connected++;
    Permit();

    /* Blocks until the break below. */
    if (c_recv(base, s, (APTR)&byte, 1) >= 0 || c_errno(base) != C_EINTR)
        p->cp_Result = -1;

    (VOID)c_close(base, s);
}

static VOID cb_entry(VOID)
{
    struct Process *me = (struct Process *)FindTask((STRPTR)0);
    struct Library *base;
    CtPool         *p;

    Wait(SIGF_SINGLE);

    p = (CtPool *)me->pr_Task.tc_UserData;

    base = (p->cp_Base != NULL)
               ? p->cp_Base
               : OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4UL);
    if (base == NULL)
    {
        p->cp_Result = -2;
        p->cp_Done   = 1U;
        return;
    }

    if (p->cp_Role == CP_SERVER)
        cb_server_body(base);
    else if (p->cp_Role == CP_READER)
        cb_reader_body(p, base, (UWORD)(CB_PORT + ((p - cb_reader) & 1)));
    else
    {
        LONG s = c_socket(base, C_AF_INET, C_SOCK_STREAM, 0);

        if (s < 0)
            p->cp_Result = c_errno(base);
        else
            (VOID)c_close(base, s);
    }

    if (p->cp_Base == NULL)
        CloseLibrary(base);
    p->cp_Done = 1U;
}

static struct Task *cb_spawn(CtPool *p, UWORD role)
{
    struct Process *proc;

    p->cp_Role   = role;
    p->cp_Done   = 0U;
    p->cp_Result = 0;

    Forbid();
    proc = CreateNewProcTags(NP_Entry,     (ULONG)cb_entry,
                             NP_Name,      (ULONG)"anxd-conc-break",
                             NP_Priority,  (ULONG)0,
                             NP_StackSize, CP_STACK,
                             NP_Cli,       (ULONG)FALSE,
                             TAG_DONE);
    if (proc != NULL)
        proc->pr_Task.tc_UserData = (APTR)p;
    Permit();

    if (proc == NULL)
    {
        p->cp_Done = 1U;
        return NULL;
    }

    Signal(&proc->pr_Task, SIGF_SINGLE);
    return &proc->pr_Task;
}

/* Ctrl-C to a Task that is still running.  Forbid(): a finished one may be
   gone, and cp_Done is set as its last act. */
static VOID cb_break(CtPool *p, struct Task *task)
{
    Forbid();
    if (task != NULL && p->cp_Done == 0U)
        Signal(task, SIGBREAKF_CTRL_C);
    Permit();
}

static VOID ct_pool_break_phase(VOID)
{
    UWORD i;
    UWORD spawned = 0;
    UWORD done;
    ULONG t;

    ct_log("concurrent: %ld readers hold every slot, a waiting caller "
           "gets Ctrl-C\n", (LONG)CB_READERS);
    ct_trace("break: start");

    cb_listening = cb_connected = cb_release = 0U;

    ct_check(cb_spawn(&cb_server, CP_SERVER) != NULL, "break: spawn server", 0);
    (VOID)cp_wait(&cb_listening, 1U);
    ct_check(cb_listening != 0U, "break: server listens", cb_server.cp_Result);

    for (i = 0; i < CB_READERS; i++)
    {
        cb_reader[i].cp_Base = OpenLibrary((CONST_STRPTR)"bsdsocket.library",
                                           4UL);
        cb_task[i] = cb_spawn(&cb_reader[i], CP_READER);
        if (cb_task[i] != NULL)
            spawned++;
    }
    ct_check(spawned == CB_READERS, "break: spawn readers", (LONG)spawned);

    (VOID)cp_wait(&cb_connected, CB_READERS);
    ct_trace("break: %ld connected", (LONG)cb_connected);
    ct_check(cb_connected == CB_READERS, "break: every reader connects",
             (LONG)cb_connected);

    /* Let the last of them reach recv(). */
    Delay(50);

    cb_task[CB_READERS] = cb_spawn(&cb_probe, CP_PROBE);
    ct_check(cb_task[CB_READERS] != NULL, "break: spawn probe", 0);
    Delay(50);
    ct_check(cb_probe.cp_Done == 0U, "break: the probe waits for a slot",
             cb_probe.cp_Result);

    cb_break(&cb_probe, cb_task[CB_READERS]);
    t = cp_wait(&cb_probe.cp_Done, 1U);
    ct_trace("break: probe %s after %ld ticks, errno %ld",
             (LONG)(cb_probe.cp_Done ? "done" : "STUCK"), (LONG)t,
             cb_probe.cp_Result);
    ct_check(cb_probe.cp_Done != 0U && cb_probe.cp_Result == C_EINTR,
             "break: Ctrl-C ends the wait for a slot with EINTR",
             cb_probe.cp_Result);

    for (i = 0; i < CB_READERS; i++)
        cb_break(&cb_reader[i], cb_task[i]);

    t = 0;
    for (;;)
    {
        done = cb_probe.cp_Done;
        for (i = 0; i < CB_READERS; i++)
            done = (UWORD)(done + cb_reader[i].cp_Done);

        if (done >= (UWORD)(CB_READERS + 1))
            break;

        if (t == (ULONG)CP_TICKS)
            ct_check(0, "break: every reader ends on Ctrl-C", (LONG)done);
        Delay(5);
        t += 5;
    }

    cb_release = 1U;
    while (cb_server.cp_Done == 0U)
        Delay(5);

    for (i = 0; i < CB_READERS; i++)
        if (cb_reader[i].cp_Base != NULL)
        {
            CloseLibrary(cb_reader[i].cp_Base);
            cb_reader[i].cp_Base = NULL;
        }

    for (i = 0; i < CB_READERS; i++)
        ct_check(cb_reader[i].cp_Result == 0, "break: reader ends with EINTR",
                 cb_reader[i].cp_Result);
    ct_check(cb_server.cp_Result == 0, "break: server clean",
             cb_server.cp_Result);
    ct_trace("break: done");
}

/* ------------------------- every blocking wait gives its slot back -- */

/*
 * #383.  A blocked recv() or accept() gives its adoption slot back while the
 * pool is full (bsd_wait_event()), but connect(), send() on a full window
 * and recv() with a timeout held theirs: fifteen of those were the whole
 * pool, and every other program -- httpd, a browser's next page, a name
 * lookup -- waited with them.
 *
 * Twenty-one workers block: seven connect() to a host that never answers,
 * seven send() to a peer that never reads, seven recv() with an 8 s receive
 * timeout on a peer that never sends.  While they wait, a newcomer opens the
 * library and makes a socket, a client fetches a reply from a loopback
 * server (httpd's part), a name is resolved, and a recv() with a 2 s timeout
 * must end after its 2 s with EWOULDBLOCK.  The timed workers, parked on the
 * base's timer, must end on their 8 s the same way; Ctrl-C ends the rest.
 */
#define CG_EACH         7
#define CG_WORKERS      (3 * CG_EACH)
#define CG_SINK_PORT    7443            /* accepts, never reads or writes   */
#define CG_WEB_PORT     7444            /* answers each connection, closes  */
#define CG_BLACKHOLE    0xC0000201UL    /* 192.0.2.1, TEST-NET-1            */
#define CG_DNS_NAME     "www.example.org"
#define CG_NET_NAME     "www.example.com"   /* the precondition's lookup  */
#define CG_NET_TICKS    1500            /* 30 s for DHCP after the kill phase */
#define CG_PROBE_TICKS  500             /* 10 s for each probe to end       */
#define CG_SERVED_TICKS 100             /* 2 s to be served: it takes 0-15  */
#define CG_DNS_TICKS    250             /* 5 s for a lookup on the Internet */
#define CG_DEADLINE_SECS 2              /* the probe's SO_RCVTIMEO          */
#define CG_TIMED_SECS   8               /* the timed workers', parked       */
/* A deadline is ThreadX's tick, the measurement dos.library's: the two
   drift a little apart in the emulator, so "on time" is -0.1 s to +0.5 s. */
#define CG_EARLY_TICKS  5
#define CG_LATE_TICKS   25

#define C_SO_SNDTIMEO   0x1005

#define CG_CONNECT      1
#define CG_SEND         2
#define CG_TIMED        3
#define CG_SINK         4
#define CG_WEB          5
#define CG_P_NEW        6
#define CG_P_WEB        7
#define CG_P_DNS        8
#define CG_P_DEADLINE   9

typedef struct CtGive
{
    UWORD           cg_Role;
    volatile UWORD  cg_Ready;           /* about to block                   */
    volatile UWORD  cg_Done;
    volatile LONG   cg_Result;
    volatile LONG   cg_Errno;
    volatile ULONG  cg_Ticks;           /* how long the call under test took */
    struct Task    *cg_Task;
} CtGive;

static CtGive          cg_worker[CG_WORKERS];
static CtGive          cg_sink;
static CtGive          cg_web;
static CtGive          cg_probe[4];
static volatile UWORD  cg_listening;
static volatile UWORD  cg_release;
static UBYTE           cg_buf[4096];

static ULONG cg_now(VOID)
{
    struct DateStamp ds;

    DateStamp(&ds);
    return (ULONG)ds.ds_Minute * 3000UL + (ULONG)ds.ds_Tick;
}

static LONG cg_dial(struct Library *base, UWORD port, ULONG addr, ULONG secs)
{
    LONG      s = c_socket(base, C_AF_INET, C_SOCK_STREAM, 0);
    CtAddr    sa;
    CpTimeval tv;

    if (s < 0)
        return -1;

    if (secs != 0)
    {
        tv.tv_secs  = secs;
        tv.tv_micro = 0;
        (VOID)c_setsockopt(base, s, C_SOL_SOCKET, C_SO_SNDTIMEO, (APTR)&tv,
                           (LONG)sizeof(tv));
    }

    sa.sin_len    = (UBYTE)sizeof(sa);
    sa.sin_family = C_AF_INET;
    sa.sin_port   = port;
    sa.sin_addr   = addr;

    if (c_connect(base, s, &sa) < 0)
    {
        LONG e = c_errno(base);

        (VOID)c_close(base, s);
        return -2 - e;
    }
    return s;
}

/* A listener on loopback `port`: accepts, and either keeps every connection
   silent (the sink) or answers each with a line and closes it (the web). */
static VOID cg_server_body(CtGive *g, struct Library *base, UWORD port)
{
    LONG      ls, cs[CG_WORKERS + 4];
    UWORD     n = 0;
    LONG      one = 1;
    CtAddr    sa;
    CpTimeval tv;

    ls = c_socket(base, C_AF_INET, C_SOCK_STREAM, 0);
    if (ls < 0)
    {
        g->cg_Result = c_errno(base);
        return;
    }
    (VOID)c_setsockopt(base, ls, C_SOL_SOCKET, C_SO_REUSEADDR, (APTR)&one,
                       (LONG)sizeof(one));

    sa.sin_len    = (UBYTE)sizeof(sa);
    sa.sin_family = C_AF_INET;
    sa.sin_port   = port;
    sa.sin_addr   = 0x7F000001UL;

    if (c_bind(base, ls, &sa) < 0 || c_listen(base, ls, 16) < 0)
    {
        g->cg_Result = c_errno(base);
        (VOID)c_close(base, ls);
        return;
    }

    tv.tv_secs  = 0;
    tv.tv_micro = 500000UL;
    (VOID)c_setsockopt(base, ls, C_SOL_SOCKET, C_SO_RCVTIMEO, (APTR)&tv,
                       (LONG)sizeof(tv));

    Forbid();
    cg_listening++;
    Permit();

    while (cg_release == 0U)
    {
        LONG s = c_accept(base, ls);

        if (s < 0)
        {
            if (c_errno(base) == C_EWOULDBLOCK)
                continue;
            g->cg_Result = c_errno(base);
            break;
        }

        if (g->cg_Role == CG_WEB)
        {
            static const char ok[] = "HTTP/1.0 200 OK\r\n\r\nok\n";

            (VOID)c_send(base, s, (APTR)ok, (LONG)sizeof(ok) - 1);
            (VOID)c_close(base, s);
            g->cg_Ticks++;
        }
        else if (n < (UWORD)(sizeof(cs) / sizeof(cs[0])))
            cs[n++] = s;
        else
            (VOID)c_close(base, s);
    }

    while (n > 0)
        (VOID)c_close(base, cs[--n]);
    (VOID)c_close(base, ls);
}

static VOID cg_worker_body(CtGive *g, struct Library *base)
{
    LONG  s;
    ULONG t0;

    switch (g->cg_Role)
    {
        case CG_CONNECT:
            g->cg_Ready = 1U;
            s = cg_dial(base, 80, CG_BLACKHOLE, 60);
            g->cg_Result = (s >= 0) ? 0 : -1;
            g->cg_Errno  = (s >= 0) ? 0 : -2 - s;
            if (s >= 0)
                (VOID)c_close(base, s);
            return;

        case CG_SEND:
            s = cg_dial(base, CG_SINK_PORT, 0x7F000001UL, 0);
            if (s < 0)
            {
                g->cg_Result = -1;
                g->cg_Errno  = -2 - s;
                return;
            }
            g->cg_Ready = 1U;
            for (;;)
            {
                LONG n = c_send(base, s, (APTR)cg_buf, (LONG)sizeof(cg_buf));

                if (n < 0)
                {
                    g->cg_Result = -1;
                    g->cg_Errno  = c_errno(base);
                    break;
                }
                /* A short count is the break arriving mid-write. */
                if (n < (LONG)sizeof(cg_buf) && cg_release != 0U)
                {
                    g->cg_Result = -1;
                    g->cg_Errno  = C_EINTR;
                    break;
                }
            }
            (VOID)c_close(base, s);
            return;

        case CG_TIMED:
        case CG_P_DEADLINE:
        {
            CpTimeval tv;
            UBYTE     byte;

            s = cg_dial(base, CG_SINK_PORT, 0x7F000001UL, 0);
            if (s < 0)
            {
                g->cg_Result = -1;
                g->cg_Errno  = -2 - s;
                return;
            }
            tv.tv_secs  = (g->cg_Role == CG_TIMED) ? CG_TIMED_SECS
                                                   : CG_DEADLINE_SECS;
            tv.tv_micro = 0;
            (VOID)c_setsockopt(base, s, C_SOL_SOCKET, C_SO_RCVTIMEO, (APTR)&tv,
                               (LONG)sizeof(tv));
            g->cg_Ready = 1U;
            t0 = cg_now();
            g->cg_Result = c_recv(base, s, (APTR)&byte, 1);
            g->cg_Ticks  = cg_now() - t0;
            if (g->cg_Result < 0)
                g->cg_Errno = c_errno(base);
            (VOID)c_close(base, s);
            return;
        }

        case CG_P_NEW:
            t0 = cg_now();
            s = c_socket(base, C_AF_INET, C_SOCK_STREAM, 0);
            g->cg_Ticks = cg_now() - t0;
            g->cg_Result = (s >= 0) ? 0 : -1;
            if (s < 0)
                g->cg_Errno = c_errno(base);
            else
                (VOID)c_close(base, s);
            return;

        case CG_P_WEB:
        {
            static const char get[] = "GET / HTTP/1.0\r\n\r\n";
            UBYTE reply[64];
            LONG  got = 0, n;

            t0 = cg_now();
            s = cg_dial(base, CG_WEB_PORT, 0x7F000001UL, 0);
            if (s < 0)
            {
                g->cg_Result = -1;
                g->cg_Errno  = -2 - s;
                return;
            }
            (VOID)c_send(base, s, (APTR)get, (LONG)sizeof(get) - 1);
            while ((n = c_recv(base, s, (APTR)reply, (LONG)sizeof(reply))) > 0)
                got += n;
            g->cg_Ticks  = cg_now() - t0;
            g->cg_Result = (n == 0 && got > 0) ? 0 : -1;
            if (n < 0)
                g->cg_Errno = c_errno(base);
            (VOID)c_close(base, s);
            return;
        }

        case CG_P_DNS:
            t0 = cg_now();
            g->cg_Result = (c_gethostbyname(base, CG_DNS_NAME) != NULL) ? 0 : -1;
            g->cg_Ticks  = cg_now() - t0;
            if (g->cg_Result != 0)
                g->cg_Errno = c_errno(base);
            return;

        default:
            return;
    }
}

static VOID cg_entry(VOID)
{
    struct Process *me = (struct Process *)FindTask((STRPTR)0);
    struct Library *base;
    CtGive         *g;

    Wait(SIGF_SINGLE);

    g = (CtGive *)me->pr_Task.tc_UserData;

    base = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4UL);
    if (base == NULL)
    {
        g->cg_Result = -2;
        g->cg_Done   = 1U;
        return;
    }

    if (g->cg_Role == CG_SINK)
        cg_server_body(g, base, CG_SINK_PORT);
    else if (g->cg_Role == CG_WEB)
        cg_server_body(g, base, CG_WEB_PORT);
    else
        cg_worker_body(g, base);

    CloseLibrary(base);
    g->cg_Done = 1U;
}

static BOOL cg_spawn(CtGive *g, UWORD role, const char *name)
{
    struct Process *proc;

    g->cg_Role   = role;
    g->cg_Ready  = 0U;
    g->cg_Done   = 0U;
    g->cg_Result = 0;
    g->cg_Errno  = 0;
    g->cg_Ticks  = 0;
    g->cg_Task   = NULL;

    Forbid();
    proc = CreateNewProcTags(NP_Entry,     (ULONG)cg_entry,
                             NP_Name,      (ULONG)name,
                             NP_Priority,  (ULONG)0,
                             NP_StackSize, CP_STACK,
                             NP_Cli,       (ULONG)FALSE,
                             TAG_DONE);
    if (proc != NULL)
    {
        proc->pr_Task.tc_UserData = (APTR)g;
        g->cg_Task = &proc->pr_Task;
    }
    Permit();

    if (proc == NULL)
    {
        g->cg_Done = 1U;
        return FALSE;
    }

    Signal(&proc->pr_Task, SIGF_SINGLE);
    return TRUE;
}

static VOID cg_break(CtGive *g)
{
    Forbid();
    if (g->cg_Task != NULL && g->cg_Done == 0U)
        Signal(g->cg_Task, SIGBREAKF_CTRL_C);
    Permit();
}

static UWORD cg_count(UWORD ready)
{
    UWORD i, n = 0;

    for (i = 0; i < CG_WORKERS; i++)
        if ((ready ? cg_worker[i].cg_Ready : cg_worker[i].cg_Done) != 0U)
            n++;
    return n;
}

/* One probe, waited for: TRUE when it ended inside the deadline. */
static BOOL cg_run_probe(CtGive *g, UWORD role, const char *name, ULONG *ticks)
{
    ULONG t;

    if (!cg_spawn(g, role, name))
    {
        *ticks = 0;
        return FALSE;
    }
    t = 0;
    while (g->cg_Done == 0U && t < (ULONG)CG_PROBE_TICKS)
    {
        Delay(5);
        t += 5;
    }
    *ticks = t;
    return (BOOL)(g->cg_Done != 0U);
}

static VOID ct_giveback_phase(struct Library *base)
{
    static const char *const role_name[] = { "", "connect", "send", "timed" };
    UWORD i, blocked;
    ULONG t, tn, tw, td, tt;
    BOOL  ok;

    ct_log("concurrent: %ld workers blocked in connect/send/timed recv, then "
           "newcomers\n", (LONG)CG_WORKERS);
    ct_trace("give: start");

    /* The kill phase took the interface down.  Up again, and a lease with a
       name server, or the connects and the lookup test nothing. */
    ct_interface_up(base);
    for (t = 0; t < (ULONG)CG_NET_TICKS; t += 25)
    {
        if (c_gethostbyname(base, CG_NET_NAME) != NULL)
            break;
        Delay(25);
    }
    ct_trace("give: network up after %ld ticks", (LONG)t);
    if (t >= (ULONG)CG_NET_TICKS)
    {
        ct_check(0, "give: UNMET PRECONDITION, no lease with a name server",
                 (LONG)t);
        return;
    }

    cg_listening = cg_release = 0U;

    ct_check(cg_spawn(&cg_sink, CG_SINK, "anxd-give-sink") &&
             cg_spawn(&cg_web, CG_WEB, "anxd-give-web"),
             "give: spawn the servers", 0);
    (VOID)cp_wait(&cg_listening, 2U);
    ct_check(cg_listening == 2U, "give: both servers listen",
             cg_sink.cg_Result != 0 ? cg_sink.cg_Result : cg_web.cg_Result);

    for (i = 0; i < CG_WORKERS; i++)
        ct_check(cg_spawn(&cg_worker[i], (UWORD)(CG_CONNECT + i / CG_EACH),
                          "anxd-give-worker"),
                 "give: spawn a worker", (LONG)i);

    t = 0;
    while (cg_count(1) < CG_WORKERS && t < (ULONG)CP_TICKS)
    {
        Delay(5);
        t += 5;
    }
    /* Into their calls: the sends fill their windows, the connects retry. */
    Delay(150);

    blocked = 0;
    for (i = 0; i < CG_WORKERS; i++)
        if (cg_worker[i].cg_Ready != 0U && cg_worker[i].cg_Done == 0U)
            blocked++;
        else
            ct_trace("give: worker %ld (%s) ready=%ld done=%ld errno %ld",
                     (LONG)i, (LONG)role_name[cg_worker[i].cg_Role],
                     (LONG)cg_worker[i].cg_Ready, (LONG)cg_worker[i].cg_Done,
                     cg_worker[i].cg_Errno);
    ct_trace("give: %ld of %ld blocked", (LONG)blocked, (LONG)CG_WORKERS);
    ct_check(blocked >= 16, "give: more workers blocked than there are slots",
             (LONG)blocked);

    ok = cg_run_probe(&cg_probe[0], CG_P_NEW, "anxd-give-new", &tn);
    ct_check(ok && cg_probe[0].cg_Result == 0 && tn <= (ULONG)CG_SERVED_TICKS,
             "give: a newcomer makes a socket while they wait", (LONG)tn);

    ok = cg_run_probe(&cg_probe[1], CG_P_WEB, "anxd-give-http", &tw);
    ct_check(ok && cg_probe[1].cg_Result == 0 && tw <= (ULONG)CG_SERVED_TICKS,
             "give: a loopback server still answers", (LONG)tw);

    ok = cg_run_probe(&cg_probe[2], CG_P_DNS, "anxd-give-dns", &td);
    ct_check(ok && cg_probe[2].cg_Result == 0 && td <= (ULONG)CG_DNS_TICKS,
             "give: a name is still resolved", (LONG)td);

    ok = cg_run_probe(&cg_probe[3], CG_P_DEADLINE, "anxd-give-rcvtimeo", &tt);
    ct_check(ok && cg_probe[3].cg_Result < 0 &&
             cg_probe[3].cg_Errno == C_EWOULDBLOCK,
             "give: SO_RCVTIMEO 2 s ends with EWOULDBLOCK",
             cg_probe[3].cg_Errno);
    ct_check(cg_probe[3].cg_Ticks + CG_EARLY_TICKS >=
                 (ULONG)(CG_DEADLINE_SECS * 50) &&
             cg_probe[3].cg_Ticks <=
                 (ULONG)(CG_DEADLINE_SECS * 50 + CG_LATE_TICKS),
             "give: and after its 2 s", (LONG)cg_probe[3].cg_Ticks);

    ct_log("concurrent: give_blocked=%ld give_new_ticks=%ld give_http_ticks=%ld "
           "give_dns_ticks=%ld give_rcvtimeo_ticks=%ld\n", (LONG)blocked,
           (LONG)cg_probe[0].cg_Ticks, (LONG)cg_probe[1].cg_Ticks,
           (LONG)cg_probe[2].cg_Ticks, (LONG)cg_probe[3].cg_Ticks);

    /* The timed workers end on their own deadline. */
    t = 0;
    for (;;)
    {
        UWORD timed_done = 0;

        for (i = 2 * CG_EACH; i < CG_WORKERS; i++)
            if (cg_worker[i].cg_Done != 0U)
                timed_done++;
        if (timed_done == CG_EACH || t >= (ULONG)(CG_TIMED_SECS * 50 * 2))
            break;
        Delay(5);
        t += 5;
    }
    for (i = 2 * CG_EACH; i < CG_WORKERS; i++)
    {
        CtGive *g = &cg_worker[i];

        ct_check(g->cg_Done != 0U && g->cg_Result < 0 &&
                 g->cg_Errno == C_EWOULDBLOCK,
                 "give: a parked SO_RCVTIMEO ends with EWOULDBLOCK",
                 g->cg_Errno);
        ct_check(g->cg_Ticks + CG_EARLY_TICKS >= (ULONG)(CG_TIMED_SECS * 50) &&
                 g->cg_Ticks <= (ULONG)(CG_TIMED_SECS * 50 + CG_LATE_TICKS),
                 "give: on its 8 s", (LONG)g->cg_Ticks);
    }
    ct_log("concurrent: give_timed_ticks=%ld..%ld\n",
           (LONG)cg_worker[2 * CG_EACH].cg_Ticks,
           (LONG)cg_worker[CG_WORKERS - 1].cg_Ticks);

    /* Ctrl-C: every other worker ends its call with EINTR. */
    cg_release = 1U;
    for (i = 0; i < 2 * CG_EACH; i++)
        cg_break(&cg_worker[i]);

    t = 0;
    while (cg_count(0) < CG_WORKERS && t < (ULONG)CP_TICKS)
    {
        Delay(5);
        t += 5;
    }
    ct_trace("give: %ld of %ld ended %ld ticks after Ctrl-C", (LONG)cg_count(0),
             (LONG)CG_WORKERS, (LONG)t);
    ct_check(cg_count(0) == CG_WORKERS, "give: Ctrl-C ends every worker",
             (LONG)cg_count(0));

    for (i = 0; i < 2 * CG_EACH; i++)
    {
        if (cg_worker[i].cg_Errno != C_EINTR)
            ct_trace("give: worker %ld (%s) result %ld errno %ld", (LONG)i,
                     (LONG)role_name[cg_worker[i].cg_Role],
                     cg_worker[i].cg_Result, cg_worker[i].cg_Errno);
        ct_check(cg_worker[i].cg_Result < 0 && cg_worker[i].cg_Errno == C_EINTR,
                 "give: a worker ends with EINTR", cg_worker[i].cg_Errno);
    }

    /* Every Process must be gone before the segment is. */
    for (;;)
    {
        UWORD done = (UWORD)(cg_count(0) + cg_sink.cg_Done + cg_web.cg_Done);

        for (i = 0; i < 4; i++)
            done = (UWORD)(done + cg_probe[i].cg_Done);
        if (done >= (UWORD)(CG_WORKERS + 6))
            break;
        Delay(5);
    }
    ct_check(cg_sink.cg_Result == 0 && cg_web.cg_Result == 0,
             "give: servers clean",
             cg_sink.cg_Result != 0 ? cg_sink.cg_Result : cg_web.cg_Result);
    ct_trace("give: done");
}

/* ----------------- a Task removed in a timed WaitSelect() is not hit -- */

/*
 * #383.  The base's timer.device request stays out between waits, and its
 * reply Signal()ed the opener.  A program that RemTask()s another blocked in
 * a timed WaitSelect() -- IBrowse ending its network processes -- left the
 * request armed, and its reply wrote a byte of tc_SigRecvd into the freed
 * Task.
 *
 * Eight victims loop in WaitSelect() with a 1 s timeout on a connection that
 * stays silent.  Each is removed, and the memory Exec frees with it is taken
 * back at once and filled with a pattern.  Three seconds later -- every
 * request has fired by then -- not a byte of it may have changed.
 */
#define CR_VICTIMS      8
#define CR_REGIONS      4
/* Zero, so a Signal() into the freed Task changes the byte whatever bit it
   sets. */
#define CR_PATTERN      0x00U

typedef struct CtRegion
{
    UBYTE  *cr_Addr;
    ULONG   cr_Len;
    BOOL    cr_Claimed;
} CtRegion;

typedef struct CtVictim
{
    volatile UWORD  cv_Ready;
    volatile UWORD  cv_Done;
    struct Task    *cv_Task;
    CtRegion        cv_Region[CR_REGIONS];
    UWORD           cv_Regions;
} CtVictim;

static CtVictim cr_victim[CR_VICTIMS];

static VOID cr_entry(VOID)
{
    struct Process *me = (struct Process *)FindTask((STRPTR)0);
    struct Library *base;
    CtVictim       *v;
    LONG            s;

    Wait(SIGF_SINGLE);
    v = (CtVictim *)me->pr_Task.tc_UserData;

    base = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4UL);
    if (base == NULL)
    {
        v->cv_Done = 1U;
        return;
    }

    s = cg_dial(base, CG_SINK_PORT, 0x7F000001UL, 0);
    if (s >= 0)
    {
        for (;;)
        {
            ULONG     rd[2];
            CpTimeval tv;
            ULONG     sigs = 0;

            rd[0] = rd[1] = 0;
            rd[s / 32] |= 1UL << (s % 32);
            tv.tv_secs  = 1;
            tv.tv_micro = 0;
            v->cv_Ready = 1U;
            if (c_waitselect(base, s + 1, (APTR)rd, (APTR)&tv, &sigs) != 0)
                break;
        }
        (VOID)c_close(base, s);
    }

    CloseLibrary(base);
    v->cv_Done = 1U;
}

/* Remove the victim and take its memory back, patterned, under one
   Forbid(): nothing can allocate it in between. */
static BOOL cr_remove(CtVictim *v)
{
    struct MemList *ml;
    UWORD           i;
    ULONG           k;

    v->cv_Regions = 0;

    Forbid();
    if (v->cv_Done != 0U || v->cv_Task == NULL)
    {
        Permit();
        return FALSE;
    }

    for (ml = (struct MemList *)v->cv_Task->tc_MemEntry.lh_Head;
         ml->ml_Node.ln_Succ != NULL;
         ml = (struct MemList *)ml->ml_Node.ln_Succ)
        for (i = 0; i < ml->ml_NumEntries && v->cv_Regions < CR_REGIONS; i++)
        {
            v->cv_Region[v->cv_Regions].cr_Addr =
                (UBYTE *)ml->ml_ME[i].me_Addr;
            v->cv_Region[v->cv_Regions].cr_Len  = ml->ml_ME[i].me_Length;
            v->cv_Regions++;
        }

    RemTask(v->cv_Task);

    for (i = 0; i < v->cv_Regions; i++)
    {
        CtRegion *r = &v->cv_Region[i];

        r->cr_Claimed = (AllocAbs(r->cr_Len, (APTR)r->cr_Addr) != NULL);
        if (r->cr_Claimed)
            for (k = 0; k < r->cr_Len; k++)
                r->cr_Addr[k] = CR_PATTERN;
    }
    Permit();

    return TRUE;
}

static ULONG cr_written(CtVictim *v, ULONG *first)
{
    UWORD i;
    ULONG k, bad = 0;

    *first = 0;
    Forbid();
    for (i = 0; i < v->cv_Regions; i++)
    {
        CtRegion *r = &v->cv_Region[i];

        if (!r->cr_Claimed)
            continue;
        for (k = 0; k < r->cr_Len; k++)
            if (r->cr_Addr[k] != CR_PATTERN)
            {
                if (bad == 0)
                    *first = ((ULONG)i << 24) | k;
                bad++;
            }
    }
    Permit();
    return bad;
}

static VOID ct_removed_timer_phase(VOID)
{
    UWORD i, removed = 0, claimed = 0;
    ULONG bad_total = 0;
    ULONG t;

    ct_log("concurrent: %ld Tasks removed in a timed WaitSelect()\n",
           (LONG)CR_VICTIMS);
    ct_trace("removed: start");

    cg_listening = cg_release = 0U;
    ct_check(cg_spawn(&cg_sink, CG_SINK, "anxd-removed-sink"),
             "removed: spawn the server", 0);
    (VOID)cp_wait(&cg_listening, 1U);

    for (i = 0; i < CR_VICTIMS; i++)
    {
        struct Process *proc;
        CtVictim       *v = &cr_victim[i];

        v->cv_Ready = v->cv_Done = 0U;
        Forbid();
        proc = CreateNewProcTags(NP_Entry,     (ULONG)cr_entry,
                                 NP_Name,      (ULONG)"anxd-removed-victim",
                                 NP_Priority,  (ULONG)0,
                                 NP_StackSize, CP_STACK,
                                 NP_Cli,       (ULONG)FALSE,
                                 TAG_DONE);
        if (proc != NULL)
            proc->pr_Task.tc_UserData = (APTR)v;
        v->cv_Task = (proc != NULL) ? &proc->pr_Task : NULL;
        Permit();
        if (proc != NULL)
            Signal(&proc->pr_Task, SIGF_SINGLE);
        else
            v->cv_Done = 1U;
    }

    t = 0;
    for (;;)
    {
        UWORD ready = 0;

        for (i = 0; i < CR_VICTIMS; i++)
            if (cr_victim[i].cv_Ready != 0U)
                ready++;
        if (ready == CR_VICTIMS || t >= (ULONG)CP_TICKS)
            break;
        Delay(5);
        t += 5;
    }
    /* Into the Wait() of a WaitSelect() with its request out. */
    Delay(25);

    for (i = 0; i < CR_VICTIMS; i++)
        if (cr_remove(&cr_victim[i]))
        {
            UWORD k;

            removed++;
            for (k = 0; k < cr_victim[i].cv_Regions; k++)
                if (cr_victim[i].cv_Region[k].cr_Claimed)
                    claimed++;
        }
    ct_check(removed == CR_VICTIMS && claimed >= CR_VICTIMS,
             "removed: every victim removed and its memory taken back",
             (LONG)removed);

    /* Each request was armed for a second at most. */
    Delay(150);

    for (i = 0; i < CR_VICTIMS; i++)
    {
        ULONG first, bad = cr_written(&cr_victim[i], &first);

        if (bad != 0)
            ct_trace("removed: victim %ld written, %ld bytes, region %ld "
                     "offset %ld", (LONG)i, (LONG)bad, (LONG)(first >> 24),
                     (LONG)(first & 0xFFFFFFUL));
        bad_total += bad;
    }
    ct_check(bad_total == 0,
             "removed: nothing writes into a removed Task's memory",
             (LONG)bad_total);
    ct_log("concurrent: removed_victims=%ld removed_claimed=%ld "
           "removed_written=%ld\n", (LONG)removed, (LONG)claimed,
           (LONG)bad_total);

    for (i = 0; i < CR_VICTIMS; i++)
    {
        UWORD k;

        for (k = 0; k < cr_victim[i].cv_Regions; k++)
            if (cr_victim[i].cv_Region[k].cr_Claimed)
            {
                FreeMem((APTR)cr_victim[i].cv_Region[k].cr_Addr,
                        cr_victim[i].cv_Region[k].cr_Len);
                cr_victim[i].cv_Region[k].cr_Claimed = FALSE;
            }
    }

    /* A newcomer still gets in once the sweep has the bases. */
    {
        ULONG tn;
        BOOL  ok = cg_run_probe(&cg_probe[0], CG_P_NEW, "anxd-removed-new",
                                &tn);

        ct_check(ok && cg_probe[0].cg_Result == 0,
                 "removed: a newcomer makes a socket", (LONG)tn);
    }

    cg_release = 1U;
    while (cg_sink.cg_Done == 0U)
        Delay(5);
    ct_trace("removed: done");
}

static VOID ct_main_body(VOID)
{
    struct Library *base;
    UWORD           i;
    ULONG           waited = 0;
    UWORD           live   = 0;

    ct_log("concurrent: %ld applications, %ld pairs, %ld bytes each way\n",
           (LONG)CT_APPS, (LONG)CT_PAIRS, (LONG)CT_BYTES);
    ct_trace("start: %ld applications, %ld pairs, %ld bytes, %ld KB stacks",
             (LONG)CT_APPS, (LONG)CT_PAIRS, (LONG)CT_BYTES,
             (LONG)(CT_STACK / 1024UL));

    /* Both ends print the arithmetic so a -t that cannot cover the harness's
       own deadlines is visible as a mismatch rather than as a hang. */
    ct_log("concurrent: 2 x %ld s of deadline; needs -t %ld or more\n",
           (LONG)CT_DEADLINE_SECS, (LONG)CT_BUDGET_SECS);

    ct_log("concurrent: opening the already-started stack library\n");
    ct_trace("parent: OpenLibrary(bsdsocket.library)");
    base = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4UL);
    ct_log("concurrent: library base %s\n",
           (LONG)((base != NULL) ? "open" : "NULL"));
    ct_trace("parent: library base %s", (LONG)((base != NULL) ? "open" : "NULL"));
    ct_check(base != NULL, "parent opens bsdsocket.library", 0);
    if (base == NULL)
    {
        ct_log("%ld checks, %ld failures, FAIL\n",
               (LONG)ct_checks, (LONG)ct_failures);
        return;
    }

    for (i = 0; i < CT_APPS; i++)
    {
        CtApp *a = &ct_app[i];

        a->ca_Id       = i;
        a->ca_IsServer = (UWORD)((i < CT_PAIRS) ? 1U : 0U);
        a->ca_Port     = (UWORD)(CT_PORT_BASE + (i % CT_PAIRS));
        a->ca_Started  = 0U;
        a->ca_Done     = 0U;
        a->ca_Failures = 0;
        a->ca_Bytes    = 0;
        a->ca_Errno    = 0;
        a->ca_Where    = 0;
        a->ca_Phase    = CT_P_SPAWNED;
        a->ca_Proc     = NULL;
    }

    /* Servers first, and let them reach listen() before the clients dial: a
       refused connect would be this test failing on its own startup order
       rather than on anything about the stack. */
    ct_log("concurrent: spawning %ld servers\n", (LONG)CT_PAIRS);
    for (i = 0; i < CT_PAIRS; i++)
    {
        ct_check(ct_spawn(&ct_app[i], "anxd-conc-server") != NULL,
                 "spawn server", (LONG)i);
        if (ct_app[i].ca_Proc != NULL)
            live++;
    }

    ct_log("concurrent: waiting for servers to listen\n");
    for (i = 0; i < CT_PAIRS; i++)
    {
        ULONG spin = 0;

        while (ct_app[i].ca_Started == 0U && ct_app[i].ca_Done == 0U
               && spin < CT_TIMEOUT_TICKS)
        {
            Delay(CT_POLL_TICKS);
            spin += CT_POLL_TICKS;
        }
        ct_check(ct_app[i].ca_Started != 0U || ct_app[i].ca_Done != 0U,
                 "server reaches listen()", (LONG)i);
    }
    ct_log("concurrent: servers listening\n");
    ct_trace("parent: servers listening");

    ct_log("concurrent: spawning %ld clients\n", (LONG)CT_PAIRS);
    for (i = CT_PAIRS; i < CT_APPS; i++)
    {
        ct_check(ct_spawn(&ct_app[i], "anxd-conc-client") != NULL,
                 "spawn client", (LONG)i);
        if (ct_app[i].ca_Proc != NULL)
            live++;
    }

    ct_log("concurrent: %ld applications running\n", (LONG)live);
    for (;;)
    {
        UWORD done = 0;

        for (i = 0; i < CT_APPS; i++)
            if (ct_app[i].ca_Done != 0U || ct_app[i].ca_Proc == NULL)
                done++;

        if (done >= CT_APPS)
            break;

        if (waited >= CT_TIMEOUT_TICKS)
        {
            for (i = 0; i < CT_APPS; i++)
            {
                if (ct_app[i].ca_Proc != NULL && ct_app[i].ca_Done == 0U)
                    ct_log("  WEDGED app %ld (%s, %s, %ld bytes, at call %ld)\n",
                           (LONG)i,
                           (LONG)(ct_app[i].ca_IsServer ? "server" : "client"),
                           (LONG)ct_phase_name[ct_app[i].ca_Phase],
                           (LONG)ct_app[i].ca_Bytes,
                           (LONG)ct_app[i].ca_Where);
            }
            ct_check(0, "all applications finished inside the deadline",
                     (LONG)waited);
            break;
        }

        /* A heartbeat rather than silence: this loop is where a wedge lands,
           and the deadline only speaks once at the end. Killed before then,
           the last beat is what says which application stopped where. */
        if ((waited % CT_BEAT_TICKS) == 0UL)
        {
            for (i = 0; i < CT_APPS; i++)
                ct_trace("beat %lds: app %ld %s %ld bytes",
                         (LONG)(waited / 50UL), (LONG)i,
                         (LONG)ct_phase_name[ct_app[i].ca_Phase],
                         (LONG)ct_app[i].ca_Bytes);
        }

        Delay(CT_POLL_TICKS);
        waited += CT_POLL_TICKS;
    }

    for (i = 0; i < CT_APPS; i++)
    {
        CtApp *a = &ct_app[i];

        if (a->ca_Proc == NULL)
            continue;

        ct_check(a->ca_Failures == 0,
                 (a->ca_IsServer != 0U) ? "server clean" : "client clean",
                 a->ca_Errno);

        if (a->ca_IsServer == 0U)
            ct_check(a->ca_Bytes == CT_BYTES, "client echoed every byte",
                     (LONG)a->ca_Bytes);
    }

    ct_kill_phase(base);

    ct_pool_phase();

    ct_pool_break_phase();

    ct_giveback_phase(base);

    ct_removed_timer_phase();

    ct_log("%ld checks, %ld failures, %s\n",
           (LONG)ct_checks, (LONG)ct_failures,
           (LONG)((ct_failures == 0UL) ? "PASS" : "FAIL"));
    ct_trace("%ld checks, %ld failures, %s",
             (LONG)ct_checks, (LONG)ct_failures,
             (LONG)((ct_failures == 0UL) ? "PASS" : "FAIL"));

    {
        UWORD stuck = 0;

        for (i = 0; i < CT_APPS; i++)
            if (ct_app[i].ca_Proc != NULL && ct_app[i].ca_Done == 0U)
                stuck++;

        if (stuck != 0U)
        {
            ct_log("concurrent: %ld application(s) never finished; not exiting\n",
                   (LONG)stuck);
            ct_trace("parked: %ld application(s) never finished", (LONG)stuck);
            for (;;)
            {
                UWORD n = 0;
                for (i = 0; i < CT_APPS; i++)
                    if (ct_app[i].ca_Proc != NULL && ct_app[i].ca_Done == 0U)
                        n++;
                if (n == 0U)
                    break;
                Delay(50);
            }
        }
    }

    ct_trace("parent: CloseLibrary (last base, this stops the stack)");
    CloseLibrary(base);
    ct_trace("parent: stack stopped");
}

static struct Task *ct_launcher;
static volatile UWORD ct_main_done;

static VOID ct_main_entry(VOID)
{
    Wait(SIGF_SINGLE);
    ct_main_body();
    ct_main_done = 1U;
    Signal(ct_launcher, SIGF_SINGLE);
}

int main(int argc, char **argv)
{
    struct Process *p;

    (VOID)argc; (VOID)argv;

    ct_launcher = FindTask((STRPTR)0);

    Forbid();
    p = CreateNewProcTags(NP_Entry,     (ULONG)ct_main_entry,
                          NP_Name,      (ULONG)"anxd-conc-main",
                          NP_Priority,  (ULONG)0,
                          NP_StackSize, CT_STACK,
                          NP_Cli,       (ULONG)FALSE,
                          NP_Output,    (ULONG)Output(),
                          NP_CloseOutput, (ULONG)FALSE,
                          TAG_DONE);
    Permit();

    if (p == NULL)
    {
        ct_log("concurrent: cannot spawn the main Process, FAIL\n");
        ct_trace("cannot spawn the main Process");
        return 20;
    }

    Signal(&p->pr_Task, SIGF_SINGLE);

    while (ct_main_done == 0U)
        Wait(SIGF_SINGLE);

    /* It signalled from inside ct_main_entry(), so it has not yet returned
       through dos.library's Process teardown. Leaving now would unload the
       segment it is still executing. */
    Delay(25);

    ct_trace("returning %ld", (LONG)((ct_failures == 0UL) ? 0 : 20));

    return (ct_failures == 0UL) ? 0 : 20;
}
