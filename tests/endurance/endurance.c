/*
 * Endurance -- hours of mixed TCP traffic, with the pool on a timeline.
 *
 *   The other harnesses in this tree run for seconds or minutes: tests/compare
 *   moves a few megabytes, tests/trace moves 512 KB, tests/tcpdrill moves a few
 *   hundred bytes and watches the packets.  None of them can see a defect
 *   whose argument is "bytes" or "hours".
 *
 *   A report on English Amiga Board (thread 122501) says both AmiTCP 4 and
 *   Roadshow fail after several hours of a TCP connection driven with wildly
 *   varying read/write sequences: they return EAGAIN on a *blocking* socket --
 *   which should be impossible -- after which the socket is finished and the
 *   only thing left is to close it.  The reporter blames mbuf fragmentation
 *   and sequence-number overrun.  Neither claim has ever been tested here.
 *
 *   N concurrent TCP connections, each driven by a pair of AmigaOS Processes
 *   (a driver and a responder, each with its own bsdsocket.library base,
 *   because the library is per-opener).  Every transaction picks:
 *
 *     - a direction   -- PUT (driver writes), GET (driver reads) or ECHO
 *                        (driver writes and reads the same bytes back);
 *     - a length      -- log-uniform from 1 byte to `maxxfer`, so the mix is
 *                        mostly small with a long tail, which is what copying
 *                        a directory of files over a share looks like;
 *     - a chunk size  -- redrawn log-uniform for every send() and recv() on
 *                        both sides independently, so the two ends never
 *                        agree about framing and the stack has to.
 *
 *   Occasional short-lived connections are opened and closed alongside the
 *   long-lived ones, because a per-socket leak only shows under churn.
 *
 *   The payload is a position-addressable pattern:
 *
 *       byte(o) = pat[o & 8191] ^ (UBYTE)(o >> 13)
 *
 *   `pat` is 8 KB of xorshift output, so any single altered byte is caught,
 *   and the high-bits term makes the period 2 MB, so a splice that repeats or
 *   drops a block is caught too unless the displacement is an exact multiple
 *   of 2 MB.  Every payload carries its own pattern offset in its header, so
 *   the receiver never has to trust its own bookkeeping, and a framing desync
 *   shows up immediately as a header whose magic is wrong.
 *
 *   Every `sample` seconds the supervisor appends one CSV row: packet-pool
 *   free count and the pool's own empty-request and empty-suspension counters,
 *   AvailMem total and largest contiguous block (a heap that fragments holds
 *   the first and loses the second), live socket count, TCP retransmissions,
 *   receive drops, checksum errors, interface allocation failures, and the
 *   workers' byte counters.  A timeline is what makes "it broke after forty
 *   minutes" useful: it says what the machine looked like at minute
 *   thirty-nine.
 *
 *   Every errno any worker sees is appended to a second file with the
 *   socket's blocking state at the time: EWOULDBLOCK on a non-blocking socket
 *   is the API working, and the same answer on a blocking one is the defect
 *   being hunted.
 *
 *   Both files are opened, appended and closed per line, so a run that has to
 *   be killed does not lose its last twenty lines (the reasoning in
 *   tests/compare/checkrunner.c, and in docs/RESEARCH.md 16.9).
 *
 *   The probes run first and take under a minute:
 *
 *   P1  a blocking recv() on an idle established socket must wait, not answer
 *   P2  a blocking send() into a peer that is not reading must wait, and must
 *       not come back short
 *   P3  the same blocking send() while the packet pool is exhausted by
 *       connections nobody reads -- the specific suspect, because
 *       NX_NO_PACKET means both "nothing to read" and "the pool is empty",
 *       and src/bsdsocket/errno.c maps it to EWOULDBLOCK either way
 *   P4  the same three on a non-blocking socket, as the control: EWOULDBLOCK
 *       there is correct, and a probe suite that never sees it is not testing
 *       anything
 *
 *   P3 is supervised from the main Process rather than timed inside the
 *   worker, because it distinguishes three outcomes -- "returned EAGAIN" (the
 *   reported defect), "waited and then succeeded" (correct) and "never
 *   returned at all" (a deadlock) -- and only an observer outside the call can
 *   tell the third from the second.
 *
 *   bsdsocket.library is reached through its published LVOs, as a third-party
 *   program reaches it, and the packet pool through NetStackQuery() at -0x366,
 *   which exists so that a program that has not linked src/netstack can still
 *   read the running stack (include/aminetxduo/netstatus.h).  A harness that
 *   linked the stack would get its own NX_IP with no interfaces in it and
 *   report zeroes.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "aminetxduo/netstatus.h"

static const char version_tag[] __attribute__((used)) =
    "$VER: Endurance 1.0 (26.7.2026)";

/* ------------------------------------------------------------------ LVOs -- */

/*
 * Straight out of the NDK's bsdsocket_lib.fd.  Hand-vectored rather than
 * inlined because the NDK inlines take the library base from one global, and
 * this program runs socket calls from a dozen Processes that each hold their
 * own.
 */
#define LVO_socket      (-30)
#define LVO_bind        (-36)
#define LVO_listen      (-42)
#define LVO_accept      (-48)
#define LVO_connect     (-54)
#define LVO_send        (-66)
#define LVO_recv        (-78)
#define LVO_shutdown    (-84)
#define LVO_setsockopt  (-90)
#define LVO_getsockopt  (-96)
#define LVO_IoctlSocket (-114)
#define LVO_CloseSocket (-120)
#define LVO_Errno       (-162)

_Static_assert(AMI_NETSTATUS_QUERY_LVO == -870, "NetStackQuery LVO moved");

#define E_AF_INET           2
#define E_SOCK_STREAM       1
#define E_SOL_SOCKET        0xFFFF
#define E_SO_REUSEADDR      0x0004
#define E_SO_RCVBUF         0x1002
#define E_SO_SNDBUF         0x1001
#define E_FIONBIO           0x8004667EUL

/* The errno numbers this stack uses; include/... is not on this path. */
#define E_EWOULDBLOCK       35
#define E_EINTR             4
#define E_ENOBUFS           55
#define E_EPIPE             32

typedef struct EndAddr
{
    UBYTE   sin_len;
    UBYTE   sin_family;
    UWORD   sin_port;
    ULONG   sin_addr;
    UBYTE   sin_zero[8];
} EndAddr;

static LONG e_socket(struct Library *base, LONG dom, LONG type, LONG proto)
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

static LONG e_bind(struct Library *base, LONG s, EndAddr *sa)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = sa;
    register LONG            d1  __asm("d1") = (LONG)sizeof(EndAddr);
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-36:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1)
                      : "a1", "cc", "memory");
    return res;
}

static LONG e_listen(struct Library *base, LONG s, LONG backlog)
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

static LONG e_accept(struct Library *base, LONG s)
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

static LONG e_connect(struct Library *base, LONG s, EndAddr *sa)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = sa;
    register LONG            d1  __asm("d1") = (LONG)sizeof(EndAddr);
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-54:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1)
                      : "a1", "cc", "memory");
    return res;
}

static LONG e_send(struct Library *base, LONG s, const UBYTE *buf, LONG len,
                   LONG flags)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = (APTR)buf;
    register LONG            d1  __asm("d1") = len;
    register LONG            d2  __asm("d2") = flags;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-66:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1), "r" (d2)
                      : "a1", "cc", "memory");
    return res;
}

static LONG e_recv(struct Library *base, LONG s, UBYTE *buf, LONG len,
                   LONG flags)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = buf;
    register LONG            d1  __asm("d1") = len;
    register LONG            d2  __asm("d2") = flags;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-78:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1), "r" (d2)
                      : "a1", "cc", "memory");
    return res;
}

static LONG e_setsockopt(struct Library *base, LONG s, LONG level, LONG name,
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

static LONG e_ioctl(struct Library *base, LONG s, ULONG req, APTR argp)
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

static LONG e_close(struct Library *base, LONG s)
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

static LONG e_errno(struct Library *base)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            res __asm("d0");

    __asm __volatile ("jsr a6@(-162:W)"
                      : "=r" (res)
                      : "r" (a6)
                      : "d1", "a0", "a1", "cc", "memory");
    return res;
}

static LONG e_query(struct Library *base, ULONG what, APTR buf, ULONG size)
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

/* ----------------------------------------------------------- the pattern -- */

#define PAT_BYTES   8192UL
#define PAT_MASK    (PAT_BYTES - 1UL)
#define PAT_SHIFT   13              /* log2(PAT_BYTES) */

static UBYTE end_pat[PAT_BYTES];

static ULONG end_xs(ULONG *s)
{
    ULONG x = *s;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;

    return x;
}

static VOID end_pat_init(ULONG seed)
{
    ULONG s = (seed != 0UL) ? seed : 0x9E3779B9UL;
    ULONG i;

    for (i = 0; i < PAT_BYTES; i++)
        end_pat[i] = (UBYTE)(end_xs(&s) >> 19);
}

/* Write `len` pattern bytes for stream offset `off`. */
static VOID end_pat_fill(UBYTE *dst, ULONG off, ULONG len)
{
    while (len > 0UL)
    {
        ULONG        idx = off & PAT_MASK;
        ULONG        run = PAT_BYTES - idx;
        UBYTE        k   = (UBYTE)(off >> PAT_SHIFT);
        const UBYTE *src = &end_pat[idx];
        ULONG        i;

        if (run > len)
            run = len;

        for (i = 0; i < run; i++)
            dst[i] = (UBYTE)(src[i] ^ k);

        dst += run;
        off += run;
        len -= run;
    }
}

/*
 * Check `len` bytes against the pattern.  Returns the count of mismatched
 * bytes and, when there is at least one, reports the stream offset of the
 * first through `first`.
 */
static ULONG end_pat_check(const UBYTE *buf, ULONG off, ULONG len,
                           ULONG *first, UBYTE *got, UBYTE *want)
{
    ULONG bad = 0UL;

    while (len > 0UL)
    {
        ULONG        idx = off & PAT_MASK;
        ULONG        run = PAT_BYTES - idx;
        UBYTE        k   = (UBYTE)(off >> PAT_SHIFT);
        const UBYTE *src = &end_pat[idx];
        ULONG        i;

        if (run > len)
            run = len;

        for (i = 0; i < run; i++)
        {
            UBYTE expect = (UBYTE)(src[i] ^ k);

            if (buf[i] != expect)
            {
                if (bad == 0UL)
                {
                    if (first != NULL) *first = off + i;
                    if (got   != NULL) *got   = buf[i];
                    if (want  != NULL) *want  = expect;
                }
                bad++;
            }
        }

        buf += run;
        off += run;
        len -= run;
    }

    return bad;
}

