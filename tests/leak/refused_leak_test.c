/*
 * AmiNetXDuo -- the socket leak of docs/RESEARCH.md 37.5, reduced.
 *
 * 37.5 measured AvailMem falling 1009 bytes/s and 776 AmiSocket structures
 * NetX Duo never released, under a concurrent workload, with
 * `nx_tcp_socket_delete refused (NX_STILL_BOUND)` on the serial log 830 times.
 * Two controls in that section cleared the innocent explanations: 11,915 plain
 * socket lifecycles leak nothing, and a dead listener on its own leaks nothing.
 *
 * This program is a matrix of socket lifecycles, each measured the same way,
 * so the arms that leak can be told from the arms that do not, in one run:
 *
 *   A  dial a port with nothing on it                  -- 37.5's control
 *   B  dial a port whose listener never accept()s      -- the named suspect
 *   C  the same, with a non-blocking connect()
 *   D  full lifecycle, the client closes first         -- 37.5's other control
 *   E  full lifecycle, the server closes first
 *   F  full lifecycle, only the client closes at all   -- a half-open peer
 *   G  three dials, then three accept()s, eight times   -- 37.4's defect
 *
 * The measurement is AvailMem(MEMF_PUBLIC), the library's own live socket
 * count, and a histogram of what state those sockets are in -- all three from
 * NetStackQuery (docs/RESEARCH.md 34), sampled before and after each arm.
 * A leaked AmiSocket shows up in all three: AvailMem is what the machine
 * agrees it has lost, the count is what the library still owns, and the
 * histogram says which TCP state it is stuck in, which names the bug.
 *
 * Arm B uses SO_SNDTIMEO because a SYN sent to a port that has a listen
 * request with no socket parked on it is queued by NetX Duo
 * (nx_tcp_packet_process.c, the "no server socket available" branch) with
 * nothing sent back, so a plain blocking connect() there never returns.
 * SO_SNDTIMEO turns that into the ECONNREFUSED 37.5 recorded, at a rate a
 * short run can accumulate.
 *
 * Exit status is 0 when every arm is clean and 5 when any arm leaked, so a
 * runner can read the result without parsing the output.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "aminetxduo/netstatus.h"

static const char version_tag[] __attribute__((used)) =
    "$VER: refused_leak_test 1.0 (26.7.2026)";

/* ------------------------------------------------------------------ LVOs --
 *
 * Hand-vectored for the same reason tests/endurance/endurance.c does it: the
 * NDK inlines read the base from a global, and nothing here wants that.
 *
 * Every stub declares d1/a0/a1 written.  An AmigaOS library call clobbers d0,
 * d1, a0 and a1.  A register that is only an input operand is one GCC may
 * assume the asm leaves alone, so a stub that passes an argument in d1 and
 * does not also declare d1 written lets GCC keep a value there across the
 * `jsr` -- and reuse, or spill, whatever the library left behind.  That turned
 * IoctlSocket(FIONBIO) into a call with a garbage request code and wedged a
 * test for a day (docs/RESEARCH.md 42).  The `_clob_*` dummies bound to those
 * registers and listed as outputs are the NDK's own idiom, from
 * inline/macros.h.
 */

#define L_AF_INET       2
#define L_SOCK_STREAM   1
#define L_SOL_SOCKET    0xFFFF
#define L_SO_REUSEADDR  0x0004
#define L_SO_SNDTIMEO   0x1005
#define L_FIONBIO       0x8004667EUL

typedef struct LeakAddr
{
    UBYTE   sin_len;
    UBYTE   sin_family;
    UWORD   sin_port;
    ULONG   sin_addr;
    UBYTE   sin_zero[8];
} LeakAddr;

typedef struct LeakTimeval
{
    ULONG   tv_secs;
    ULONG   tv_micro;
} LeakTimeval;

static LONG l_socket(struct Library *base, LONG dom, LONG type, LONG proto)
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

static LONG l_bind(struct Library *base, LONG s, LeakAddr *sa)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = sa;
    register LONG            d1  __asm("d1") = (LONG)sizeof(LeakAddr);
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-36:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1)
                      : "a1", "cc", "memory");
    return res;
}

static LONG l_listen(struct Library *base, LONG s, LONG backlog)
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

