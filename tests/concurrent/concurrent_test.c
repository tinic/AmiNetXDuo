/*
 * AmiNetXDuo, eight applications inside the stack at once.
 *
 * WHY THIS EXISTS
 *
 * The baton defect of docs/RESEARCH.md 78 was a second Task entering NetX Duo
 * without the baton, and it survived every automated harness in this tree. Not
 * for want of concurrency tests, for want of one that is concurrent through
 * bsdsocket.library:
 *
 *   tests/soak         4 adopted Tasks, 2 ThreadX threads, deliberate baton
 *                      churn, and zero socket calls. It drives the ThreadX
 *                      port directly, so ami_netstack_enter() is never on the
 *                      path.
 *   tests/ram_driver   nx_tcp_* directly, same gap.
 *   tests/endurance    up to 34 worker contexts, each with its own library
 *                      base, exactly the right shape, and not in
 *                      EMULATOR_TESTS, because it runs for hours.
 *   tests/soak/fitz_soak   the same, and the same reason.
 *
 * So the routinely-exercised count of concurrent library users was one. This
 * closes that at a size CI can afford.
 *
 * WHAT IT DOES
 *
 * Eight Processes, each with its OWN bsdsocket.library base, which is what
 * makes them applications rather than threads, and what makes each one arrive
 * at the bracket as an unrelated Exec Task. Four listen on 127.0.0.1, four
 * connect and echo-verify against them, all at the same time.
 *
 * Each pair's payload is keyed to its own id, so a byte that arrives on the
 * wrong connection is a failure rather than a coincidence: two Tasks inside
 * NetX Duo at once corrupt shared state, and cross-fed payload is what that
 * looks like from out here.
 *
 * The watchdog is not garnish. The visible symptom of the baton defect was a
 * NetX Duo caller error, but that was only the subset whose timing left the
 * pointer NULL; the rest wedged silently. A test that only checks return codes
 * would have called that a pass.
 *
 * Reaches the library through its LVOs and links nothing of the stack, for the
 * reason tests/leak/CMakeLists.txt gives: linking src/netstack would get a
 * second set of NetX Duo globals and measure the wrong stack.
 *
 * Reading a run: the verdict line is the result, and it is printed before the
 * last CloseLibrary(), which hangs in the a2065 teardown and stops the run from
 * ever writing DH0:.done. A trailing timeout under a verdict is expected;
 * run-concurrent.sh scores the verdict and says so. A timeout with no verdict
 * is a real failure, and the last [ct] line in the serial log is how far it
 * got.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdarg.h>

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <dos/dos.h>
#include <dos/dostags.h>

#include <proto/exec.h>
#include <proto/dos.h>

/* ------------------------------------------------------------- the shape -- */

#ifndef CT_PAIRS
#define CT_PAIRS        4               /* 4 servers + 4 clients == 8 bases  */
#endif
#ifndef CT_BYTES
#define CT_BYTES        32768UL         /* per pair, each way                */
#endif
#define CT_APPS         (2 * CT_PAIRS)
#define CT_PORT_BASE    7420
#define CT_CHUNK        2048UL

/*
 * A bsdsocket call runs NetX Duo on the CALLER's stack, ami_netstack_enter()
 * takes the baton and descends from there, so a child's stack has to carry
 * the whole TCP/IP call depth, not just this file's frames. The stack's own
 * session processes get TCP_SESSION_STACK (16 KB) and its startup process gets
 * BSD_STARTUP_STACK (64 KB); every other harness here spawns at 64 KB or more.
 *
 * The first version of this test used 8 KB, and that was the hang: RESEARCH
 * 16.9 records the same failure from the other end, a 4 KB Shell stack under
 * NetTrace gave an F-line trap and a reboot loop, which the runner reported as
 * a timeout with truncated output, because a reboot reopens DH0:stdout.txt.
 */
#define CT_STACK        (64UL * 1024UL)