/* ------------------------------------------------------------- the clock -- */

static ULONG end_t0_ticks;

/* Ticks (1/50 s) since the program started.  DateStamp() needs no device and
   the guest's clock is not touched during a run. */
static ULONG end_ticks(VOID)
{
    struct DateStamp ds;
    ULONG            t;

    DateStamp(&ds);

    t = ((ULONG)ds.ds_Days % 86400UL) * 4320000UL
        + (ULONG)ds.ds_Minute * 3000UL
        + (ULONG)ds.ds_Tick;

    return t - end_t0_ticks;
}

static ULONG end_secs(VOID)
{
    return end_ticks() / 50UL;
}

/* -------------------------------------------------------- shared results -- */

#define END_MAX_CONNS       6
/*
 * Ten pairs, not six.  A hog pair pins about a socket's transmit queue
 * (NX_TCP_MAXIMUM_TX_QUEUE, 20) plus the receiver's advertised window
 * (5 to 20 packets, per S24.3), so six pairs took the pool from 256 free to
 * 171 and P3 had nothing to test.  Ten pairs, with the transmitter blocking
 * so it parks holding a full queue rather than backing off, is the smallest
 * arrangement that reaches the floor.
 */
#define END_MAX_HOGS        10
#define END_MAX_WORKERS     (2 * END_MAX_CONNS + 2 * END_MAX_HOGS + 2)

#define ROLE_DRIVER     0
#define ROLE_RESPONDER  1
#define ROLE_HOGTX      2
#define ROLE_HOGRX      3
#define ROLE_PROBE      4
#define ROLE_FILER      5
#define ROLE_LEAKER     6

/* mode= */
#define MODE_LOOP       0       /* synthetic, both ends in this machine     */
#define MODE_WIRE       1       /* synthetic, the far end is a host peer    */
#define MODE_FITZ       2       /* files over a mounted Fitz share          */
#define MODE_WATCH      3       /* sample only; somebody else makes traffic */
#define MODE_LEAK       4       /* one socket lifecycle, over and over      */

/* Per-worker counters.  Only the owning Process writes them; the supervisor
   only reads, so no lock is needed for these. */
typedef struct EndWorker
{
    UWORD           w_Role;
    UWORD           w_Conn;
    volatile UWORD  w_Alive;
    volatile UWORD  w_Done;

    volatile ULONG  w_BytesTx;
    volatile ULONG  w_BytesRx;
    volatile ULONG  w_MegaTx;       /* w_BytesTx wraps; this counts the wraps */
    volatile ULONG  w_MegaRx;
    volatile ULONG  w_Xacts;
    volatile ULONG  w_Conns;        /* connections opened by this worker      */
    volatile ULONG  w_Errors;
    volatile ULONG  w_Mismatch;
    volatile ULONG  w_Desync;

    /* The stall detector: the tick at which the worker entered its current
       socket call, and which call it is.  0 means "not in a call". */
    volatile ULONG  w_CallStart;
    volatile UWORD  w_CallKind;     /* 1 send, 2 recv, 3 connect, 4 accept    */
    volatile UWORD  w_CallBlocking; /* 1 blocking, 0 non-blocking             */
    volatile ULONG  w_Beat;         /* tick of the last completed transaction */

    struct Library *w_Base;
    UBYTE          *w_Buf;
    ULONG           w_Seed;
    struct Process *w_Proc;
} EndWorker;

typedef struct EndState
{
    volatile UWORD  es_Stop;
    volatile UWORD  es_HogGo;       /* hogs may start blasting                */
    volatile UWORD  es_HogStop;
    volatile UWORD  es_ProbeGo;     /* the P3 probe may fire                  */
    volatile UWORD  es_ProbeState;  /* 0 idle, 1 in call, 2 returned          */
    volatile LONG   es_ProbeRc;
    volatile LONG   es_ProbeErrno;
    volatile ULONG  es_ProbeStart;
    volatile ULONG  es_ProbeEnd;
    volatile UWORD  es_ProbeBlocking;

    EndWorker       es_W[END_MAX_WORKERS];
    UWORD           es_Workers;

    /* Config. */
    UWORD           es_Mode;
    UWORD           es_Wire;        /* derived: es_Mode == MODE_WIRE         */
    UWORD           es_Filers;
    char            es_Path[64];    /* the mounted share, e.g. "FITZ:"       */
    char            es_Scratch[64]; /* local staging, e.g. "RAM:endz"        */
    UWORD           es_Conns;
    UWORD           es_Hogs;
    UWORD           es_Port;
    ULONG           es_Peer;
    ULONG           es_Seconds;
    ULONG           es_Sample;
    ULONG           es_MaxIo;
    ULONG           es_MaxXfer;
    ULONG           es_MaxEcho;
    ULONG           es_Churn;       /* short-lived connection every N xacts   */
    ULONG           es_NbEvery;     /* non-blocking excursion every N xacts   */
    ULONG           es_Seed;
    UWORD           es_Probes;
} EndState;

static EndState *ES;

/* --------------------------------------------------------------- output --- */

#define F_TIMELINE  "DH0:end-timeline.csv"
#define F_EVENTS    "DH0:end-events.txt"
#define F_SUMMARY   "DH0:end-summary.txt"

static struct SignalSemaphore end_out_sem;

/*
 * Append one formatted line and close.  One open/close per line is expensive,
 * but a run that has to be killed keeps everything written so far.
 */
static VOID end_emit(const char *file, const char *fmt, LONG *args)
{
    BPTR out;

    ObtainSemaphore(&end_out_sem);

    out = Open((CONST_STRPTR)file, MODE_READWRITE);
    if (out != (BPTR)0)
    {
        Seek(out, 0, OFFSET_END);
        VFPrintf(out, (CONST_STRPTR)fmt, args);
        Close(out);
    }

    ReleaseSemaphore(&end_out_sem);
}

static VOID end_truncate(const char *file)
{
    BPTR out = Open((CONST_STRPTR)file, MODE_NEWFILE);

    if (out != (BPTR)0)
        Close(out);
}

/*
 * One errno, with the socket's blocking state next to it: that pairing is the
 * question this harness answers, so it is recorded rather than reconstructed
 * later.
 */
static VOID end_event(EndWorker *w, const char *what, LONG rc, LONG err,
                      UWORD blocking, ULONG detail)
{
    LONG args[8];

    args[0] = (LONG)end_secs();
    args[1] = (LONG)w->w_Role;
    args[2] = (LONG)w->w_Conn;
    args[3] = (LONG)what;
    args[4] = rc;
    args[5] = err;
    args[6] = (LONG)blocking;
    args[7] = (LONG)detail;

    end_emit(F_EVENTS, "%ld role=%ld conn=%ld %s rc=%ld errno=%ld blocking=%ld "
                       "detail=%lu\n", args);
}

/* ------------------------------------------------------------ statistics -- */

typedef struct
{
    NetStatusHeader hdr;
    NetStatusSystem e;
} EndSysBuf;

typedef struct
{
    NetStatusHeader hdr;
    NetStatusStats  e;
} EndStatBuf;

typedef struct
{
    NetStatusHeader     hdr;
    NetStatusInterface  e[4];
} EndIfBuf;

typedef struct
{
    NetStatusHeader hdr;
    NetStatusSocket e[48];
} EndSockBuf;

static VOID end_zero(APTR p, ULONG n)
{
    UBYTE *b = (UBYTE *)p;

    while (n-- > 0UL)
        *b++ = 0;
}

static LONG end_query(struct Library *base, ULONG what, APTR buf, ULONG size)
{
    NetStatusHeader *h = (NetStatusHeader *)buf;

    end_zero(buf, size);
    h->nsh_Magic   = AMI_NETSTATUS_MAGIC;
    h->nsh_Version = (UWORD)AMI_NETSTATUS_VERSION;

    return e_query(base, what, buf, size);
}

/* ------------------------------------------------------------- protocol --- */

#define END_MAGIC       0xE7D1U
#define END_HDR         12

#define OP_PUT      1
#define OP_GET      2
#define OP_ECHO     3
#define OP_BYE      4
#define OP_ACK      5
#define OP_DATA     6

static VOID end_put32(UBYTE *p, ULONG v)
{
    p[0] = (UBYTE)(v >> 24);
    p[1] = (UBYTE)(v >> 16);
    p[2] = (UBYTE)(v >>  8);
    p[3] = (UBYTE)v;
}

static ULONG end_get32(const UBYTE *p)
{
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) |
           ((ULONG)p[2] <<  8) | (ULONG)p[3];
}

static VOID end_hdr_put(UBYTE *p, UWORD op, ULONG len, ULONG off)
{
    p[0] = (UBYTE)(END_MAGIC >> 8);
    p[1] = (UBYTE)END_MAGIC;
    p[2] = (UBYTE)op;
    p[3] = 0;
    end_put32(p + 4, len);
    end_put32(p + 8, off);
}

/* ------------------------------------------------------- socket plumbing -- */

static VOID end_bump_tx(EndWorker *w, ULONG n)
{
    ULONG before = w->w_BytesTx;

    w->w_BytesTx = before + n;
    if (w->w_BytesTx < before)
        w->w_MegaTx = w->w_MegaTx + 1UL;
}

static VOID end_bump_rx(EndWorker *w, ULONG n)
{
    ULONG before = w->w_BytesRx;

    w->w_BytesRx = before + n;
    if (w->w_BytesRx < before)
        w->w_MegaRx = w->w_MegaRx + 1UL;
}

/* A log-uniform draw in 1..limit: mostly small, with a long tail. */
static ULONG end_pick(ULONG *seed, ULONG limit)
{
    ULONG r    = end_xs(seed);
    ULONG bits = 1UL + (r % 20UL);
    ULONG span = (bits >= 31UL) ? 0x7FFFFFFFUL : (1UL << bits);
    ULONG v;

    if (span > limit)
        span = limit;

    v = 1UL + (end_xs(seed) % span);

    return v;
}

/*
 * send() the whole buffer, however many calls that takes, with a fresh chunk
 * size per call.  Returns 0, or -1 with the event already recorded.
 */