static LONG l_accept(struct Library *base, LONG s)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = NULL;
    register APTR            a1  __asm("a1") = NULL;
    register LONG            res __asm("d0");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-48:W)"
                      : "=r" (res), "=r" (_clob_a0), "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (a1)
                      : "cc", "memory");
    return res;
}

static LONG l_connect(struct Library *base, LONG s, LeakAddr *sa)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = sa;
    register LONG            d1  __asm("d1") = (LONG)sizeof(LeakAddr);
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-54:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1)
                      : "a1", "cc", "memory");
    return res;
}

static LONG l_setsockopt(struct Library *base, LONG s, LONG level, LONG name,
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

static LONG l_ioctl(struct Library *base, LONG s, ULONG req, APTR argp)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register ULONG           d1  __asm("d1") = req;
    register APTR            a0  __asm("a0") = argp;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-114:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (a0)
                      : "a1", "cc", "memory");
    return res;
}

static LONG l_close(struct Library *base, LONG s)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register LONG            res __asm("d0");

    __asm __volatile ("jsr a6@(-120:W)"
                      : "=r" (res)
                      : "r" (a6), "r" (d0)
                      : "d1", "a0", "a1", "cc", "memory");
    return res;
}

static LONG l_errno(struct Library *base)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            res __asm("d0");

    __asm __volatile ("jsr a6@(-162:W)"
                      : "=r" (res)
                      : "r" (a6)
                      : "d1", "a0", "a1", "cc", "memory");
    return res;
}

_Static_assert(AMI_NETSTATUS_QUERY_LVO == -870, "NetStackQuery LVO moved");

static LONG l_query(struct Library *base, ULONG what, APTR buf, ULONG size)
{
    register struct Library *a6  __asm("a6") = base;
    register ULONG           d0  __asm("d0") = AMI_NETSTATUS_MAGIC;
    register ULONG           d1  __asm("d1") = what;
    register APTR            a0  __asm("a0") = buf;
    register ULONG           d2  __asm("d2") = size;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-870:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (a0), "r" (d2)
                      : "a1", "cc", "memory");
    return res;
}

/* ----------------------------------------------------------- the sampler -- */

#define LEAK_MAX_SOCKETS    96
#define LEAK_STATES         12  /* NX_TCP_* run 1..11 */

typedef struct LeakSample
{
    ULONG   ls_Avail;
    ULONG   ls_Sockets;
    ULONG   ls_PoolFree;
    UWORD   ls_State[LEAK_STATES];
} LeakSample;

static UBYTE leak_buf[sizeof(NetStatusHeader) +
                      LEAK_MAX_SOCKETS * sizeof(NetStatusSocket)];

static VOID leak_sample(struct Library *base, LeakSample *out)
{
    NetStatusHeader *hdr = (NetStatusHeader *)leak_buf;
    UWORD            i;

    out->ls_Avail    = (ULONG)AvailMem(MEMF_PUBLIC);
    out->ls_Sockets  = 0UL;
    out->ls_PoolFree = 0UL;

    for (i = 0; i < LEAK_STATES; i++)
        out->ls_State[i] = 0;

    hdr->nsh_Magic   = AMI_NETSTATUS_MAGIC;
    hdr->nsh_Version = AMI_NETSTATUS_VERSION;
    if (l_query(base, NETSTATUS_SOCKETS, leak_buf, (ULONG)sizeof(leak_buf))
        >= 0)
    {
        NetStatusSocket *so = (NetStatusSocket *)NETSTATUS_ENTRIES(hdr);

        out->ls_Sockets = (ULONG)hdr->nsh_Available;

        for (i = 0; i < hdr->nsh_Count; i++)
        {
            UWORD st = so[i].nso_State;

            if ((so[i].nso_Flags & NETSTATUS_SOCK_TCP) != 0 &&
                st < LEAK_STATES)
                out->ls_State[st]++;
        }
    }

    hdr->nsh_Magic   = AMI_NETSTATUS_MAGIC;
    hdr->nsh_Version = AMI_NETSTATUS_VERSION;
    if (l_query(base, NETSTATUS_SYSTEM, leak_buf,
                (ULONG)(sizeof(NetStatusHeader) + sizeof(NetStatusSystem)))
        >= 0 && hdr->nsh_Count > 0)
    {
        NetStatusSystem *sys = (NetStatusSystem *)NETSTATUS_ENTRIES(hdr);

        out->ls_PoolFree = sys->nss_PoolFree;
    }
}