/*
 * Two deadlines run back to back, servers reaching listen(), then everything
 * finishing, so the harness needs 2 x CT_DEADLINE_SECS of its own, and the
 * -t it runs under has to cover that PLUS the boot it does not control.
 *
 * CT_BUDGET_SECS is that arithmetic, and run-concurrent.sh derives its default
 * -t from the same numbers rather than from a guess. Kickstart boot and the
 * DHCP attempt against SLIRP come out of CT_BOOT_SECS; DHCP is the long pole
 * and is bounded by the stack's own retry schedule, not by us.
 */
#ifndef CT_DEADLINE_SECS
#define CT_DEADLINE_SECS  60
#endif
#define CT_BOOT_SECS      90
#define CT_BUDGET_SECS    (CT_BOOT_SECS + 2 * CT_DEADLINE_SECS + 30)

#define CT_TIMEOUT_TICKS  (CT_DEADLINE_SECS * 50)   /* 50 ticks/s            */
#define CT_POLL_TICKS     10
#define CT_BEAT_TICKS     100           /* heartbeat every 2 s               */

/* --------------------------------------------------------------- the LVOs -- */

/*
 * Offsets and register assignments from the NDK's own
 * pragmas/bsdsocket_pragmas.h, socket 0x01e, send 0x042, recv 0x04e, and
 * not from counting in sixes. Every stub declares d1/a0/a1 clobbered: the one
 * that did not turned IoctlSocket(FIONBIO) into a call with a garbage request
 * code and wedged a test for a day (RESEARCH 42).
 */

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

/* -------------------------------------------------------------- the state -- */

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

/*
 * Static, not allocated: the child reads it through pr_Task.tc_UserData while
 * the parent is inside Wait(), and a static lives past any argument frame.
 */
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

/*
 * Two channels, because they fail in different ways.
 *
 * ct_log() is stdout, flushed per line: the runner reads it out of a file
 * after the run, so an unflushed line is a line that does not exist when the
 * program never exits.
 *
 * ct_trace() is the serial port through exec's RawPutChar, and it is the one
 * that survives. It needs no FileHandle and no dos.library, so a child can
 * trace without sharing the parent's Output(); FS-UAE writes it to
 * build/serial-<tag>.log as it arrives, and the runner prints that log even on
 * a timeout. A guest reboot reopens DH0:stdout.txt and erases stdout; it does
 * not truncate the serial log, which is how a reboot loop becomes visible
 * rather than looking like a silent hang.
 *
 * The NDK declares RawPutChar only in the assembler headers, so it is wired up
 * here the way src/common/compat.c does it.
 */
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

/* -------------------------------------------------------------- the apps -- */

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

    /*
     * Send a chunk, read it back, verify, repeat. Lock-step rather than
     * streaming: it keeps all four pairs inside a socket call at overlapping
     * times, which is the state this test exists to produce, and it bounds the
     * buffers to one chunk.
     */
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

/*
 * One application. Its own library base, opened here and closed here, so the
 * bracket sees an unrelated Exec Task with an unrelated base, which is the
 * whole point of spawning Processes instead of starting threads.
 */