static LONG end_send_all(EndWorker *w, LONG s, const UBYTE *buf, ULONG len,
                         UWORD blocking)
{
    ULONG done = 0UL;

    while (done < len)
    {
        ULONG chunk = end_pick(&w->w_Seed, ES->es_MaxIo);
        LONG  rc;

        if (chunk > len - done)
            chunk = len - done;

        w->w_CallStart    = end_ticks() | 1UL;
        w->w_CallKind     = 1;
        w->w_CallBlocking = blocking;

        rc = e_send(w->w_Base, s, buf + done, (LONG)chunk, 0);

        w->w_CallStart = 0UL;

        if (rc > 0)
        {
            done += (ULONG)rc;
            end_bump_tx(w, (ULONG)rc);
            continue;
        }

        {
            LONG err = e_errno(w->w_Base);

            w->w_Errors = w->w_Errors + 1UL;
            end_event(w, "send", rc, err, blocking, done);

            if (!blocking && err == E_EWOULDBLOCK)
            {
                Delay(1);
                continue;
            }

            return -1;
        }
    }

    return 0;
}

/*
 * recv() exactly `len` bytes, with a fresh chunk size per call.  Returns 0,
 * or -1 (event recorded).  A clean end-of-file mid-record is a failure here:
 * the peer promised these bytes in a header it already sent.
 */
static LONG end_recv_all(EndWorker *w, LONG s, UBYTE *buf, ULONG len,
                         UWORD blocking)
{
    ULONG done = 0UL;

    while (done < len)
    {
        ULONG chunk = end_pick(&w->w_Seed, ES->es_MaxIo);
        LONG  rc;

        if (chunk > len - done)
            chunk = len - done;

        w->w_CallStart    = end_ticks() | 1UL;
        w->w_CallKind     = 2;
        w->w_CallBlocking = blocking;

        rc = e_recv(w->w_Base, s, buf + done, (LONG)chunk, 0);

        w->w_CallStart = 0UL;

        if (rc > 0)
        {
            done += (ULONG)rc;
            end_bump_rx(w, (ULONG)rc);
            continue;
        }

        if (rc == 0)
        {
            w->w_Errors = w->w_Errors + 1UL;
            end_event(w, "recv-eof", 0, 0, blocking, done);
            return -1;
        }

        {
            LONG err = e_errno(w->w_Base);

            w->w_Errors = w->w_Errors + 1UL;
            end_event(w, "recv", rc, err, blocking, done);

            if (!blocking && err == E_EWOULDBLOCK)
            {
                Delay(1);
                continue;
            }

            return -1;
        }
    }

    return 0;
}

/* Receive `len` bytes and verify them against the pattern at `off`. */
static LONG end_recv_verify(EndWorker *w, LONG s, ULONG len, ULONG off,
                            UWORD blocking)
{
    ULONG done = 0UL;

    while (done < len)
    {
        ULONG want = len - done;
        ULONG bad, first = 0UL;
        UBYTE got = 0, expect = 0;

        if (want > ES->es_MaxIo)
            want = ES->es_MaxIo;

        if (end_recv_all(w, s, w->w_Buf, want, blocking) != 0)
            return -1;

        bad = end_pat_check(w->w_Buf, off + done, want, &first, &got, &expect);
        if (bad != 0UL)
        {
            LONG args[8];

            w->w_Mismatch = w->w_Mismatch + bad;

            args[0] = (LONG)end_secs();
            args[1] = (LONG)w->w_Role;
            args[2] = (LONG)w->w_Conn;
            args[3] = (LONG)bad;
            args[4] = (LONG)first;
            args[5] = (LONG)got;
            args[6] = (LONG)expect;
            args[7] = (LONG)want;
            end_emit(F_EVENTS, "%ld role=%ld conn=%ld PAYLOAD-CORRUPT bad=%lu "
                               "firstoff=%lu got=%02lx want=%02lx run=%lu\n",
                     args);
        }

        done += want;
    }

    return 0;
}

/* Send `len` pattern bytes starting at stream offset `off`. */
static LONG end_send_pattern(EndWorker *w, LONG s, ULONG len, ULONG off,
                             UWORD blocking)
{
    ULONG done = 0UL;

    while (done < len)
    {
        ULONG run = len - done;

        if (run > ES->es_MaxIo)
            run = ES->es_MaxIo;

        end_pat_fill(w->w_Buf, off + done, run);

        if (end_send_all(w, s, w->w_Buf, run, blocking) != 0)
            return -1;

        done += run;
    }

    return 0;
}

static LONG end_set_nonblock(EndWorker *w, LONG s, LONG on)
{
    LONG v = on;

    return e_ioctl(w->w_Base, s, E_FIONBIO, &v);
}

static LONG end_new_socket(EndWorker *w)
{
    LONG s = e_socket(w->w_Base, E_AF_INET, E_SOCK_STREAM, 0);

    if (s < 0)
    {
        w->w_Errors = w->w_Errors + 1UL;
        end_event(w, "socket", s, e_errno(w->w_Base), 1, 0UL);
    }

    return s;
}

/* ------------------------------------------------------------- responder -- */

/*
 * Serve one connection until the driver says goodbye.  Every read and every
 * write picks its own size, independently of whatever the driver chose, which
 * is the "wildly varying read/write sequences" half of the reported workload.
 */
static VOID end_serve(EndWorker *w, LONG s)
{
    ULONG out_off = 0UL;

    for (;;)
    {
        UBYTE hdr[END_HDR];
        UWORD op;
        ULONG len, off;

        if (end_recv_all(w, s, hdr, END_HDR, 1) != 0)
            return;

        if ((((UWORD)hdr[0] << 8) | hdr[1]) != END_MAGIC)
        {
            w->w_Desync = w->w_Desync + 1UL;
            end_event(w, "FRAMING-DESYNC", 0,
                      (LONG)(((ULONG)hdr[0] << 8) | hdr[1]), 1, 0UL);
            return;
        }

        op  = hdr[2];
        len = end_get32(hdr + 4);
        off = end_get32(hdr + 8);

        if (op == OP_BYE)
        {
            end_hdr_put(hdr, OP_ACK, 0UL, 0UL);
            (VOID)end_send_all(w, s, hdr, END_HDR, 1);
            return;
        }

        if (op == OP_PUT)
        {
            if (end_recv_verify(w, s, len, off, 1) != 0)
                return;

            end_hdr_put(hdr, OP_ACK, len, w->w_Mismatch);
            if (end_send_all(w, s, hdr, END_HDR, 1) != 0)
                return;
        }
        else if (op == OP_GET)
        {
            end_hdr_put(hdr, OP_DATA, len, out_off);
            if (end_send_all(w, s, hdr, END_HDR, 1) != 0)
                return;
            if (end_send_pattern(w, s, len, out_off, 1) != 0)
                return;
            out_off += len;
        }
        else if (op == OP_ECHO)
        {
            /*
             * Drain the whole record before echoing a byte of it.  Echoing as
             * it arrives would deadlock: the driver is still writing, and two
             * blocking sockets whose windows are both full never recover.
             * es_MaxEcho bounds the buffer.
             */
            if (len > ES->es_MaxEcho)
                len = ES->es_MaxEcho;

            if (end_recv_all(w, s, w->w_Buf, len, 1) != 0)
                return;
            if (end_send_all(w, s, w->w_Buf, len, 1) != 0)
                return;
        }
        else
        {
            end_event(w, "BAD-OP", 0, (LONG)op, 1, 0UL);
            return;
        }

        w->w_Xacts = w->w_Xacts + 1UL;
        w->w_Beat  = end_ticks() | 1UL;
    }
}

static VOID end_responder_body(EndWorker *w)
{
    EndAddr sa;
    LONG    ls, on = 1;

    ls = end_new_socket(w);
    if (ls < 0)
        return;

    (VOID)e_setsockopt(w->w_Base, ls, E_SOL_SOCKET, E_SO_REUSEADDR,
                       &on, (LONG)sizeof(on));

    end_zero(&sa, sizeof(sa));
    sa.sin_len    = (UBYTE)sizeof(sa);
    sa.sin_family = E_AF_INET;
    sa.sin_port   = (UWORD)(ES->es_Port + w->w_Conn);
    sa.sin_addr   = 0x7F000001UL;

    if (e_bind(w->w_Base, ls, &sa) < 0)
    {
        end_event(w, "bind", -1, e_errno(w->w_Base), 1, (ULONG)sa.sin_port);
        (VOID)e_close(w->w_Base, ls);
        return;
    }

    if (e_listen(w->w_Base, ls, 4) < 0)
    {
        end_event(w, "listen", -1, e_errno(w->w_Base), 1, 0UL);
        (VOID)e_close(w->w_Base, ls);
        return;
    }

    while (!ES->es_Stop)
    {
        LONG s;

        w->w_CallStart    = end_ticks() | 1UL;
        w->w_CallKind     = 4;
        w->w_CallBlocking = 1;

        s = e_accept(w->w_Base, ls);

        w->w_CallStart = 0UL;

        if (s < 0)
        {
            LONG err = e_errno(w->w_Base);

            w->w_Errors = w->w_Errors + 1UL;
            end_event(w, "accept", s, err, 1, 0UL);

            if (ES->es_Stop)
                break;

            Delay(10);
            continue;
        }

        w->w_Conns = w->w_Conns + 1UL;
        end_serve(w, s);
        (VOID)e_close(w->w_Base, s);
    }

    (VOID)e_close(w->w_Base, ls);
}

/* ---------------------------------------------------------------- driver -- */

static LONG end_dial(EndWorker *w)
{
    EndAddr sa;
    LONG    s = end_new_socket(w);

    if (s < 0)
        return -1;

    end_zero(&sa, sizeof(sa));
    sa.sin_len    = (UBYTE)sizeof(sa);
    sa.sin_family = E_AF_INET;
    sa.sin_port   = (UWORD)(ES->es_Port + w->w_Conn);
    sa.sin_addr   = ES->es_Wire ? ES->es_Peer : 0x7F000001UL;

    w->w_CallStart    = end_ticks() | 1UL;
    w->w_CallKind     = 3;
    w->w_CallBlocking = 1;

    if (e_connect(w->w_Base, s, &sa) < 0)
    {
        w->w_CallStart = 0UL;
        w->w_Errors    = w->w_Errors + 1UL;
        end_event(w, "connect", -1, e_errno(w->w_Base), 1,
                  (ULONG)sa.sin_port);
        (VOID)e_close(w->w_Base, s);
        return -1;
    }

    w->w_CallStart = 0UL;
    w->w_Conns     = w->w_Conns + 1UL;

    return s;
}

