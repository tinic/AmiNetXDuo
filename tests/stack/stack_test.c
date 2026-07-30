/*
 * AmiNetXDuo -- the API from a 4 KB stack, and how much of it gets touched.
 *
 * WHY THIS EXISTS
 *
 * A bsdsocket.library vector runs on the CALLER's stack. An AmigaDOS Shell
 * gives a command 4 KB by default, there is no guard page and no MMU, so a
 * frame that does not fit does not fault -- it writes over whatever lies below
 * the stack and the machine dies later, somewhere unrelated. That is the
 * opposite of a hosted C environment, where the same mistake is a clean
 * SIGSEGV at the instruction that made it.
 *
 * Every other harness in this tree hides that. tests/soak/fitz_soak.c,
 * tests/endurance/endurance.c and tests/concurrent/concurrent_test.c all spawn
 * their workers with a generous NP_StackSize -- concurrent_test at 8 KB, the
 * others more -- because a test that is about concurrency does not want to
 * think about stack. So nothing here has ever called the API from a stack a
 * real user's program would have.
 *
 * An A3000 owner on English Amiga Board reported applications freezing under
 * an ordinary file copy, a jerky pointer, then the machine gone, with an
 * upscroller of MungWall hits from this library in the Sashimi window as it
 * died -- and said to go and look at stack usage in the API calls. He was
 * reading a stack overflow: on AmigaOS a Process's stack is itself an AllocMem
 * block, so running off the bottom of it lands in the guard MungWall put
 * there, which is exactly the "massive hitting" he saw.
 *
 * WHAT IT DOES
 *
 * A worker Process with NP_StackSize 4096 -- not a number chosen to be
 * dramatic, it is what `Execute` and the Shell hand out -- then hammers the
 * entry points that matter: the loopback connect/send/recv/WaitSelect loop
 * that a file copy is made of, plus the resolver and interface calls, which
 * measurement says are the deep ones.
 *
 * The measurement is a low-water mark, not an estimate. The worker paints its
 * own unused stack with a pattern, works, and then scans up from tc_SPLower
 * for the first word that changed. That is the deepest address anything
 * actually wrote, so it counts what -fstack-usage cannot: the vendored NetX
 * Duo frames, the ThreadX port, and any interrupt or Exec call that borrowed
 * the stack on the way through.
 *
 * A run that overflows does not reach the report, so the parent watchdogs it
 * and a silent worker is a failure rather than a pass.
 *
 * Reaches the library through its LVOs and links nothing of the stack, for the
 * reason tests/leak/CMakeLists.txt gives: linking src/netstack would get a
 * second set of NetX Duo globals and measure the wrong stack.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdarg.h>

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>

#include <proto/exec.h>
#include <proto/dos.h>

/* ------------------------------------------------------------- the shape -- */

/*
 * The AmigaDOS Shell default. Raise it from the command line to find the
 * headroom rather than only to pass:
 *
 *   cmake -DSTACK_TEST_STACK=8192 ...
 */
#ifndef ST_STACK
#define ST_STACK        4096UL
#endif

/* Enough passes that a per-call leak of stack shows up as a deeper mark, few
   enough that the emulator tier is not waiting on it. */
#ifndef ST_ROUNDS
#define ST_ROUNDS       32UL
#endif

#define ST_PORT         7460
#define ST_CHUNK        512UL
#define ST_PATTERN      0xA5C3A5C3UL
#define ST_PHASES       11

/*
 * Generous because the resolver calls are meant to fail and each one waits out
 * BSD_RESOLVE_TIMEOUT (src/bsdsocket/resolver.c). Waiting them out is the
 * point -- the depth reached on the way to the timeout is the measurement --
 * and under SLIRP a lookup with nowhere to go has been seen to take
 * substantially longer than the 30 s nominal.
 */
#define ST_TIMEOUT_TICKS (600 * 50)     /* 600 s at 50 ticks/s */

/* The signal the worker sends the parent when it is done. */
#define ST_DONE_SIGNAL  SIGBREAKB_CTRL_F
#define ST_DONE_MASK    SIGBREAKF_CTRL_F

/* --------------------------------------------------------------- the LVOs -- */