/* ------------------------------------------------------------- the arms -- */

#define LEAK_PORT_LIVE  7911U   /* a listener that never accepts             */
#define LEAK_PORT_FULL  7913U   /* a listener that does accept               */
#define LEAK_PORT_DEAD  7912U   /* nothing at all                            */
#define LEAK_ROUNDS     32

/*
 * How long an arm waits before its final reading, in Delay() ticks.
 * LEAK_QUICK is just long enough for the round's own traffic to finish.
 * LEAK_DEADLINE is past BSD_CLOSING_DEADLINE (60 s, socket.c): a socket
 * parked by bsd_closing_park() is not a leak until the sweep has had its
 * chance at it, and reading the count before then counts every half-closed
 * socket as a leak.
 */
#define LEAK_QUICK      25UL
#define LEAK_DEADLINE   (66UL * 50UL)

/* What a round does after the connect. */
#define LEAK_DIAL       0       /* nothing: the connect is the whole round   */
#define LEAK_CLIENT1ST  1       /* accept, close client, close server        */
#define LEAK_SERVER1ST  2       /* accept, close server, close client        */
#define LEAK_HALF       3       /* accept, close client, leak the server fd  */

static VOID leak_zero(APTR p, ULONG n)
{
    UBYTE *b = (UBYTE *)p;

    while (n-- > 0UL)
        *b++ = 0;
}

static VOID leak_fill_addr(LeakAddr *sa, UWORD port)
{
    leak_zero(sa, sizeof(*sa));

    sa->sin_len    = (UBYTE)sizeof(*sa);
    sa->sin_family = L_AF_INET;
    sa->sin_port   = port;
    sa->sin_addr   = 0x7F000001UL;
}

/*
 * One arm.  Returns the number of rounds whose connect() reported a failure:
 * an arm that expects refusals and gets none is not testing what it claims.
 */
static VOID leak_kick_sweep(struct Library *base)
{
    /* bsd_closing_sweep() is driven by CloseSocket() and by nothing else, so
       a program that has stopped closing sockets never collects the ones it
       parked.  Two, because the sweep runs before the socket being closed is
       itself parked. */
    LONG i;

    for (i = 0; i < 2; i++)
    {
        LONG s = l_socket(base, L_AF_INET, L_SOCK_STREAM, 0);

        if (s >= 0)
            (VOID)l_close(base, s);
    }
}

static ULONG leak_arm(struct Library *base, UWORD port, UWORD shape,
                      UWORD nonblock, LONG listener, ULONG settle,
                      LeakSample *before, LeakSample *after, LONG *last_errno)
{
    ULONG refused = 0UL;
    LONG  round;

    *last_errno = 0;

    /* Let the previous arm's traffic finish before the baseline is taken. */
    Delay(25);
    leak_sample(base, before);

    for (round = 0; round < LEAK_ROUNDS; round++)
    {
        LeakAddr    sa;
        LeakTimeval tv;
        LONG        on = 1;
        LONG        s, a;

        s = l_socket(base, L_AF_INET, L_SOCK_STREAM, 0);
        if (s < 0)
        {
            *last_errno = l_errno(base);
            break;
        }

        if (nonblock)
        {
            (VOID)l_ioctl(base, s, L_FIONBIO, &on);
        }
        else
        {
            /* Short, because a SYN queued on a listen request with no socket
               on it is never answered: without a timeout this connect() does
               not come back at all. */
            tv.tv_secs  = 0UL;
            tv.tv_micro = 250000UL;
            (VOID)l_setsockopt(base, s, L_SOL_SOCKET, L_SO_SNDTIMEO, &tv,
                               (LONG)sizeof(tv));
        }

        leak_fill_addr(&sa, port);

        if (l_connect(base, s, &sa) < 0)
        {
            *last_errno = l_errno(base);
            refused++;
        }

        if (shape == LEAK_DIAL)
        {
            (VOID)l_close(base, s);
            continue;
        }

        a = l_accept(base, listener);
        if (a < 0)
        {
            *last_errno = l_errno(base);
            (VOID)l_close(base, s);
            break;
        }

        if (shape == LEAK_SERVER1ST)
        {
            (VOID)l_close(base, a);
            (VOID)l_close(base, s);
        }
        else if (shape == LEAK_HALF)
        {
            /* The client closes; the server end is abandoned open, which is
               the half-open peer bsd_closing_sweep()'s deadline exists for. */
            (VOID)l_close(base, s);
        }
        else
        {
            (VOID)l_close(base, s);
            (VOID)l_close(base, a);
        }
    }

    /* The closing sweep runs from CloseSocket(), so give the last rounds the
       same chance to complete that every earlier round had -- and, for an arm
       that is about the sweep's own deadline, wait it out. */
    Delay(settle);
    leak_kick_sweep(base);
    Delay(25);
    leak_sample(base, after);

    return refused;
}