/*
 * One transaction.  Returns 0 to carry on, -1 to drop the connection and
 * redial (which is itself recorded, because a connection this harness had to
 * abandon is the reported symptom).
 */
static LONG end_transact(EndWorker *w, LONG s, ULONG *out_off, UWORD blocking)
{
    UBYTE hdr[END_HDR];
    ULONG len  = end_pick(&w->w_Seed, ES->es_MaxXfer);
    ULONG pick = end_xs(&w->w_Seed) % 100UL;

    if (pick < 40UL)                    /* PUT: driver writes */
    {
        end_hdr_put(hdr, OP_PUT, len, *out_off);
        if (end_send_all(w, s, hdr, END_HDR, blocking) != 0)
            return -1;
        if (end_send_pattern(w, s, len, *out_off, blocking) != 0)
            return -1;
        *out_off += len;

        if (end_recv_all(w, s, hdr, END_HDR, blocking) != 0)
            return -1;
        if ((((UWORD)hdr[0] << 8) | hdr[1]) != END_MAGIC || hdr[2] != OP_ACK)
        {
            w->w_Desync = w->w_Desync + 1UL;
            end_event(w, "FRAMING-DESYNC", 0,
                      (LONG)(((ULONG)hdr[0] << 8) | hdr[1]), blocking, 0UL);
            return -1;
        }
    }
    else if (pick < 80UL)               /* GET: driver reads */
    {
        ULONG dlen, doff;

        end_hdr_put(hdr, OP_GET, len, 0UL);
        if (end_send_all(w, s, hdr, END_HDR, blocking) != 0)
            return -1;

        if (end_recv_all(w, s, hdr, END_HDR, blocking) != 0)
            return -1;
        if ((((UWORD)hdr[0] << 8) | hdr[1]) != END_MAGIC || hdr[2] != OP_DATA)
        {
            w->w_Desync = w->w_Desync + 1UL;
            end_event(w, "FRAMING-DESYNC", 0,
                      (LONG)(((ULONG)hdr[0] << 8) | hdr[1]), blocking, 0UL);
            return -1;
        }

        dlen = end_get32(hdr + 4);
        doff = end_get32(hdr + 8);

        if (dlen != len)
        {
            end_event(w, "LEN-MISMATCH", (LONG)dlen, (LONG)len, blocking, 0UL);
            return -1;
        }

        if (end_recv_verify(w, s, dlen, doff, blocking) != 0)
            return -1;
    }
    else                                /* ECHO: both directions, one record */
    {
        if (len > ES->es_MaxEcho)
            len = ES->es_MaxEcho;

        end_hdr_put(hdr, OP_ECHO, len, *out_off);
        if (end_send_all(w, s, hdr, END_HDR, blocking) != 0)
            return -1;
        if (end_send_pattern(w, s, len, *out_off, blocking) != 0)
            return -1;

        if (end_recv_verify(w, s, len, *out_off, blocking) != 0)
            return -1;

        *out_off += len;
    }

    w->w_Xacts = w->w_Xacts + 1UL;
    w->w_Beat  = end_ticks() | 1UL;

    return 0;
}

static VOID end_say_bye(EndWorker *w, LONG s)
{
    UBYTE hdr[END_HDR];

    end_hdr_put(hdr, OP_BYE, 0UL, 0UL);
    if (end_send_all(w, s, hdr, END_HDR, 1) == 0)
        (VOID)end_recv_all(w, s, hdr, END_HDR, 1);
}

static VOID end_driver_body(EndWorker *w)
{
    ULONG out_off = 0UL;
    ULONG since   = 0UL;
    LONG  s;

    /* Give the responder time to bind and listen. */
    Delay(25);

    for (;;)
    {
        if (ES->es_Stop)
            return;

        s = end_dial(w);
        if (s < 0)
        {
            Delay(50);
            continue;
        }

        while (!ES->es_Stop)
        {
            UWORD blocking = 1;

            /*
             * A non-blocking excursion every so often, as the control:
             * EWOULDBLOCK here is the API working, and a run in which it never
             * appears has not exercised the path the blocking case is compared
             * against.
             */
            if (ES->es_NbEvery != 0UL && (since % ES->es_NbEvery) == 0UL &&
                since != 0UL)
            {
                if (end_set_nonblock(w, s, 1) == 0)
                    blocking = 0;
            }

            if (end_transact(w, s, &out_off, blocking) != 0)
            {
                if (!blocking)
                    (VOID)end_set_nonblock(w, s, 0);
                break;
            }

            if (!blocking)
                (VOID)end_set_nonblock(w, s, 0);

            since++;

            /*
             * A short-lived connection alongside the long-lived one: open,
             * one small transaction, close.  Socket churn is the only thing
             * that makes a per-socket leak visible, and the reported workload
             * (many small files over a share) is mostly this.
             */
            if (ES->es_Churn != 0UL && (since % ES->es_Churn) == 0UL)
            {
                LONG t = end_dial(w);

                if (t >= 0)
                {
                    ULONG toff = 0UL;

                    (VOID)end_transact(w, t, &toff, 1);
                    end_say_bye(w, t);
                    (VOID)e_close(w->w_Base, t);
                }
            }
        }

        end_say_bye(w, s);
        (VOID)e_close(w->w_Base, s);
    }
}

/* ------------------------------------------------------------------ hogs -- */

/*
 * The pool-exhaustion pair.  HOGRX accepts and then reads nothing; HOGTX
 * blasts at it.  The bytes pile up in the receiver's socket queue, and since
 * NX_TCP_MAXIMUM_RX_QUEUE is not defined in port/netxduo-amiga/inc/nx_user.h
 * (docs/RESEARCH.md 24.7) the only bound on that queue is the advertised
 * window, so a handful of these takes the packet pool down.
 *
 * That is the state in which the suspect mapping matters: NX_NO_PACKET means
 * both "nothing to read" and "the pool is empty", and src/bsdsocket/errno.c
 * maps it to EWOULDBLOCK without asking which.
 */
static VOID end_hogrx_body(EndWorker *w)
{
    EndAddr sa;
    LONG    ls, on = 1;

    ls = end_new_socket(w);
    if (ls < 0)
        return;

    (VOID)e_setsockopt(w->w_Base, ls, E_SOL_SOCKET, E_SO_REUSEADDR,
                       &on, (LONG)sizeof(on));

    end_zero(&sa, sizeof(sa));
    sa.sin_len    = (UBYTE)sizeof(sa);
    sa.sin_family = E_AF_INET;
    sa.sin_port   = (UWORD)(ES->es_Port + 100U + w->w_Conn);
    sa.sin_addr   = 0x7F000001UL;

    if (e_bind(w->w_Base, ls, &sa) < 0 || e_listen(w->w_Base, ls, 2) < 0)
    {
        end_event(w, "hogrx-bind", -1, e_errno(w->w_Base), 1,
                  (ULONG)sa.sin_port);
        (VOID)e_close(w->w_Base, ls);
        return;
    }

    while (!ES->es_HogStop && !ES->es_Stop)
    {
        LONG s = e_accept(w->w_Base, ls);

        if (s < 0)
        {
            Delay(10);
            continue;
        }

        w->w_Conns = w->w_Conns + 1UL;

        /* Hold it open, read nothing, until told to let go. */
        while (!ES->es_HogStop && !ES->es_Stop)
            Delay(10);

        (VOID)e_close(w->w_Base, s);
    }

    (VOID)e_close(w->w_Base, ls);
}

static VOID end_hogtx_body(EndWorker *w)
{
    EndAddr sa;
    LONG    s;

    while (!ES->es_HogGo && !ES->es_HogStop && !ES->es_Stop)
        Delay(5);

    if (ES->es_HogStop || ES->es_Stop)
        return;

    s = end_new_socket(w);
    if (s < 0)
        return;

    end_zero(&sa, sizeof(sa));
    sa.sin_len    = (UBYTE)sizeof(sa);
    sa.sin_family = E_AF_INET;
    sa.sin_port   = (UWORD)(ES->es_Port + 100U + w->w_Conn);
    sa.sin_addr   = 0x7F000001UL;

    if (e_connect(w->w_Base, s, &sa) < 0)
    {
        end_event(w, "hogtx-connect", -1, e_errno(w->w_Base), 1, 0UL);
        (VOID)e_close(w->w_Base, s);
        return;
    }

    /*
     * Blocking, deliberately: a non-blocking hog backs off the moment the
     * peer's window closes and gives its packets straight back, while a
     * blocking one suspends inside nx_tcp_socket_send() holding a full
     * transmit queue, which keeps packets out of the pool.  Measured: six
     * non-blocking pairs reached a floor of 171 free of 256 and P3 had
     * nothing to test.
     */
    end_pat_fill(w->w_Buf, 0UL, 4096UL);

    while (!ES->es_HogStop && !ES->es_Stop)
    {
        LONG rc = e_send(w->w_Base, s, w->w_Buf, 4096, 0);

        if (rc > 0)
            end_bump_tx(w, (ULONG)rc);
        else
            break;              /* the connection is gone; stop, do not spin */
    }

    /* Hold the queue until told to let go. */
    while (!ES->es_HogStop && !ES->es_Stop)
        Delay(10);

    (VOID)e_close(w->w_Base, s);
}

/* -------------------------------------------------------------- the leaker -- */

/*
 * One socket lifecycle, over and over, and nothing else.
 *
 * The first run of the loopback arm showed AvailMem falling by a steady
 * 1009 bytes a second and NETSTATUS_SOCKETS climbing by two a second and
 * never coming down, while the workload was also failing for an unrelated
 * reason, so that run said only that something in it leaked.
 *
 * This mode narrows it to the smallest repeatable step: create a socket, put
 * it through one lifecycle, close it.  Two kinds, chosen so the difference
 * between them isolates the cause:
 *
 *   conn 0  connect() to a port with nothing on it (ECONNREFUSED), close
 *   conn 1  connect() to a listener, exchange four bytes, close both ends
 *
 * If both leak, closing leaks.  If only the refused one does, a connection
 * that was never established is the case that leaks.
 */