/*
 * Offsets and register assignments from src/bsdsocket/bsdsocket_vectors.c and
 * the NDK's pragmas, not from counting in sixes. Every stub declares d1/a0/a1
 * clobbered -- the one in tests/concurrent that did not turned an
 * IoctlSocket() into a call with a garbage request code (RESEARCH 42).
 */

#define S_AF_INET       2
#define S_SOCK_STREAM   1
#define S_SOL_SOCKET    0xFFFF
#define S_SO_REUSEADDR  0x0004

typedef struct StAddr
{
    UBYTE   sin_len;
    UBYTE   sin_family;
    UWORD   sin_port;
    ULONG   sin_addr;
    UBYTE   sin_zero[8];
} StAddr;

typedef struct StTimeval
{
    LONG    tv_secs;
    LONG    tv_micro;
} StTimeval;

static LONG s_socket(struct Library *base, LONG dom, LONG type, LONG proto)
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

static LONG s_bind(struct Library *base, LONG s, StAddr *sa)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = sa;
    register LONG            d1  __asm("d1") = (LONG)sizeof(StAddr);
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-36:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1)
                      : "a1", "cc", "memory");
    return res;
}

static LONG s_listen(struct Library *base, LONG s, LONG backlog)
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

static LONG s_accept(struct Library *base, LONG s)
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

static LONG s_connect(struct Library *base, LONG s, StAddr *sa)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = sa;
    register LONG            d1  __asm("d1") = (LONG)sizeof(StAddr);
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-54:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1)
                      : "a1", "cc", "memory");
    return res;
}

static LONG s_send(struct Library *base, LONG s, APTR buf, LONG len)
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

static LONG s_recv(struct Library *base, LONG s, APTR buf, LONG len)
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

static LONG s_setsockopt(struct Library *base, LONG s, LONG level, LONG name,
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

static LONG s_getsockname(struct Library *base, LONG s, StAddr *sa, LONG *len)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = sa;
    register APTR            a1  __asm("a1") = len;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-102:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (a1)
                      : "cc", "memory");
    return res;
}

static LONG s_closesocket(struct Library *base, LONG s)
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

static LONG s_waitselect(struct Library *base, LONG nfds, APTR rd, APTR wr,
                         APTR ex, StTimeval *tv, ULONG *sigs)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = nfds;
    register APTR            a0  __asm("a0") = rd;
    register APTR            a1  __asm("a1") = wr;
    register APTR            a2  __asm("a2") = ex;
    register APTR            a3  __asm("a3") = tv;
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

static LONG s_errno(struct Library *base)
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

static APTR s_gethostbyname(struct Library *base, const char *name)
{
    register struct Library *a6  __asm("a6") = base;
    register APTR            a0  __asm("a0") = (APTR)name;
    register APTR            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-210:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0)
                      : "a1", "cc", "memory");
    return res;
}

static APTR s_gethostbyaddr(struct Library *base, APTR addr, LONG len,
                            LONG type)
{
    register struct Library *a6  __asm("a6") = base;
    register APTR            a0  __asm("a0") = addr;
    register LONG            d0  __asm("d0") = len;
    register LONG            d1  __asm("d1") = type;
    register APTR            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-216:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0), "r" (d0), "r" (d1)
                      : "a1", "cc", "memory");
    return res;
}

static APTR s_getservbyname(struct Library *base, const char *name,
                            const char *proto)
{
    register struct Library *a6  __asm("a6") = base;
    register APTR            a0  __asm("a0") = (APTR)name;
    register APTR            a1  __asm("a1") = (APTR)proto;
    register APTR            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-234:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1)
                      : "r" (a6), "r" (a0), "r" (a1)
                      : "cc", "memory");
    return res;
}

static LONG s_getaddrinfo(struct Library *base, const char *node,
                          const char *service, APTR hints, APTR *res_out)
{
    register struct Library *a6  __asm("a6") = base;
    register APTR            a0  __asm("a0") = (APTR)node;
    register APTR            a1  __asm("a1") = (APTR)service;
    register APTR            a2  __asm("a2") = hints;
    register APTR            a3  __asm("a3") = res_out;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-810:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1)
                      : "r" (a6), "r" (a0), "r" (a1), "r" (a2), "r" (a3)
                      : "cc", "memory");
    return res;
}