/* Written and flushed before the arm runs, so a run that has to be killed
   still says which arm it was in. */
static VOID leak_mark(const char *name)
{
    LONG args[1];

    args[0] = (LONG)name;
    VPrintf((CONST_STRPTR)"-- arm %s\n", args);
    (VOID)Flush(Output());
}

static VOID leak_report(const char *name, ULONG refused,
                        const LeakSample *before, const LeakSample *after,
                        LONG last_errno)
{
    LONG  args[8];
    UWORD i;

    args[0] = (LONG)name;
    args[1] = (LONG)refused;
    args[2] = (LONG)LEAK_ROUNDS;
    args[3] = last_errno;
    args[4] = (LONG)after->ls_Avail - (LONG)before->ls_Avail;
    args[5] = (LONG)after->ls_Sockets - (LONG)before->ls_Sockets;
    args[6] = (LONG)after->ls_PoolFree - (LONG)before->ls_PoolFree;

    VPrintf((CONST_STRPTR)"%s: fail %ld/%ld errno %ld | "
            "AvailMem %ld  sockets %ld  poolfree %ld\n", args);
    (VOID)Flush(Output());

    for (i = 1; i < LEAK_STATES; i++)
    {
        if (after->ls_State[i] == 0 && before->ls_State[i] == 0)
            continue;

        args[0] = (LONG)i;
        args[1] = (LONG)before->ls_State[i];
        args[2] = (LONG)after->ls_State[i];
        VPrintf((CONST_STRPTR)"    state %2ld: %ld -> %ld\n", args);
    }
}

/*
 * Whether the listener keeps accepting.  docs/RESEARCH.md 37.4 found one that
 * stopped for good -- 1,951 consecutive EINVALs after a single failed
 * relisten -- and the trigger for that relisten failure is not established,
 * so this cannot provoke it directly.  It instead drives the path the failure
 * was seen on hardest: three dials before any accept(), so two of them sit in
 * the listen queue and the re-arm after each accept() has to replay a queued
 * connection (nx_tcp_server_socket_relisten's NX_CONNECTION_PENDING branch)
 * rather than park an idle socket.
 *
 * Returns the number of accept() calls that failed.  Anything but zero is the
 * defect, whatever provoked it.
 */
#define LEAK_BURST      3
#define LEAK_BURSTS     8

/*
 * This arm used to wedge the calling task, and the cause was in this file, not
 * the library.  Three non-blocking connect() calls issued one after another
 * wedged on the second or the third, three times out of three
 * (docs/RESEARCH.md 41.4).  The LVO stubs above did not declare d1/a0/a1
 * clobbered, so GCC kept the FIONBIO request code in d1 across a library call
 * that overwrites it, IoctlSocket() was handed a stale request, ASF_NONBLOCK
 * was never set, and the dial became a blocking connect() to a listen request
 * with no socket parked on it, which never returns.  Any real call in between
 * forced GCC to rematerialise the constant, which is why interposing a
 * VPrintf() "fixed" it.  docs/RESEARCH.md 42.
 *
 * Build with -DLEAK_BURST_DELAY=OFF to run the dials back to back, the shape
 * that failed.
 */
#ifndef LEAK_BURST_ARM
#  define LEAK_BURST_ARM 0
#endif
#ifndef LEAK_BURST_DELAY
#  define LEAK_BURST_DELAY 1
#endif