static VOID end_leaker_body(EndWorker *w)
{
    UWORD refused = (w->w_Conn == 0);
    UBYTE four[4];
    ULONG round = 0UL;

    Delay(60);

    while (!ES->es_Stop)
    {
        EndAddr sa;
        LONG    s;

        s = e_socket(w->w_Base, E_AF_INET, E_SOCK_STREAM, 0);
        if (s < 0)
        {
            /* Out of descriptors is itself the answer, so say so once and
               stop rather than spinning. */
            end_event(w, "LEAK-socket", s, e_errno(w->w_Base), 1, round);
            return;
        }

        end_zero(&sa, sizeof(sa));
        sa.sin_len    = (UBYTE)sizeof(sa);
        sa.sin_family = E_AF_INET;
        sa.sin_addr   = 0x7F000001UL;
        sa.sin_port   = refused ? (UWORD)(ES->es_Port + 210U)
                                : (UWORD)(ES->es_Port + 1U);

        if (e_connect(w->w_Base, s, &sa) < 0)
        {
            LONG err = e_errno(w->w_Base);

            if (refused)
            {
                w->w_Xacts = w->w_Xacts + 1UL;
            }
            else
            {
                w->w_Errors = w->w_Errors + 1UL;
                if ((round % 64UL) == 0UL)
                    end_event(w, "LEAK-connect", -1, err, 1, round);
            }
        }
        else if (!refused)
        {
            end_pat_fill(four, 0UL, 4UL);
            if (e_send(w->w_Base, s, four, 4, 0) == 4)
            {
                UBYTE hdr[END_HDR];

                /* The responder answers a BYE with an ACK; anything shorter
                   just closes, which is fine -- this is a lifecycle test, not
                   a protocol test. */
                (VOID)e_recv(w->w_Base, s, hdr, (LONG)sizeof(hdr), 0);
            }
            w->w_Xacts = w->w_Xacts + 1UL;
        }

        (VOID)e_close(w->w_Base, s);

        w->w_Conns = w->w_Conns + 1UL;
        w->w_Beat  = end_ticks() | 1UL;
        round++;

        /* One lifecycle per tick: fast enough for a trend inside a few
           minutes, slow enough that the machine is not otherwise busy. */
        Delay(1);
    }
}

/* --------------------------------------------------------------- the filer -- */

/*
 * The Fitz workload: files of wildly varying size, written to a mounted share
 * and read back, in a loop, for hours.
 *
 * AmigaDOS calls on a mounted volume rather than sockets, deliberately.
 * Fitz's Amiga client is a DOS handler that turns each Read()/Write() into
 * blocking send()/recv() pairs on one TCP connection (src/amiga-client.c,
 * send_all()/recv_all()), so the traffic this generates is the reported
 * workload: many small requests, occasional large ones, both directions,
 * driven by a program that has no idea a network is involved.
 *
 * checkretry() in amiga-client.c treats EAGAIN on its blocking socket as
 * retryable, waits 20 ms and tries again, but only MAXRETRY = 10 times per
 * call, after which send_all()/recv_all() fail and the connection is
 * abandoned.  So a stack that answers EAGAIN on a blocking socket loses the
 * connection past ten in a row, which is the symptom the EAB report describes.
 * Every I/O error below is recorded with its DOS error code and the time.
 */

#define FILER_NAMES     16

static VOID end_filer_name(char *dst, const char *dir, ULONG idx, ULONG slot)
{
    ULONG i = 0UL;
    ULONG v;
    char  digits[12];
    LONG  d = 0;

    while (dir[i] != '\0')
    {
        dst[i] = dir[i];
        i++;
    }

    dst[i++] = 'e';
    dst[i++] = 'n';
    dst[i++] = 'd';

    v = idx * 100UL + slot;
    do
    {
        digits[d++] = (char)('0' + (v % 10UL));
        v /= 10UL;
    } while (v != 0UL);

    while (d > 0)
        dst[i++] = digits[--d];

    dst[i++] = '.';
    dst[i++] = 'd';
    dst[i++] = 'a';
    dst[i++] = 't';
    dst[i]   = '\0';
}

static VOID end_filer_fail(EndWorker *w, const char *what, ULONG detail)
{
    LONG args[6];

    w->w_Errors = w->w_Errors + 1UL;

    args[0] = (LONG)end_secs();
    args[1] = (LONG)w->w_Conn;
    args[2] = (LONG)what;
    args[3] = IoErr();
    args[4] = (LONG)detail;
    args[5] = (LONG)AvailMem(MEMF_PUBLIC);
    end_emit(F_EVENTS, "%lu filer=%lu %s ioerr=%ld detail=%lu avail=%lu\n",
             args);
}

/*
 * Write `len` pattern bytes to `name`, in chunks of a size redrawn per call.
 * Returns TRUE on success.
 */
static BOOL end_filer_write(EndWorker *w, const char *name, ULONG len,
                            ULONG off)
{
    BPTR  f = Open((CONST_STRPTR)name, MODE_NEWFILE);
    ULONG done = 0UL;

    if (f == (BPTR)0)
    {
        end_filer_fail(w, "open-write", len);
        return FALSE;
    }

    while (done < len)
    {
        ULONG chunk = end_pick(&w->w_Seed, ES->es_MaxIo);
        LONG  rc;

        if (chunk > len - done)
            chunk = len - done;

        end_pat_fill(w->w_Buf, off + done, chunk);

        w->w_CallStart    = end_ticks() | 1UL;
        w->w_CallKind     = 1;
        w->w_CallBlocking = 1;

        rc = Write(f, w->w_Buf, (LONG)chunk);

        w->w_CallStart = 0UL;

        if (rc != (LONG)chunk)
        {
            end_filer_fail(w, "write", done);
            Close(f);
            return FALSE;
        }

        done += chunk;
        end_bump_tx(w, chunk);
    }

    if (Close(f) == 0)
    {
        end_filer_fail(w, "close-write", len);
        return FALSE;
    }

    return TRUE;
}

/* Read `name` back and check every byte of it against the pattern. */
static BOOL end_filer_verify(EndWorker *w, const char *name, ULONG len,
                             ULONG off)
{
    BPTR  f = Open((CONST_STRPTR)name, MODE_OLDFILE);
    ULONG done = 0UL;

    if (f == (BPTR)0)
    {
        end_filer_fail(w, "open-read", len);
        return FALSE;
    }

    while (done < len)
    {
        ULONG chunk = end_pick(&w->w_Seed, ES->es_MaxIo);
        ULONG bad, first = 0UL;
        UBYTE got = 0, expect = 0;
        LONG  rc;

        if (chunk > len - done)
            chunk = len - done;

        w->w_CallStart    = end_ticks() | 1UL;
        w->w_CallKind     = 2;
        w->w_CallBlocking = 1;

        rc = Read(f, w->w_Buf, (LONG)chunk);

        w->w_CallStart = 0UL;

        if (rc <= 0)
        {
            end_filer_fail(w, (rc == 0) ? "read-short" : "read", done);
            Close(f);
            return FALSE;
        }

        bad = end_pat_check(w->w_Buf, off + done, (ULONG)rc, &first, &got,
                            &expect);
        if (bad != 0UL)
        {
            LONG args[8];

            w->w_Mismatch = w->w_Mismatch + bad;

            args[0] = (LONG)end_secs();
            args[1] = (LONG)w->w_Conn;
            args[2] = (LONG)bad;
            args[3] = (LONG)first;
            args[4] = (LONG)got;
            args[5] = (LONG)expect;
            args[6] = (LONG)rc;
            args[7] = (LONG)len;
            end_emit(F_EVENTS, "%lu filer=%lu FILE-CORRUPT bad=%lu "
                               "firstoff=%lu got=%02lx want=%02lx run=%lu "
                               "filelen=%lu\n", args);
        }

        done += (ULONG)rc;
        end_bump_rx(w, (ULONG)rc);
    }

    /* And the file must end where it was said to. */
    if (Read(f, w->w_Buf, 1) != 0)
    {
        end_filer_fail(w, "read-overlong", len);
        Close(f);
        return FALSE;
    }

    Close(f);

    return TRUE;
}

static VOID end_filer_body(EndWorker *w)
{
    char  remote[128];
    ULONG round = 0UL;
    ULONG off   = 0UL;

    Delay(50);

    while (!ES->es_Stop)
    {
        ULONG slot = end_xs(&w->w_Seed) % (ULONG)FILER_NAMES;
        ULONG len  = end_pick(&w->w_Seed, ES->es_MaxXfer);

        end_filer_name(remote, ES->es_Path, (ULONG)w->w_Conn, slot);

        if (!end_filer_write(w, remote, len, off))
        {
            Delay(50);
            round++;
            continue;
        }

        if (!end_filer_verify(w, remote, len, off))
        {
            Delay(50);
            round++;
            continue;
        }

        off += len;

        /*
         * Every so often, delete rather than overwrite.  A share that is only
         * ever rewritten in place never exercises create/delete, and the
         * reported workload -- copying directories back and forth -- is
         * mostly create and delete.
         */
        if ((round % 7UL) == 6UL)
        {
            if (DeleteFile((CONST_STRPTR)remote) == 0)
                end_filer_fail(w, "delete", slot);
        }

        w->w_Xacts = w->w_Xacts + 1UL;
        w->w_Beat  = end_ticks() | 1UL;
        round++;
    }
}

/* ----------------------------------------------------------- the P3 probe -- */

/*
 * A healthy connection, held open beside the hogs, on which one blocking
 * send() is attempted once the pool has been driven down.  The Process
 * publishes "in call" and "returned" so the supervisor can tell a wait from
 * an answer from a wedge; it does not time itself, because a wedged Process
 * cannot report that it is wedged.
 */