static LONG s_getnameinfo(struct Library *base, StAddr *sa, LONG salen,
                          char *host, LONG hostlen, char *serv, LONG servlen,
                          LONG flags)
{
    register struct Library *a6  __asm("a6") = base;
    register APTR            a0  __asm("a0") = sa;
    register LONG            d0  __asm("d0") = salen;
    register APTR            a1  __asm("a1") = host;
    register LONG            d1  __asm("d1") = hostlen;
    register APTR            a2  __asm("a2") = serv;
    register LONG            d2  __asm("d2") = servlen;
    register LONG            d3  __asm("d3") = flags;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-822:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1)
                      : "r" (a6), "r" (a0), "r" (d0), "r" (a1), "r" (d1),
                        "r" (a2), "r" (d2), "r" (d3)
                      : "cc", "memory");
    return res;
}

static LONG s_getdtablesize(struct Library *base)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");

    __asm __volatile ("jsr a6@(-138:W)"
                      : "=r" (res), "=r" (_clob_d1)
                      : "r" (a6)
                      : "a0", "a1", "cc", "memory");
    return res;
}

/* ------------------------------------------------------------- the shared -- */

/*
 * Everything the worker reports back. It lives in the parent, because a worker
 * that runs out of stack cannot be relied on to hand anything over.
 */
typedef struct StResult
{
    ULONG   sr_Asked;               /* NP_StackSize we passed             */
    ULONG   sr_Stack;               /* tc_SPUpper - tc_SPLower, measured   */
    ULONG   sr_Deepest;             /* bytes below tc_SPUpper ever used   */
    ULONG   sr_Untouched;           /* what the pattern still holds       */
    LONG    sr_Checks;
    LONG    sr_Failures;
    LONG    sr_Finished;
    LONG    sr_Bytes;               /* echoed over loopback               */
    LONG    sr_Phase;               /* how far the worker got             */
    ULONG   sr_Mark[ST_PHASES];     /* the mark as each phase ended       */
} StResult;

/*
 * The worker cannot report for itself. A Process made by CreateNewProc with no
 * NP_Output has no Output() to Printf to, so everything it "logs" goes nowhere
 * -- and a worker that overflows its stack could not be trusted to say so
 * anyway. It writes a phase number into the shared struct instead, and the
 * parent prints the phase names, including on the timeout path where the last
 * phase reached IS the failure report.
 */
static const char * const st_phase_name[ST_PHASES] =
{
    "before OpenLibrary",
    "OpenLibrary + getdtablesize",
    "socket/connect/getsockname",
    "loopback send/WaitSelect/recv rounds",
    "gethostbyname localhost (hosts file, shallow)",
    "gethostbyname .invalid (DNS)",
    "gethostbyname .local (mDNS)",
    "getservbyname",
    "getaddrinfo .invalid",
    "getnameinfo 192.0.2.1 (reverse DNS)",
    "CloseLibrary"
};

static StResult st_result;
static struct Task *st_parent;

/* ---------------------------------------------------------------- output -- */

/*
 * Flushed per line: the emulator runner reads stdout out of a file after the
 * run, so a line still sitting in a buffer is a line that does not exist if
 * the program never exits -- and never exiting is one of the two failures this
 * test is for.
 */
static VOID st_log(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    (VOID)VPrintf((STRPTR)fmt, (LONG *)ap);
    va_end(ap);
    (VOID)Flush(Output());
}

static VOID st_check(LONG ok, const char *what, LONG detail)
{
    st_result.sr_Checks++;

    if (ok)
    {
        st_log("  ok   %s\n", (LONG)what);
        return;
    }

    st_result.sr_Failures++;
    st_log("  FAIL %s (%ld)\n", (LONG)what, detail);
}

/* ------------------------------------------------------------ the painter -- */

/*
 * Paint the worker's own unused stack, then read the mark back.
 *
 * tc_SPLower/tc_SPUpper bound the block CreateNewProc allocated. Painting
 * stops a margin below the current stack pointer so this function's own frame
 * and its return address survive being written over -- taking the address of a
 * local is what the C standard gives us for "roughly where the stack is now".
 *
 * The mark is then the distance from tc_SPUpper down to the lowest word that
 * no longer holds the pattern, which counts every frame anything left behind,
 * including the vendored NetX Duo and ThreadX ones that -fstack-usage on our
 * own sources cannot see.
 */