#if LEAK_BURST_ARM
static ULONG leak_accept_burst(struct Library *base, LONG listener,
                               LONG *last_errno)
{
    ULONG failures = 0UL;
    LONG  on = 1, off = 0;
    LONG  round;

    *last_errno = 0;

    /* Non-blocking, so a listener that has stopped accepting reports it
       instead of hanging this program on the first burst. */
    (VOID)l_ioctl(base, listener, L_FIONBIO, &on);

    for (round = 0; round < LEAK_BURSTS; round++)
    {
        LONG c[LEAK_BURST];
        LONG i;

        for (i = 0; i < LEAK_BURST; i++)
        {
            LeakAddr sa;

            c[i] = l_socket(base, L_AF_INET, L_SOCK_STREAM, 0);
            if (c[i] < 0)
                continue;

            (VOID)l_ioctl(base, c[i], L_FIONBIO, &on);
            leak_fill_addr(&sa, LEAK_PORT_FULL);
            (VOID)l_connect(base, c[i], &sa);

            /*
             * One tick between dials, no longer load-bearing: with the LVO
             * clobbers above fixed it only paces the burst.  Build with
             * -DLEAK_BURST_DELAY=OFF to run the dials back to back.
             * docs/RESEARCH.md 42.
             */
#if LEAK_BURST_DELAY
            Delay(1);
#endif
        }

        for (i = 0; i < LEAK_BURST; i++)
        {
            LONG a    = -1;
            LONG tries;


            /* 0.4 s per connection, which is many times the handshake on
               loopback: past that the listener is not going to answer. */
            for (tries = 0; tries < 20 && a < 0; tries++)
            {
                a = l_accept(base, listener);
                if (a < 0)
                {
                    *last_errno = l_errno(base);
                    Delay(1);
                }
            }

            if (a < 0)
            {
                failures++;
            }
            else
            {
                (VOID)l_close(base, a);
            }
        }

        for (i = 0; i < LEAK_BURST; i++)
        {
            if (c[i] >= 0)
                (VOID)l_close(base, c[i]);
        }
    }

    (VOID)l_ioctl(base, listener, L_FIONBIO, &off);

    return failures;
}
#endif /* LEAK_BURST_ARM */

/* Bind and listen on `port`, or -1. */
static LONG leak_listener(struct Library *base, UWORD port)
{
    LeakAddr sa;
    LONG     ls, on = 1;

    ls = l_socket(base, L_AF_INET, L_SOCK_STREAM, 0);
    if (ls < 0)
        return -1;

    (VOID)l_setsockopt(base, ls, L_SOL_SOCKET, L_SO_REUSEADDR, &on,
                       (LONG)sizeof(on));

    leak_fill_addr(&sa, port);

    if (l_bind(base, ls, &sa) < 0 || l_listen(base, ls, 4) < 0)
    {
        LONG args[1];

        args[0] = l_errno(base);
        VPrintf((CONST_STRPTR)"refused_leak: bind/listen failed, errno %ld\n",
                args);
        (VOID)l_close(base, ls);
        return -1;
    }

    return ls;
}