static VOID end_probe_body(EndWorker *w)
{
    EndAddr sa;
    LONG    ls, s, on = 1;
    LONG    rc;

    ls = end_new_socket(w);
    if (ls < 0)
        return;

    (VOID)e_setsockopt(w->w_Base, ls, E_SOL_SOCKET, E_SO_REUSEADDR,
                       &on, (LONG)sizeof(on));

    end_zero(&sa, sizeof(sa));
    sa.sin_len    = (UBYTE)sizeof(sa);
    sa.sin_family = E_AF_INET;
    sa.sin_port   = (UWORD)(ES->es_Port + 200U);
    sa.sin_addr   = 0x7F000001UL;

    if (e_bind(w->w_Base, ls, &sa) < 0 || e_listen(w->w_Base, ls, 2) < 0)
    {
        end_event(w, "probe-bind", -1, e_errno(w->w_Base), 1, 0UL);
        (VOID)e_close(w->w_Base, ls);
        return;
    }

    /* Dial ourselves: one Process, both ends, so nothing else has to be
       alive for the probe to have a peer. */
    s = end_new_socket(w);
    if (s < 0)
    {
        (VOID)e_close(w->w_Base, ls);
        return;
    }

    (VOID)end_set_nonblock(w, s, 1);
    (VOID)e_connect(w->w_Base, s, &sa);
    (VOID)end_set_nonblock(w, s, 0);

    {
        LONG peer;
        LONG tries = 0;

        (VOID)end_set_nonblock(w, ls, 1);
        for (;;)
        {
            peer = e_accept(w->w_Base, ls);
            if (peer >= 0)
                break;
            if (++tries > 200)
            {
                end_event(w, "probe-accept", -1, e_errno(w->w_Base), 0, 0UL);
                (VOID)e_close(w->w_Base, s);
                (VOID)e_close(w->w_Base, ls);
                return;
            }
            Delay(2);
        }

        while (!ES->es_ProbeGo && !ES->es_Stop)
            Delay(5);

        if (ES->es_Stop)
        {
            (VOID)e_close(w->w_Base, peer);
            (VOID)e_close(w->w_Base, s);
            (VOID)e_close(w->w_Base, ls);
            return;
        }

        end_pat_fill(w->w_Buf, 0UL, 8192UL);

        ES->es_ProbeBlocking = 1;
        ES->es_ProbeStart    = end_ticks() | 1UL;
        ES->es_ProbeState    = 1;

        rc = e_send(w->w_Base, s, w->w_Buf, 8192, 0);

        ES->es_ProbeEnd   = end_ticks() | 1UL;
        ES->es_ProbeRc    = rc;
        ES->es_ProbeErrno = (rc < 0) ? e_errno(w->w_Base) : 0;
        ES->es_ProbeState = 2;

        (VOID)e_close(w->w_Base, peer);
    }

    (VOID)e_close(w->w_Base, s);
    (VOID)e_close(w->w_Base, ls);
}

/* -------------------------------------------------------- Process fabric -- */

static VOID end_worker_entry(VOID)
{
    struct Process *me = (struct Process *)FindTask((STRPTR)0);
    EndWorker      *w;

    Wait(SIGF_SINGLE);

    w = (EndWorker *)me->pr_Task.tc_UserData;
    if (w == NULL)
        return;

    w->w_Base = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4UL);
    if (w->w_Base == NULL)
    {
        w->w_Done = 1;
        return;
    }

    w->w_Alive = 1;

    switch (w->w_Role)
    {
        case ROLE_DRIVER:    end_driver_body(w);    break;
        case ROLE_RESPONDER: end_responder_body(w); break;
        case ROLE_HOGTX:     end_hogtx_body(w);     break;
        case ROLE_HOGRX:     end_hogrx_body(w);     break;
        case ROLE_PROBE:     end_probe_body(w);     break;
        case ROLE_FILER:     end_filer_body(w);     break;
        case ROLE_LEAKER:    end_leaker_body(w);    break;
        default: break;
    }

    CloseLibrary(w->w_Base);
    w->w_Base  = NULL;
    w->w_Alive = 0;
    w->w_Done  = 1;
}

static struct Process *end_spawn(EndWorker *w, const char *name)
{
    struct Process *p;

    Forbid();

    p = CreateNewProcTags(NP_Entry,     (ULONG)end_worker_entry,
                          NP_Name,      (ULONG)name,
                          NP_Priority,  (ULONG)0,
                          NP_StackSize, (ULONG)16384,
                          NP_Cli,       (ULONG)FALSE,
                          TAG_DONE);

    if (p != NULL)
        p->pr_Task.tc_UserData = (APTR)w;

    Permit();

    if (p != NULL)
    {
        w->w_Proc = p;
        Signal(&p->pr_Task, SIGF_SINGLE);
    }

    return p;
}

/* ------------------------------------------------------------ the config -- */

static ULONG end_atoi(const char *s)
{
    ULONG v = 0UL;

    while (*s >= '0' && *s <= '9')
        v = v * 10UL + (ULONG)(*s++ - '0');

    return v;
}

static ULONG end_dotted(const char *s)
{
    ULONG v = 0UL;
    LONG  i;

    for (i = 0; i < 4; i++)
    {
        ULONG o = 0UL;

        while (*s >= '0' && *s <= '9')
            o = o * 10UL + (ULONG)(*s++ - '0');

        v = (v << 8) | (o & 255UL);

        if (*s == '.')
            s++;
    }

    return v;
}

static BOOL end_key(const char *line, const char *key, const char **val)
{
    const char *l = line;
    const char *k = key;

    while (*k != '\0' && *l == *k)
    {
        l++;
        k++;
    }

    if (*k != '\0' || (*l != ' ' && *l != '\t' && *l != '='))
        return FALSE;

    while (*l == ' ' || *l == '\t' || *l == '=')
        l++;

    *val = l;

    return TRUE;
}

static VOID end_copy(char *dst, const char *src, ULONG max)
{
    ULONG i = 0UL;

    while (src[i] != '\0' && i + 1UL < max)
    {
        dst[i] = src[i];
        i++;
    }

    dst[i] = '\0';
}

static VOID end_defaults(VOID)
{
    ES->es_Mode     = MODE_LOOP;
    ES->es_Wire     = 0;
    ES->es_Filers   = 2;
    end_copy(ES->es_Path,    "FITZ:",    sizeof(ES->es_Path));
    end_copy(ES->es_Scratch, "RAM:endz", sizeof(ES->es_Scratch));
    ES->es_Conns    = 2;
    ES->es_Hogs     = 4;
    ES->es_Port     = 7801;
    ES->es_Peer     = 0x0A000202UL;      /* 10.0.2.2 */
    ES->es_Seconds  = 900UL;
    ES->es_Sample   = 15UL;
    ES->es_MaxIo    = 32768UL;
    ES->es_MaxXfer  = 262144UL;
    ES->es_MaxEcho  = 16384UL;
    ES->es_Churn    = 20UL;
    ES->es_NbEvery  = 50UL;
    ES->es_Seed     = 0x1234567UL;
    ES->es_Probes   = 1;
}

static VOID end_read_config(VOID)
{
    BPTR  f = Open((CONST_STRPTR)"DH0:endurance.cfg", MODE_OLDFILE);
    char  line[128];

    if (f == (BPTR)0)
        return;

    while (FGets(f, (STRPTR)line, (LONG)sizeof(line)) != NULL)
    {
        const char *v;
        LONG        i;

        for (i = 0; line[i] != '\0'; i++)
        {
            if (line[i] == '\n' || line[i] == '\r')
            {
                line[i] = '\0';
                break;
            }
        }

        if (line[0] == '#' || line[0] == '\0')
            continue;

        if (end_key(line, "mode", &v))
        {
            /*
             * loop | wire | fitz | watch | leak.  Matched on two letters:
             * "loop" and "leak" share their first, and matching on one ran
             * the wrong workload for ten minutes and produced a timeline that
             * could not be read correctly.
             */
            if      (v[0] == 'w' && v[1] == 'i') ES->es_Mode = MODE_WIRE;
            else if (v[0] == 'w' && v[1] == 'a') ES->es_Mode = MODE_WATCH;
            else if (v[0] == 'f')                ES->es_Mode = MODE_FITZ;
            else if (v[0] == 'l' && v[1] == 'e') ES->es_Mode = MODE_LEAK;
            else                                 ES->es_Mode = MODE_LOOP;
        }
        else if (end_key(line, "path", &v))     end_copy(ES->es_Path, v,
                                                         sizeof(ES->es_Path));
        else if (end_key(line, "scratch", &v))  end_copy(ES->es_Scratch, v,
                                                         sizeof(ES->es_Scratch));
        else if (end_key(line, "filers", &v))   ES->es_Filers  = (UWORD)end_atoi(v);
        else if (end_key(line, "conns", &v))    ES->es_Conns   = (UWORD)end_atoi(v);
        else if (end_key(line, "hogs", &v))     ES->es_Hogs    = (UWORD)end_atoi(v);
        else if (end_key(line, "port", &v))     ES->es_Port    = (UWORD)end_atoi(v);
        else if (end_key(line, "peer", &v))     ES->es_Peer    = end_dotted(v);
        else if (end_key(line, "seconds", &v))  ES->es_Seconds = end_atoi(v);
        else if (end_key(line, "sample", &v))   ES->es_Sample  = end_atoi(v);
        else if (end_key(line, "maxio", &v))    ES->es_MaxIo   = end_atoi(v);
        else if (end_key(line, "maxxfer", &v))  ES->es_MaxXfer = end_atoi(v);
        else if (end_key(line, "maxecho", &v))  ES->es_MaxEcho = end_atoi(v);
        else if (end_key(line, "churn", &v))    ES->es_Churn   = end_atoi(v);
        else if (end_key(line, "nbevery", &v))  ES->es_NbEvery = end_atoi(v);
        else if (end_key(line, "seed", &v))     ES->es_Seed    = end_atoi(v);
        else if (end_key(line, "probes", &v))   ES->es_Probes  = (UWORD)end_atoi(v);
    }

    Close(f);

    ES->es_Wire = (ES->es_Mode == MODE_WIRE);

    if (ES->es_Filers > END_MAX_CONNS) ES->es_Filers = END_MAX_CONNS;
    if (ES->es_Conns  > END_MAX_CONNS) ES->es_Conns = END_MAX_CONNS;
    if (ES->es_Conns  < 1)             ES->es_Conns = 1;
    if (ES->es_Hogs   > END_MAX_HOGS)  ES->es_Hogs  = END_MAX_HOGS;
    if (ES->es_Sample < 5UL)           ES->es_Sample = 5UL;
    if (ES->es_MaxIo  < 64UL)          ES->es_MaxIo = 64UL;
    if (ES->es_MaxEcho > ES->es_MaxIo) ES->es_MaxEcho = ES->es_MaxIo;
}

/* -------------------------------------------------------- the supervisor -- */