#define ST_PAINT_MARGIN     256

static VOID st_paint(VOID)
{
    struct Task *me    = FindTask(NULL);
    ULONG        here;
    ULONG       *lo    = (ULONG *)me->tc_SPLower;
    ULONG       *stop  = (ULONG *)(((ULONG)&here) - ST_PAINT_MARGIN);
    ULONG       *p;

    if (lo == NULL || stop <= lo)
        return;

    for (p = lo; p < stop; p++)
        *p = ST_PATTERN;
}

static ULONG st_mark(VOID)
{
    struct Task *me = FindTask(NULL);
    ULONG       *lo = (ULONG *)me->tc_SPLower;
    ULONG       *hi = (ULONG *)me->tc_SPUpper;
    ULONG       *p;

    if (lo == NULL || hi == NULL)
        return 0;

    /*
     * The stack that arrived, not the one asked for. AROS rounds NP_StackSize
     * up to its own floor -- 16 KB under the ROM the emulator tier boots -- so
     * a run that reports 4096 without checking would be measuring something
     * else and calling it the Shell default.
     */
    st_result.sr_Stack = (ULONG)((UBYTE *)hi - (UBYTE *)lo);

    for (p = lo; p < hi; p++)
        if (*p != ST_PATTERN)
            break;

    st_result.sr_Untouched = (ULONG)((UBYTE *)p - (UBYTE *)lo);

    return (ULONG)((UBYTE *)hi - (UBYTE *)p);
}

/*
 * Record that a phase finished, and what the mark was when it did. The
 * per-phase marks are what turn "976 bytes somewhere" into "the resolver cost
 * this much and the send loop cost that much".
 */
static VOID st_phase(LONG which)
{
    st_result.sr_Phase = which;

    if (which >= 0 && which < ST_PHASES)
        st_result.sr_Mark[which] = st_mark();
}

/* ------------------------------------------------------------- the server -- */

/*
 * A peer, on a stack of its own choosing. It is not the thing under test --
 * the point is to give the 4 KB worker a real connection to drive, so this one
 * gets room and is not measured.
 */
#define ST_PEER_STACK   16384UL

static volatile LONG   st_peer_port;
static volatile LONG   st_peer_stop;

static VOID st_peer_entry(VOID)
{
    struct Library *base;
    LONG            fd  = -1;
    LONG            one = 1;
    StAddr          sa;
    ULONG           i;

    base = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4UL);
    if (base == NULL)
    {
        st_peer_port = -1;
        Signal(st_parent, ST_DONE_MASK);
        return;
    }

    fd = s_socket(base, S_AF_INET, S_SOCK_STREAM, 0);
    if (fd >= 0)
    {
        (VOID)s_setsockopt(base, fd, S_SOL_SOCKET, S_SO_REUSEADDR, &one,
                           (LONG)sizeof(one));

        for (i = 0; i < sizeof(sa); i++)
            ((UBYTE *)&sa)[i] = 0;
        sa.sin_len    = (UBYTE)sizeof(sa);
        sa.sin_family = S_AF_INET;
        sa.sin_port   = (UWORD)ST_PORT;
        sa.sin_addr   = 0x7F000001UL;

        if (s_bind(base, fd, &sa) != 0 || s_listen(base, fd, 4) != 0)
        {
            (VOID)s_closesocket(base, fd);
            fd = -1;
        }
    }

    st_peer_port = (fd >= 0) ? ST_PORT : -1;
    Signal(st_parent, ST_DONE_MASK);

    while (fd >= 0 && !st_peer_stop)
    {
        ULONG     rd[1];
        StTimeval tv;
        LONG      conn;

        rd[0] = 1UL << fd;
        tv.tv_secs  = 1;
        tv.tv_micro = 0;

        if (s_waitselect(base, fd + 1, rd, NULL, NULL, &tv, NULL) <= 0)
            continue;

        conn = s_accept(base, fd);
        if (conn < 0)
            continue;

        /* Echo until the far end closes. */
        for (;;)
        {
            UBYTE buf[ST_CHUNK];
            LONG  got = s_recv(base, conn, buf, (LONG)sizeof(buf));
            LONG  done = 0;

            if (got <= 0)
                break;

            while (done < got)
            {
                LONG put = s_send(base, conn, buf + done, got - done);

                if (put <= 0)
                {
                    done = got;
                    break;
                }
                done += put;
            }
        }

        (VOID)s_closesocket(base, conn);
    }

    if (fd >= 0)
        (VOID)s_closesocket(base, fd);

    CloseLibrary(base);
}