int main(VOID)
{
    struct Library *base;
    LeakSample      before, after;
    LONG            quiet, busy;
    LONG            err = 0;
    ULONG           refused;
    LONG            failed = 0;
    LONG            args[2];

    base = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    if (base == NULL)
    {
        PutStr((CONST_STRPTR)"refused_leak: no bsdsocket.library\n");
        return RETURN_FAIL;
    }

    args[0] = LEAK_ROUNDS;
    VPrintf((CONST_STRPTR)"refused_leak: 7 arms, %ld lifecycles each\n", args);

    /* ---- A: the control.  Nothing is listening; the RST is immediate. ---- */
    leak_mark("A");
    refused = leak_arm(base, LEAK_PORT_DEAD, LEAK_DIAL, 0, -1, LEAK_QUICK,
                       &before, &after, &err);
    leak_report("A dead port  ", refused, &before, &after, err);
    if ((LONG)after.ls_Sockets - (LONG)before.ls_Sockets >= LEAK_ROUNDS / 2)
        failed |= 1;

    quiet = leak_listener(base, LEAK_PORT_LIVE);
    busy  = leak_listener(base, LEAK_PORT_FULL);
    if (quiet < 0 || busy < 0)
    {
        CloseLibrary(base);
        return RETURN_FAIL;
    }

    /*
     * bsd_listen() parks an armed socket on the port, so the first dial to a
     * healthy listener is answered.  Spend that one here, outside the arms:
     * every dial after it finds the listen request with no socket on it,
     * which is the state 37.5 is about.
     */
    {
        LeakAddr sa;
        LONG     s = l_socket(base, L_AF_INET, L_SOCK_STREAM, 0);

        if (s >= 0)
        {
            leak_fill_addr(&sa, LEAK_PORT_LIVE);
            (VOID)l_connect(base, s, &sa);
            (VOID)l_close(base, s);
        }
    }

    /* ---- G: whether the listener keeps accepting.  37.4's defect. ---- */
#if LEAK_BURST_ARM
    {
        ULONG bad;

        leak_mark("G");
        bad = leak_accept_burst(base, busy, &err);

        args[0] = (LONG)bad;
        args[1] = (LONG)(LEAK_BURSTS * LEAK_BURST);
        VPrintf((CONST_STRPTR)"G accept burst: %ld/%ld accepts failed", args);
        args[0] = err;
        VPrintf((CONST_STRPTR)", last errno %ld\n", args);

        if (bad != 0UL)
            failed |= 64;
    }
#endif

    /* ---- B: the named suspect. ---- */
    leak_mark("B");
    refused = leak_arm(base, LEAK_PORT_LIVE, LEAK_DIAL, 0, -1, LEAK_QUICK,
                       &before, &after, &err);
    leak_report("B listen req ", refused, &before, &after, err);
    if ((LONG)after.ls_Sockets - (LONG)before.ls_Sockets >= LEAK_ROUNDS / 2)
        failed |= 2;

    /* ---- C: the same, with a non-blocking connect. ---- */
    leak_mark("C");
    refused = leak_arm(base, LEAK_PORT_LIVE, LEAK_DIAL, 1, -1, LEAK_QUICK,
                       &before, &after, &err);
    leak_report("C nonblocking", refused, &before, &after, err);
    if ((LONG)after.ls_Sockets - (LONG)before.ls_Sockets >= LEAK_ROUNDS / 2)
        failed |= 4;

    /* ---- D: full lifecycle, client closes first -- 37.5's clean control. -- */
    leak_mark("D");
    refused = leak_arm(base, LEAK_PORT_FULL, LEAK_CLIENT1ST, 1, busy,
                       LEAK_QUICK, &before, &after, &err);
    leak_report("D client 1st ", refused, &before, &after, err);
    if ((LONG)after.ls_Sockets - (LONG)before.ls_Sockets >= LEAK_ROUNDS / 2)
        failed |= 8;

    /* ---- E: full lifecycle, server closes first. ---- */
    leak_mark("E");
    refused = leak_arm(base, LEAK_PORT_FULL, LEAK_SERVER1ST, 1, busy,
                       LEAK_QUICK, &before, &after, &err);
    leak_report("E server 1st ", refused, &before, &after, err);
    if ((LONG)after.ls_Sockets - (LONG)before.ls_Sockets >= LEAK_ROUNDS / 2)
        failed |= 16;

    /*
     * ---- F: the server end is never closed at all. ----
     *
     * Counted differently from the others: this arm abandons 32 server
     * descriptors, so 32 more live sockets is the arm working, not leaking.
     * The clients are the tell -- each sent its FIN and waits for one that is
     * never coming, so after the sweep's deadline every one must be gone.  Any
     * still in FIN_WAIT_1 or FIN_WAIT_2 is a socket the sweep tried and failed
     * to collect, the 830 NX_STILL_BOUND of 37.5.
     */
    leak_mark("F");
    refused = leak_arm(base, LEAK_PORT_FULL, LEAK_HALF, 1, busy,
                       LEAK_DEADLINE, &before, &after, &err);
    leak_report("F half open  ", refused, &before, &after, err);
    if ((LONG)after.ls_State[NETSTATUS_TCP_FIN_WAIT_1] +
        (LONG)after.ls_State[NETSTATUS_TCP_FIN_WAIT_2] >= LEAK_ROUNDS / 2)
        failed |= 32;

    (VOID)l_close(base, quiet);
    (VOID)l_close(base, busy);
    CloseLibrary(base);

    if (failed != 0)
    {
        args[0] = failed;
        VPrintf((CONST_STRPTR)"refused_leak: LEAKED (arms 0x%lx)\n", args);
        return 5;
    }

    PutStr((CONST_STRPTR)"refused_leak: clean\n");

    return RETURN_OK;
}