static ULONG end_total(const volatile ULONG *lo, const volatile ULONG *hi)
{
    /* Bytes in units of 1 MB, so the CSV column does not wrap in six hours. */
    return (*hi) * 4096UL + (*lo) / 1048576UL;
}

static VOID end_sample(struct Library *base, ULONG t)
{
    EndSysBuf   sys;
    EndStatBuf  st;
    EndIfBuf    ifb;
    EndSockBuf  sk;
    LONG        args[26];
    ULONG       i;
    ULONG       tx_mb = 0UL, rx_mb = 0UL, xacts = 0UL, errs = 0UL;
    ULONG       bad = 0UL, desync = 0UL, conns = 0UL;
    ULONG       socks = 0UL;
    ULONG       alloc_fail = 0UL, overrun = 0UL, rxerr = 0UL, txerr = 0UL;

    for (i = 0; i < ES->es_Workers; i++)
    {
        EndWorker *w = &ES->es_W[i];

        tx_mb  += end_total(&w->w_BytesTx, &w->w_MegaTx);
        rx_mb  += end_total(&w->w_BytesRx, &w->w_MegaRx);
        xacts  += w->w_Xacts;
        errs   += w->w_Errors;
        bad    += w->w_Mismatch;
        desync += w->w_Desync;
        conns  += w->w_Conns;
    }

    end_zero(&sys, sizeof(sys));
    end_zero(&st,  sizeof(st));

    (VOID)end_query(base, NETSTATUS_SYSTEM, &sys, sizeof(sys));
    (VOID)end_query(base, NETSTATUS_STATS,  &st,  sizeof(st));

    if (end_query(base, NETSTATUS_INTERFACES, &ifb, sizeof(ifb)) > 0)
    {
        UWORD n = ifb.hdr.nsh_Count;
        UWORD j;

        for (j = 0; j < n && j < 4; j++)
        {
            alloc_fail += ifb.e[j].nsi_AllocFailures;
            overrun    += ifb.e[j].nsi_Overruns;
            rxerr      += ifb.e[j].nsi_RxErrors;
            txerr      += ifb.e[j].nsi_TxErrors;
        }
    }

    if (end_query(base, NETSTATUS_SOCKETS, &sk, sizeof(sk)) >= 0)
        socks = (ULONG)sk.hdr.nsh_Available;

    args[0]  = (LONG)t;
    args[1]  = (LONG)AvailMem(MEMF_PUBLIC);
    args[2]  = (LONG)AvailMem(MEMF_PUBLIC | MEMF_LARGEST);
    args[3]  = (LONG)sys.e.nss_PoolTotal;
    args[4]  = (LONG)sys.e.nss_PoolFree;
    args[5]  = (LONG)sys.e.nss_PoolEmptyRequests;
    args[6]  = (LONG)sys.e.nss_PoolEmptySuspensions;
    args[7]  = (LONG)sys.e.nss_PoolInvalidReleases;
    args[8]  = (LONG)socks;
    args[9]  = (LONG)st.e.nsx_TcpConnections;
    args[10] = (LONG)st.e.nsx_TcpDisconnections;
    args[11] = (LONG)st.e.nsx_TcpConnectionsDropped;
    args[12] = (LONG)st.e.nsx_TcpRetransmits;
    args[13] = (LONG)st.e.nsx_TcpReceiveDropped;
    args[14] = (LONG)st.e.nsx_TcpChecksumErrors;
    args[15] = (LONG)st.e.nsx_IpReceiveDropped;
    args[16] = (LONG)st.e.nsx_IpSendDropped;
    args[17] = (LONG)alloc_fail;
    args[18] = (LONG)overrun;
    args[19] = (LONG)rxerr;
    args[20] = (LONG)txerr;
    args[21] = (LONG)tx_mb;
    args[22] = (LONG)rx_mb;
    args[23] = (LONG)xacts;
    args[24] = (LONG)conns;
    args[25] = (LONG)(errs * 1000000UL + bad * 1000UL + desync);

    end_emit(F_TIMELINE,
             "%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,"
             "%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu\n",
             args);
}

/*
 * A worker inside one socket call for longer than this has not merely slowed
 * down: every call in this harness has a peer trying to satisfy it.  Reported
 * once per worker, with the call and the blocking flag, because a blocking
 * call that never returns on an exhausted pool is a distinct failure from one
 * that returns EAGAIN.
 */
#define END_STALL_TICKS     (120UL * 50UL)

/*
 * The elapsed time inside a call, in ticks, or 0 when it has not started yet.
 *
 * w_CallStart is stamped `end_ticks() | 1` so that 0 can mean "not in a call",
 * which puts it up to one tick in the future; without the guard below,
 * `now - start` underflows to 0xFFFFFFFF and the stall detector reports every
 * healthy worker as stalled for 85899345 seconds.  Observed on the first run
 * that used it.
 */
static ULONG end_in_call(ULONG start)
{
    ULONG now = end_ticks();

    if (start == 0UL || now < start)
        return 0UL;

    return now - start;
}

static VOID end_check_stalls(ULONG t, UWORD *reported)
{
    ULONG i;

    for (i = 0; i < ES->es_Workers; i++)
    {
        EndWorker *w       = &ES->es_W[i];
        ULONG      elapsed = end_in_call(w->w_CallStart);

        if (elapsed == 0UL || (reported[i] & 1U) != 0U)
            continue;

        if (elapsed > END_STALL_TICKS)
        {
            LONG args[6];

            reported[i] |= 1U;

            args[0] = (LONG)t;
            args[1] = (LONG)w->w_Role;
            args[2] = (LONG)w->w_Conn;
            args[3] = (LONG)w->w_CallKind;
            args[4] = (LONG)w->w_CallBlocking;
            args[5] = (LONG)(elapsed / 50UL);
            end_emit(F_EVENTS, "%lu role=%lu conn=%lu STALLED kind=%lu "
                               "blocking=%lu seconds=%lu\n", args);
        }
    }
}

/* ------------------------------------------------------------- the probes -- */

static ULONG end_pool_free(struct Library *base)
{
    EndSysBuf sys;

    if (end_query(base, NETSTATUS_SYSTEM, &sys, sizeof(sys)) <= 0)
        return 0UL;

    return sys.e.nss_PoolFree;
}

static VOID end_p3(struct Library *base)
{
    ULONG floor_free = 0xFFFFFFFFUL;
    ULONG waited     = 0UL;
    LONG  args[6];

    ES->es_HogGo = 1;

    /* Let the hogs run until the pool stops falling, or 90 s, whichever
       first.  The floor reached is itself a result. */
    while (waited < 900UL)
    {
        ULONG f = end_pool_free(base);

        if (f < floor_free)
            floor_free = f;

        if (f <= 2UL)
            break;

        Delay(10);
        waited += 10UL;
    }

    args[0] = (LONG)end_secs();
    args[1] = (LONG)floor_free;
    args[2] = (LONG)end_pool_free(base);
    end_emit(F_EVENTS, "%lu P3 pool driven down: floor=%lu now=%lu\n", args);

    ES->es_ProbeGo = 1;

    /* Watch the probe from outside it. */
    waited = 0UL;
    while (waited < 3000UL && ES->es_ProbeState != 2)
    {
        Delay(10);
        waited += 10UL;
    }

    args[0] = (LONG)end_secs();
    args[1] = (LONG)ES->es_ProbeState;
    args[2] = ES->es_ProbeRc;
    args[3] = ES->es_ProbeErrno;
    args[4] = (LONG)((ES->es_ProbeState == 2)
                     ? ((ES->es_ProbeEnd >= ES->es_ProbeStart)
                        ? (ES->es_ProbeEnd - ES->es_ProbeStart) / 50UL : 0UL)
                     : (waited / 50UL));
    args[5] = (LONG)floor_free;
    end_emit(F_EVENTS, "%lu P3 blocking send on an exhausted pool: state=%lu "
                       "rc=%ld errno=%ld seconds=%lu poolfloor=%lu\n", args);

    Printf((CONST_STRPTR)"P3: pool floor %lu, blocking send state=%lu rc=%ld "
                         "errno=%ld after %lus\n",
           (LONG)floor_free, (LONG)ES->es_ProbeState, ES->es_ProbeRc,
           ES->es_ProbeErrno, args[4]);

    ES->es_HogStop = 1;
}

/* ----------------------------------------------------------------- main --- */

static VOID end_summary(struct Library *base, ULONG ran, ULONG avail_end)
{
    EndSysBuf sys;
    LONG      args[12];
    ULONG     i;
    ULONG     tx_mb = 0UL, rx_mb = 0UL, xacts = 0UL, errs = 0UL;
    ULONG     bad = 0UL, desync = 0UL, conns = 0UL;
    ULONG     max_conn_mb = 0UL;

    for (i = 0; i < ES->es_Workers; i++)
    {
        EndWorker *w = &ES->es_W[i];
        ULONG      mb;

        tx_mb  += end_total(&w->w_BytesTx, &w->w_MegaTx);
        rx_mb  += end_total(&w->w_BytesRx, &w->w_MegaRx);
        xacts  += w->w_Xacts;
        errs   += w->w_Errors;
        bad    += w->w_Mismatch;
        desync += w->w_Desync;
        conns  += w->w_Conns;

        if (w->w_Role == ROLE_DRIVER || w->w_Role == ROLE_FILER)
        {
            mb = end_total(&w->w_BytesTx, &w->w_MegaTx)
                 + end_total(&w->w_BytesRx, &w->w_MegaRx);
            if (mb > max_conn_mb)
                max_conn_mb = mb;
        }
    }

    end_zero(&sys, sizeof(sys));
    (VOID)end_query(base, NETSTATUS_SYSTEM, &sys, sizeof(sys));

    args[0]  = (LONG)ran;
    args[1]  = (LONG)tx_mb;
    args[2]  = (LONG)rx_mb;
    args[3]  = (LONG)xacts;
    args[4]  = (LONG)conns;
    args[5]  = (LONG)errs;
    args[6]  = (LONG)bad;
    args[7]  = (LONG)desync;
    args[8]  = (LONG)sys.e.nss_PoolFree;
    args[9]  = (LONG)sys.e.nss_PoolTotal;
    args[10] = (LONG)avail_end;
    args[11] = (LONG)max_conn_mb;

    end_emit(F_SUMMARY,
             "seconds %lu\ntx_mb %lu\nrx_mb %lu\nxacts %lu\nconnections %lu\n"
             "errno_events %lu\ncorrupt_bytes %lu\nframing_desyncs %lu\n"
             "pool_free_end %lu\npool_total %lu\navail_end %lu\n"
             "busiest_driver_mb %lu\n", args);

    Printf((CONST_STRPTR)"ran %lus  tx %luMB  rx %luMB  xacts %lu  conns %lu  "
                         "errno %lu  corrupt %lu  desync %lu  pool %lu/%lu  "
                         "avail %lu\n",
           (LONG)ran, (LONG)tx_mb, (LONG)rx_mb, (LONG)xacts, (LONG)conns,
           (LONG)errs, (LONG)bad, (LONG)desync, (LONG)sys.e.nss_PoolFree,
           (LONG)sys.e.nss_PoolTotal, (LONG)avail_end);
}