/* ------------------------------------------------------------- the worker -- */

/*
 * The whole point of the file: this runs on ST_STACK bytes and nothing else in
 * here may add to its frame, so the buffers are deliberately small and there
 * is no recursion.
 */
static VOID st_worker_entry(VOID)
{
    struct Library *base;
    LONG            fd = -1;
    ULONG           round;
    StAddr          sa;
    ULONG           i;

    st_paint();
    st_phase(0);

    base = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4UL);
    st_check(base != NULL, "OpenLibrary on a 4 KB stack", 0);
    if (base == NULL)
    {
        st_result.sr_Deepest  = st_mark();
        st_result.sr_Finished = 0;
        Signal(st_parent, ST_DONE_MASK);
        return;
    }

    st_check(s_getdtablesize(base) > 0, "getdtablesize", 0);
    st_phase(1);

    for (round = 0; round < ST_ROUNDS; round++)
    {
        UBYTE     buf[64];
        ULONG     rd[1];
        StTimeval tv;
        LONG      n;

        fd = s_socket(base, S_AF_INET, S_SOCK_STREAM, 0);
        if (fd < 0)
        {
            st_check(0, "socket", s_errno(base));
            break;
        }

        for (i = 0; i < sizeof(sa); i++)
            ((UBYTE *)&sa)[i] = 0;
        sa.sin_len    = (UBYTE)sizeof(sa);
        sa.sin_family = S_AF_INET;
        sa.sin_port   = (UWORD)ST_PORT;
        sa.sin_addr   = 0x7F000001UL;

        if (s_connect(base, fd, &sa) != 0)
        {
            st_check(0, "connect to the loopback peer", s_errno(base));
            (VOID)s_closesocket(base, fd);
            break;
        }

        {
            LONG len = (LONG)sizeof(sa);

            (VOID)s_getsockname(base, fd, &sa, &len);
        }

        if (round == 0)
            st_phase(2);

        for (i = 0; i < sizeof(buf); i++)
            buf[i] = (UBYTE)(round + i);

        n = s_send(base, fd, buf, (LONG)sizeof(buf));
        if (n != (LONG)sizeof(buf))
        {
            st_check(0, "send", s_errno(base));
            (VOID)s_closesocket(base, fd);
            break;
        }

        /* The WaitSelect loop a file copy is made of. */
        rd[0] = 1UL << fd;
        tv.tv_secs  = 5;
        tv.tv_micro = 0;

        if (s_waitselect(base, fd + 1, rd, NULL, NULL, &tv, NULL) <= 0)
        {
            st_check(0, "WaitSelect on the echo", s_errno(base));
            (VOID)s_closesocket(base, fd);
            break;
        }

        n = s_recv(base, fd, buf, (LONG)sizeof(buf));
        if (n <= 0)
        {
            st_check(0, "recv the echo", s_errno(base));
            (VOID)s_closesocket(base, fd);
            break;
        }

        st_result.sr_Bytes += n;
        (VOID)s_closesocket(base, fd);
    }

    st_check(st_result.sr_Bytes > 0, "loopback round trips completed",
             st_result.sr_Bytes);

    st_phase(3);

    /*
     * The resolver last, because it is the part that can hang.
     *
     * These lookups are meant to MISS -- "localhost" is answered out of
     * DEVS:Internet/hosts before netstack_resolve() enters the bracket at all,
     * so a run that only asked for that would measure the shallow path and
     * call it a pass. A name under .invalid (RFC 2606, guaranteed never to
     * resolve) goes the whole way to the DNS client and one under .local goes
     * down the mDNS branch, which is the deeper of the two. getnameinfo on an
     * unroutable address (RFC 5737 TEST-NET-1) takes netstack_resolve_reverse
     * the same way, and it is the deepest entry point measured.
     *
     * Failure is the expected answer; the depth reached on the way to it is
     * the measurement. Each miss waits out BSD_RESOLVE_TIMEOUT, which is why
     * this is minutes rather than seconds and why it runs after the loopback
     * rounds instead of before them -- a resolver that never comes back must
     * not take the send/recv/WaitSelect figures with it.
     */
    (VOID)s_gethostbyname(base, "localhost");
    st_phase(4);
    (VOID)s_gethostbyname(base, "no-such-host.invalid");
    st_phase(5);
    (VOID)s_gethostbyname(base, "no-such-host.local");
    st_phase(6);
    st_check(1, "gethostbyname reached the resolver three ways", 0);

    {
        UBYTE quad[4];

        /* 127.0.0.1, which DEVS:Internet/hosts answers: this is here for the
           entry point's own frame, not for the resolver below it -- the deep
           reverse path is getnameinfo's, and paying the timeout twice buys
           nothing. */
        quad[0] = 127; quad[1] = 0; quad[2] = 0; quad[3] = 1;
        (VOID)s_gethostbyaddr(base, quad, 4, S_AF_INET);
        st_check(1, "gethostbyaddr returned", 0);
    }

    (VOID)s_getservbyname(base, "telnet", "tcp");
    st_phase(7);
    st_check(1, "getservbyname returned", 0);

    {
        APTR list = NULL;

        (VOID)s_getaddrinfo(base, "no-such-host.invalid", NULL, NULL, &list);
        st_phase(8);
        st_check(1, "getaddrinfo returned", 0);
    }

    {
        char   host[64];
        char   serv[16];

        for (i = 0; i < sizeof(sa); i++)
            ((UBYTE *)&sa)[i] = 0;
        sa.sin_len    = (UBYTE)sizeof(sa);
        sa.sin_family = S_AF_INET;
        sa.sin_port   = (UWORD)23;
        sa.sin_addr   = 0xC0000201UL;           /* 192.0.2.1 */

        (VOID)s_getnameinfo(base, &sa, (LONG)sizeof(sa), host,
                            (LONG)sizeof(host), serv, (LONG)sizeof(serv), 0);
        st_phase(9);
        st_check(1, "getnameinfo returned", 0);
    }

    CloseLibrary(base);
    st_phase(10);

    st_result.sr_Deepest  = st_mark();
    st_result.sr_Finished = 1;

    Signal(st_parent, ST_DONE_MASK);
}