static VOID ct_app_entry(VOID)
{
    struct Process *me = (struct Process *)FindTask((STRPTR)0);
    struct Library *base;
    CtApp          *a;

    Wait(SIGF_SINGLE);

    a = (CtApp *)me->pr_Task.tc_UserData;
    if (a == NULL)
    {
        /* The parent sets tc_UserData under Forbid() before it signals, so
           this means the signal was already pending at creation and the Wait()
           fell straight through. Say so rather than exiting silently, which
           reads from outside as a child that never started. */
        ct_trace("child woke with no CtApp -- SIGF_SINGLE was already set");
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

/*
 * Spawn one application, following tests/endurance/endurance.c, which is the
 * working precedent for a worker with its own library base.
 *
 * Forbid() covers creation and tc_UserData together: without it the child can
 * reach ct_app_entry() before the pointer is stored, and if SIGF_SINGLE is
 * already pending the Wait() falls through onto a NULL.
 *
 * No NP_Output. The first version passed the parent's Output() to all eight
 * with NP_CloseOutput FALSE, which put nine Processes on one FileHandle's
 * fh_Buf/fh_Pos. Children have no reason to touch dos.library at all now that
 * ct_trace() goes to the serial port, so the sharing is simply removed rather
 * than made safe.
 */
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

/* --------------------------------------------------------------- the main -- */

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

    /*
     * The parent holds a base of its own for the duration. Without it the
     * stack would come up and go down again around each child, and the run
     * would measure eight sequential startups rather than eight concurrent
     * users.
     */
    ct_log("concurrent: opening the library (this starts the stack)\n");
    ct_trace("parent: OpenLibrary(bsdsocket.library)");
    base = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4UL);
    ct_log("concurrent: library base %s\n",
           (LONG)((base != NULL) ? "open" : "NULL"));
    ct_trace("parent: library base %s", (LONG)((base != NULL) ? "open" : "NULL"));
    ct_check(base != NULL, "parent opens bsdsocket.library", 0);
    if (base == NULL)
    {
        ct_log("%ld checks, %ld failures -- FAIL\n",
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

    /*
     * Wait with a deadline. A wedge is the failure mode that matters: the
     * baton defect's loud symptom was a caller error, but its quiet one was a
     * Task that never came back, and a harness without a deadline reports that
     * as nothing at all.
     */
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

    /*
     * The verdict goes out before the park below, not after it. A wedged run
     * never leaves that loop, so a summary printed afterwards is a summary
     * nobody ever reads, which is how the first version of this managed to
     * hang and report nothing.
     */
    ct_log("%ld checks, %ld failures -- %s\n",
           (LONG)ct_checks, (LONG)ct_failures,
           (LONG)((ct_failures == 0UL) ? "PASS" : "FAIL"));
    ct_trace("%ld checks, %ld failures -- %s",
             (LONG)ct_checks, (LONG)ct_failures,
             (LONG)((ct_failures == 0UL) ? "PASS" : "FAIL"));

    /* A wedged child is still running in our address space; leaving would free
       nothing and it would write into a static that no longer belongs to it.
       The runner sees this as a timeout with no exit status, because DH0:.done
       is written by the Startup-Sequence after we return: the verdict above is
       the result, and the timeout is only how it had to be delivered. */
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

    /*
     * The parent's is the last base, so this is what shuts the stack down,
     * and bsd_lib_close() runs netstack_shutdown() on the CALLER's stack.
     * Startup does not: bsd_lib_open() hands it to a 64 KB Process precisely
     * because an opener's stack cannot carry it. That asymmetry is why this
     * whole function runs on a spawned Process rather than on main()'s, which
     * a Kickstart 3.1 Shell gives 4,096 bytes.
     */
    ct_trace("parent: CloseLibrary (last base -- this stops the stack)");
    CloseLibrary(base);
    ct_trace("parent: stack stopped");
}

/*
 * main() does nothing but hand the work to a Process with a real stack.
 *
 * A Shell command gets 4,096 bytes on Kickstart 3.1 and this toolchain's crt0
 * exports no __stack hook to ask for more (RESEARCH 11.5), so every harness
 * here that touches the library does its work on a spawned Process instead,
 * tests/crypto68k, tests/compare, ClientRun. This one is an application like
 * the eight it spawns, and it has the same reason to be one.
 */
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
                          /* Safe to share here, unlike the eight: this one
                             does all the ct_log() and main() is inside Wait()
                             for its whole life, so the FileHandle has one
                             writer at a time. */
                          NP_Output,    (ULONG)Output(),
                          NP_CloseOutput, (ULONG)FALSE,
                          TAG_DONE);
    Permit();

    if (p == NULL)
    {
        ct_log("concurrent: cannot spawn the main Process -- FAIL\n");
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