int main(void)
{
    struct Library *base;
    ULONG           i, n = 0UL;
    ULONG           deadline, next_sample, t;
    UWORD           stall_reported[END_MAX_WORKERS];
    LONG            args[4];
    int             rc = RETURN_OK;

    {
        struct DateStamp ds;

        DateStamp(&ds);
        end_t0_ticks = ((ULONG)ds.ds_Days % 86400UL) * 4320000UL
                       + (ULONG)ds.ds_Minute * 3000UL
                       + (ULONG)ds.ds_Tick;
    }

    InitSemaphore(&end_out_sem);

    ES = (EndState *)AllocMem((ULONG)sizeof(EndState), MEMF_PUBLIC | MEMF_CLEAR);
    if (ES == NULL)
    {
        Printf((CONST_STRPTR)"endurance: out of memory\n");
        return RETURN_FAIL;
    }

    end_defaults();
    end_read_config();
    end_pat_init(ES->es_Seed);

    base = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4UL);
    if (base == NULL)
    {
        Printf((CONST_STRPTR)"endurance: no bsdsocket.library\n");
        FreeMem(ES, (ULONG)sizeof(EndState));
        return RETURN_FAIL;
    }

    if (base->lib_Revision < (UWORD)AMI_NETSTATUS_MIN_REVISION)
    {
        Printf((CONST_STRPTR)"endurance: bsdsocket.library revision %ld is "
                             "older than %ld; no pool timeline is possible\n",
               (LONG)base->lib_Revision, (LONG)AMI_NETSTATUS_MIN_REVISION);
        CloseLibrary(base);
        FreeMem(ES, (ULONG)sizeof(EndState));
        return RETURN_FAIL;
    }

    end_truncate(F_TIMELINE);
    end_truncate(F_EVENTS);
    end_truncate(F_SUMMARY);

    end_emit(F_TIMELINE,
             "t_s,avail_pub,avail_largest,pool_total,pool_free,pool_empty_req,"
             "pool_empty_susp,pool_bad_release,sockets,tcp_conn,tcp_disc,"
             "tcp_drop,tcp_retx,tcp_rxdrop,tcp_cksum,ip_rxdrop,ip_txdrop,"
             "if_allocfail,if_overrun,if_rxerr,if_txerr,tx_mb,rx_mb,xacts,"
             "conns,errcode\n", NULL);

    {
        static const char *const modename[] =
            { "loopback", "wire", "fitz", "watch", "leak" };
        const char *mn = modename[(ES->es_Mode <= MODE_LEAK) ? ES->es_Mode : 0U];

        args[0] = (LONG)ES->es_Seconds;
        args[1] = (LONG)ES->es_Conns;
        args[2] = (LONG)mn;
        args[3] = (LONG)ES->es_Port;
        end_emit(F_SUMMARY, "# seconds=%lu conns=%lu mode=%s port=%lu\n", args);

        Printf((CONST_STRPTR)"Endurance: %lus, mode %s, %ld connection(s)/"
                             "%ld filer(s), port %ld, path %s\n",
               (LONG)ES->es_Seconds, (LONG)mn, (LONG)ES->es_Conns,
               (LONG)ES->es_Filers, (LONG)ES->es_Port, (LONG)ES->es_Path);
    }

    /* ---- workers ---------------------------------------------------- */

    if (ES->es_Mode == MODE_FITZ)
    {
        for (i = 0; i < (ULONG)ES->es_Filers; i++)
        {
            EndWorker *f = &ES->es_W[n++];

            f->w_Role = ROLE_FILER;
            f->w_Conn = (UWORD)i;
            f->w_Seed = ES->es_Seed + 0x7A5B1CD3UL + i * 15485863UL;
            f->w_Buf  = (UBYTE *)AllocMem(ES->es_MaxIo, MEMF_PUBLIC);
        }
    }
    else if (ES->es_Mode == MODE_LEAK)
    {
        /* One responder, on conn 1's port, for the accepted-connection arm.
           conn 0 dials a port nothing is on. */
        EndWorker *r  = &ES->es_W[n++];
        EndWorker *l0 = &ES->es_W[n++];
        EndWorker *l1 = &ES->es_W[n++];

        r->w_Role  = ROLE_RESPONDER;
        r->w_Conn  = 1;
        r->w_Seed  = ES->es_Seed + 11UL;
        r->w_Buf   = (UBYTE *)AllocMem(ES->es_MaxIo, MEMF_PUBLIC);

        l0->w_Role = ROLE_LEAKER;
        l0->w_Conn = 0;
        l0->w_Seed = ES->es_Seed + 22UL;
        l0->w_Buf  = (UBYTE *)AllocMem(4096UL, MEMF_PUBLIC);

        l1->w_Role = ROLE_LEAKER;
        l1->w_Conn = 1;
        l1->w_Seed = ES->es_Seed + 33UL;
        l1->w_Buf  = (UBYTE *)AllocMem(4096UL, MEMF_PUBLIC);
    }
    else if (ES->es_Mode == MODE_LOOP || ES->es_Mode == MODE_WIRE)
    {
        for (i = 0; i < (ULONG)ES->es_Conns; i++)
        {
            if (!ES->es_Wire)
            {
                EndWorker *r = &ES->es_W[n++];

                r->w_Role = ROLE_RESPONDER;
                r->w_Conn = (UWORD)i;
                r->w_Seed = ES->es_Seed + 0x51ED2701UL + i * 7919UL;
                r->w_Buf  = (UBYTE *)AllocMem(ES->es_MaxIo, MEMF_PUBLIC);
            }

            {
                EndWorker *d = &ES->es_W[n++];

                d->w_Role = ROLE_DRIVER;
                d->w_Conn = (UWORD)i;
                d->w_Seed = ES->es_Seed + 0x2545F491UL + i * 104729UL;
                d->w_Buf  = (UBYTE *)AllocMem(ES->es_MaxIo, MEMF_PUBLIC);
            }
        }
    }

    if (ES->es_Probes && ES->es_Mode == MODE_LOOP)
    {
        for (i = 0; i < (ULONG)ES->es_Hogs; i++)
        {
            EndWorker *hr = &ES->es_W[n++];
            EndWorker *ht = &ES->es_W[n++];

            hr->w_Role = ROLE_HOGRX;
            hr->w_Conn = (UWORD)i;
            hr->w_Seed = 0x1000UL + i;
            hr->w_Buf  = (UBYTE *)AllocMem(8192UL, MEMF_PUBLIC);

            ht->w_Role = ROLE_HOGTX;
            ht->w_Conn = (UWORD)i;
            ht->w_Seed = 0x2000UL + i;
            ht->w_Buf  = (UBYTE *)AllocMem(8192UL, MEMF_PUBLIC);
        }

        {
            EndWorker *p = &ES->es_W[n++];

            p->w_Role = ROLE_PROBE;
            p->w_Conn = 0;
            p->w_Seed = 0x3000UL;
            p->w_Buf  = (UBYTE *)AllocMem(16384UL, MEMF_PUBLIC);
        }
    }

    ES->es_Workers = (UWORD)n;

    for (i = 0; i < n; i++)
    {
        if (ES->es_W[i].w_Buf == NULL)
        {
            Printf((CONST_STRPTR)"endurance: out of buffer memory\n");
            CloseLibrary(base);
            return RETURN_FAIL;
        }

        stall_reported[i] = 0;

        if (end_spawn(&ES->es_W[i], "endurance worker") == NULL)
        {
            Printf((CONST_STRPTR)"endurance: cannot start worker %ld\n",
                   (LONG)i);
            CloseLibrary(base);
            return RETURN_FAIL;
        }
    }

    /* ---- the probes, then the soak ---------------------------------- */

    if (ES->es_Probes && ES->es_Mode == MODE_LOOP)
    {
        Delay(150);                 /* let the ordinary traffic get going */
        end_p3(base);
    }

    deadline    = ES->es_Seconds;
    next_sample = 0UL;

    for (;;)
    {
        t = end_secs();

        if (t >= next_sample)
        {
            end_sample(base, t);
            next_sample = t + ES->es_Sample;
        }

        end_check_stalls(t, stall_reported);

        if (t >= deadline)
            break;

        Delay(50);
    }

    /* ---- stop ------------------------------------------------------- */

    ES->es_Stop    = 1;
    ES->es_HogStop = 1;

    for (i = 0; i < 600UL; i++)
    {
        ULONG live = 0UL;
        ULONG j;

        for (j = 0; j < n; j++)
            if (!ES->es_W[j].w_Done)
                live++;

        if (live == 0UL)
            break;

        Delay(10);
    }

    {
        ULONG live = 0UL;
        ULONG j;

        for (j = 0; j < n; j++)
            if (!ES->es_W[j].w_Done)
                live++;

        if (live != 0UL)
        {
            args[0] = (LONG)end_secs();
            args[1] = (LONG)live;
            end_emit(F_EVENTS, "%lu %lu worker(s) did not finish -- see "
                               "STALLED above\n", args);
            Printf((CONST_STRPTR)"!! %ld worker(s) never returned\n",
                   (LONG)live);
            rc = RETURN_WARN;
        }
    }

    end_sample(base, end_secs());
    end_summary(base, end_secs(), AvailMem(MEMF_PUBLIC));

    {
        ULONG bad = 0UL, desync = 0UL, j;

        for (j = 0; j < n; j++)
        {
            bad    += ES->es_W[j].w_Mismatch;
            desync += ES->es_W[j].w_Desync;
        }

        if (bad != 0UL || desync != 0UL)
            rc = RETURN_FAIL;
    }

    CloseLibrary(base);

    return rc;
}