/* ----------------------------------------------------------------- main -- */

int main(int argc, char **argv)
{
    struct Process *peer;
    struct Process *worker;
    ULONG           waited = 0;

    (VOID)argc;
    (VOID)argv;

    st_parent = FindTask(NULL);
    st_result.sr_Asked = ST_STACK;
    st_result.sr_Phase = -1;            /* not even phase 0 yet */

    st_log("stack: the API from %lu bytes, which is the Shell default\n",
           (LONG)ST_STACK);

    SetSignal(0, ST_DONE_MASK);

    peer = CreateNewProcTags(NP_Entry,      (ULONG)st_peer_entry,
                             NP_Name,       (ULONG)"aminetxduo stack peer",
                             NP_StackSize,  ST_PEER_STACK,
                             NP_Priority,   0,
                             TAG_DONE);
    st_check(peer != NULL, "the loopback peer starts", 0);
    if (peer == NULL)
        return RETURN_FAIL;

    /*
     * The peer signals once it is listening, or once it has given up. Waited
     * on with a bound rather than Wait(): a peer that died on the way would
     * otherwise hang the whole harness with nothing printed, which is the one
     * failure mode a test about crashes must not have.
     */
    while ((SetSignal(0UL, 0UL) & ST_DONE_MASK) == 0)
    {
        Delay(10);
        waited += 10;

        if (waited >= ST_TIMEOUT_TICKS)
            break;
    }
    waited = 0;

    st_check(st_peer_port > 0, "the peer is listening", st_peer_port);
    if (st_peer_port <= 0)
    {
        st_peer_stop = 1;
        Delay(100);
        return RETURN_FAIL;
    }

    SetSignal(0, ST_DONE_MASK);

    worker = CreateNewProcTags(NP_Entry,     (ULONG)st_worker_entry,
                               NP_Name,      (ULONG)"aminetxduo stack worker",
                               NP_StackSize, ST_STACK,
                               NP_Priority,  0,
                               TAG_DONE);
    st_check(worker != NULL, "the 4 KB worker starts", 0);
    if (worker == NULL)
    {
        st_peer_stop = 1;
        Delay(100);
        return RETURN_FAIL;
    }

    /*
     * A worker that overflowed does not get as far as signalling, so the
     * timeout is the failure report rather than a formality.
     */
    while ((SetSignal(0UL, 0UL) & ST_DONE_MASK) == 0)
    {
        Delay(10);
        waited += 10;

        if (waited >= ST_TIMEOUT_TICKS)
            break;
    }

    st_peer_stop = 1;

    st_check((SetSignal(0UL, 0UL) & ST_DONE_MASK) != 0,
             "the worker came back", (LONG)waited);
    st_check(st_result.sr_Finished != 0, "the worker ran to the end", 0);

    /*
     * The per-phase marks. On a timeout this is the whole report: the last
     * phase that completed is where the worker is stuck or died, and the
     * worker has no Output() of its own to say so.
     */
    st_log("\nmark after each phase (bytes of stack used):\n");
    {
        LONG ph;

        for (ph = 0; ph < ST_PHASES; ph++)
        {
            if (ph > st_result.sr_Phase)
            {
                st_log("  ----  %s   NOT REACHED\n", (LONG)st_phase_name[ph]);
                continue;
            }
            st_log("  %4lu  %s\n", (LONG)st_result.sr_Mark[ph],
                   (LONG)st_phase_name[ph]);
        }
    }

    /* Let the peer notice the stop flag and close its listener. */
    Delay(150);

    st_log("\n");
    st_log("stack asked for %lu bytes\n", (LONG)st_result.sr_Asked);
    st_log("stack granted   %lu bytes\n", (LONG)st_result.sr_Stack);
    st_log("deepest touched %lu bytes\n", (LONG)st_result.sr_Deepest);
    st_log("still unpainted %lu bytes of headroom\n",
           (LONG)st_result.sr_Untouched);
    st_log("bytes echoed    %ld\n", st_result.sr_Bytes);

    /*
     * Reported, not asserted. AROS rounds NP_StackSize up to 16 KB, so under
     * the ROM the emulator tier boots the worker never gets the 4 KB it asked
     * for and the mark is measured against what it did get. On Kickstart the
     * two agree; a reader has to be able to tell which run this was.
     */
    if (st_result.sr_Stack > st_result.sr_Asked)
        st_log("note: this ROM rounded the request up, so the headroom below "
               "is against %lu, not %lu\n",
               (LONG)st_result.sr_Stack, (LONG)st_result.sr_Asked);

    st_check(st_result.sr_Deepest < st_result.sr_Stack,
             "the mark fits the stack that was granted",
             (LONG)st_result.sr_Deepest);

    /*
     * The number the report is really about: what the deepest call cost,
     * against the 4 KB a Shell command gets, whatever this ROM handed out.
     */
    st_check(st_result.sr_Deepest < ST_STACK,
             "the mark fits 4 KB, which is what a Shell command has",
             (LONG)st_result.sr_Deepest);

    /*
     * Headroom, not just survival. A run that touched all but a few dozen
     * bytes passed by luck: the next Enforcer hit or a slightly different
     * interrupt on the way through would have gone over. An eighth of a Shell
     * stack is the bar.
     */
    {
        ULONG spare = (st_result.sr_Deepest < ST_STACK)
                          ? (ST_STACK - st_result.sr_Deepest) : 0;

        st_check(spare >= (ST_STACK / 8),
                 "an eighth of a Shell stack was still spare", (LONG)spare);
    }

    st_log("\nstack: %ld checks, %ld failures\n",
           st_result.sr_Checks, st_result.sr_Failures);

    return (st_result.sr_Failures == 0) ? RETURN_OK : RETURN_FAIL;
}
