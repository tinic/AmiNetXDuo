/*
 * AmiNetXDuo, Fitz connection-drop soak.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <stdlib.h>   /* atexit */

#include "aminetxduo/netstatus.h"

static const char version_tag[] __attribute__((used)) =
    "$VER: FitzSoak 1.0 (29.7.2026)";

_Static_assert(AMI_NETSTATUS_QUERY_LVO == -870, "NetStackQuery LVO moved");


/* ------------------------------------------------------------------ LVOs -- */

static LONG s_query(struct Library *base, ULONG what, APTR buf, ULONG size)
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


/* ------------------------------------------------------------- the clock -- */

static ULONG s_t0;

/* Ticks (1/50 s) since the program started; DateStamp() needs no device. */
static ULONG s_ticks(VOID)
{
    struct DateStamp ds;
    ULONG            t;

    DateStamp(&ds);

    t = ((ULONG)ds.ds_Days % 86400UL) * 4320000UL
        + (ULONG)ds.ds_Minute * 3000UL
        + (ULONG)ds.ds_Tick;

    return t - s_t0;
}

static ULONG s_secs(VOID)
{
    return s_ticks() / 50UL;
}


/* -------------------------------------------------------------- the state -- */

#define S_MAX_FILERS    8
#define S_PATH          64
#define S_NAME          96

#define ARM_WIRE        0   /* host fitz-serve, over the A2065        */
#define ARM_LOCAL       1   /* the guest's own fitz serve, loopback   */
#define ARM_COUNT       2

#define PH_WIRE         0
#define PH_IDLE1        1
#define PH_LOCAL        2
#define PH_IDLE2        3
#define PH_BOTH         4
#define PH_CHURN        5
#define PH_COUNT        6

static const char *const s_phase_name[PH_COUNT] =
{
    "WIRE", "IDLE", "LOCAL", "IDLE", "BOTH", "CHURN"
};

typedef struct SoakFiler
{
    UWORD           f_Index;
    UWORD           f_Arm;
    volatile UWORD  f_Alive;
    volatile UWORD  f_Done;
    volatile UWORD  f_Stop;
    volatile UWORD  f_Run;          /* the supervisor's go/no-go per phase   */

    /* Stamped `s_ticks() | 1` around every DOS call, so 0 means "not in
       one", the watchdog needs to tell a slow call from no call. */
    volatile ULONG  f_CallStart;
    volatile ULONG  f_CallKind;

    volatile ULONG  f_Files;
    volatile ULONG  f_Bytes;        /* low part, wraps into f_Mega           */
    volatile ULONG  f_Mega;
    volatile ULONG  f_Errors;
    volatile ULONG  f_Corrupt;

    ULONG           f_Seed;
    struct Process *f_Proc;
    char            f_Dir[S_PATH];
    UBYTE          *f_Buf;

    /* Its own library base.  bsdsocket.library hands each opener a private
       base and the tree's other harnesses open one per Process; a snapshot
       taken through somebody else's is not worth the risk. */
    struct Library *f_Base;
} SoakFiler;

typedef struct SoakState
{
    struct Library *ss_Base;            /* bsdsocket.library, supervisor's   */
    struct Library *ss_ChurnBase;       /* and the churner's own             */

    ULONG           ss_Seconds;
    ULONG           ss_Sample;
    ULONG           ss_Phase;           /* seconds per phase                 */
    ULONG           ss_MaxIo;
    ULONG           ss_MaxXfer;
    ULONG           ss_Seed;
    ULONG           ss_ChurnEvery;      /* seconds between queries off-phase */
    UWORD           ss_Filers;          /* per arm                           */
    UWORD           ss_ServePort;
    UWORD           ss_WirePort;
    char            ss_WireHost[32];
    char            ss_Vol[ARM_COUNT][S_PATH];

    SoakFiler       ss_F[ARM_COUNT * S_MAX_FILERS];
    UWORD           ss_FilerCount;

    /* the churner */
    struct Process *ss_ChurnProc;       /* NULL until it is spawned          */
    volatile UWORD  ss_ChurnRun;
    volatile UWORD  ss_ChurnAlive;
    volatile UWORD  ss_ChurnDone;
    volatile UWORD  ss_ChurnStop;
    volatile ULONG  ss_Queries[ARM_COUNT];
    volatile ULONG  ss_QueryFail[ARM_COUNT];
    volatile ULONG  ss_ChurnCallStart;

    volatile ULONG  ss_CurPhase;
    volatile ULONG  ss_Drops;           /* anything that looks like a drop   */
} SoakState;

static SoakState *SS;


/* --------------------------------------------------------------- output --- */

#define F_TIMELINE  "DH0:soak-timeline.csv"
#define F_EVENTS    "DH0:soak-events.txt"
#define F_SUMMARY   "DH0:soak-summary.txt"

static struct SignalSemaphore s_out_sem;

static VOID s_emit(const char *file, const char *fmt, LONG *args)
{
    BPTR out;

    ObtainSemaphore(&s_out_sem);

    out = Open((CONST_STRPTR)file, MODE_READWRITE);
    if (out != (BPTR)0)
    {
        Seek(out, 0, OFFSET_END);
        VFPrintf(out, (CONST_STRPTR)fmt, args);
        Close(out);
    }

    ReleaseSemaphore(&s_out_sem);
}

static VOID s_truncate(const char *file)
{
    BPTR out = Open((CONST_STRPTR)file, MODE_NEWFILE);

    if (out != (BPTR)0)
        Close(out);
}

static VOID s_zero(APTR p, ULONG n)
{
    UBYTE *b = (UBYTE *)p;

    while (n-- > 0UL)
        *b++ = 0;
}


/* ------------------------------------------------------------ statistics -- */

typedef struct { NetStatusHeader hdr; NetStatusSystem    e;      } SoakSysBuf;
typedef struct { NetStatusHeader hdr; NetStatusStats     e;      } SoakStatBuf;
typedef struct { NetStatusHeader hdr; NetStatusInterface e[4];   } SoakIfBuf;
typedef struct { NetStatusHeader hdr; NetStatusSocket    e[64];  } SoakSockBuf;
typedef struct { NetStatusHeader hdr; NetStatusHealth    e;      } SoakHealthBuf;

static LONG s_ask(struct Library *base, ULONG what, APTR buf, ULONG size)
{
    NetStatusHeader *h = (NetStatusHeader *)buf;

    s_zero(buf, size);
    h->nsh_Magic   = AMI_NETSTATUS_MAGIC;
    h->nsh_Version = (UWORD)AMI_NETSTATUS_VERSION;

    return s_query(base, what, buf, size);
}

static VOID s_dump_sockets(struct Library *base, const char *why)
{
    SoakSockBuf sk;
    LONG        args[7];
    UWORD       n, i;

    if (s_ask(base, NETSTATUS_SOCKETS, &sk, sizeof(sk)) < 0)
        return;

    n = sk.hdr.nsh_Count;

    args[0] = (LONG)s_secs();
    args[1] = (LONG)why;
    args[2] = (LONG)n;
    args[3] = (LONG)sk.hdr.nsh_Available;
    s_emit(F_EVENTS, "%lu SNAP %s sockets shown=%lu held=%lu\n", args);

    for (i = 0; i < n; i++)
    {
        args[0] = (LONG)s_secs();
        args[1] = (LONG)i;
        args[2] = (LONG)((sk.e[i].nso_Flags & NETSTATUS_SOCK_TCP)
                         ? "tcp" : "udp");
        args[3] = (LONG)sk.e[i].nso_LocalPort;
        args[4] = (LONG)sk.e[i].nso_PeerPort;
        args[5] = (LONG)sk.e[i].nso_State;
        args[6] = (LONG)sk.e[i].nso_PeerAddress;
        s_emit(F_EVENTS, "%lu SNAP   sock[%lu] %s local=%lu peer=%lu "
                         "state=%lu peeraddr=0x%08lx\n", args);
    }
}

static VOID s_note(const char *what, LONG detail);

static VOID s_snapshot(struct Library *base, const char *why)
{
    /* Heap, not stack: 2.4 KB of buffers on the 4 KB stack an AmigaDOS CLI
       gives a program.  Not `static`: s_fail() and s_churn_entry() can both
       be in here at once. */
    struct { SoakSysBuf sys; SoakStatBuf st; SoakIfBuf ifb; SoakSockBuf sk; } *b;
    LONG        args[16];
    ULONG       socks = 0UL;
    ULONG       alloc_fail = 0UL, overrun = 0UL, rxerr = 0UL, txerr = 0UL;
    UWORD       j;

    b = (APTR)AllocVec((ULONG)sizeof(*b), MEMF_ANY);
    if (b == NULL)
    {
        s_note("snapshot-nomem", (LONG)sizeof(*b));
        return;
    }

    (VOID)s_ask(base, NETSTATUS_SYSTEM, &b->sys, sizeof(b->sys));
    (VOID)s_ask(base, NETSTATUS_STATS,  &b->st, sizeof(b->st));

    if (s_ask(base, NETSTATUS_INTERFACES, &b->ifb, sizeof(b->ifb)) > 0)
    {
        UWORD n = b->ifb.hdr.nsh_Count;

        for (j = 0; j < n && j < 4; j++)
        {
            alloc_fail += b->ifb.e[j].nsi_AllocFailures;
            overrun    += b->ifb.e[j].nsi_Overruns;
            rxerr      += b->ifb.e[j].nsi_RxErrors;
            txerr      += b->ifb.e[j].nsi_TxErrors;
        }
    }

    if (s_ask(base, NETSTATUS_SOCKETS, &b->sk, sizeof(b->sk)) >= 0)
        socks = (ULONG)b->sk.hdr.nsh_Available;

    args[0]  = (LONG)s_secs();
    args[1]  = (LONG)why;
    args[2]  = (LONG)AvailMem(MEMF_PUBLIC);
    args[3]  = (LONG)AvailMem(MEMF_PUBLIC | MEMF_LARGEST);
    args[4]  = (LONG)b->sys.e.nss_PoolFree;
    args[5]  = (LONG)b->sys.e.nss_PoolTotal;
    args[6]  = (LONG)b->sys.e.nss_PoolEmptyRequests;
    args[7]  = (LONG)socks;
    args[8]  = (LONG)b->st.e.nsx_TcpRetransmits;
    args[9]  = (LONG)b->st.e.nsx_TcpConnections;
    args[10] = (LONG)b->st.e.nsx_TcpDisconnections;
    args[11] = (LONG)b->st.e.nsx_TcpConnectionsDropped;
    args[12] = (LONG)b->st.e.nsx_IpSendDropped;
    args[13] = (LONG)b->st.e.nsx_IpReceiveDropped;
    args[14] = (LONG)(alloc_fail + overrun);
    args[15] = (LONG)(rxerr + txerr);

    s_emit(F_EVENTS,
           "%lu SNAP %s avail=%lu largest=%lu poolfree=%lu pooltotal=%lu "
           "poolempty=%lu sockets=%lu retrans=%lu conn=%lu disc=%lu "
           "connDropped=%lu ipSendDrop=%lu ipRecvDrop=%lu sanaAlloc=%lu "
           "sanaErr=%lu\n", args);

    {
        SoakHealthBuf *h = (APTR)AllocVec((ULONG)sizeof(*h), MEMF_ANY);

        if (h != NULL)
        {
            if (s_ask(base, NETSTATUS_HEALTH, h, sizeof(*h)) >= 0)
            {
                LONG targs[8];

                targs[0] = (LONG)s_secs();
                targs[1] = (LONG)why;
                targs[2] = (LONG)h->e.nsl_TickSkew;
                targs[3] = (LONG)h->e.nsl_TickSkewPeak;
                targs[4] = (LONG)h->e.nsl_TickLost;
                targs[5] = (LONG)h->e.nsl_TickDeferred;
                targs[6] = (LONG)h->e.nsl_TickUptimeMs;
                targs[7] = (LONG)h->e.nsl_TickWorstServiceUs;

                s_emit(F_EVENTS,
                       "%lu TICK %s skew=%lu skewpeak=%lu lost=%lu deferred=%lu "
                       "uptime_ms=%lu worst_service_us=%lu\n", targs);
            }
            FreeVec((APTR)h);
        }
    }

    FreeVec((APTR)b);

    s_dump_sockets(base, why);
}

static VOID s_fail(struct Library *base, ULONG arm, const char *what,
                   LONG ioerr, ULONG detail)
{
    LONG args[6];

    SS->ss_Drops = SS->ss_Drops + 1UL;

    args[0] = (LONG)s_secs();
    args[1] = (LONG)s_phase_name[SS->ss_CurPhase];
    args[2] = (LONG)((arm == ARM_WIRE) ? "wire" : "local");
    args[3] = (LONG)what;
    args[4] = ioerr;
    args[5] = (LONG)detail;

    s_emit(F_EVENTS, "%lu FAIL phase=%s arm=%s %s ioerr=%ld detail=%lu\n",
           args);

    if (base != NULL)
        s_snapshot(base, what);
}


/* ----------------------------------------------------------- the pattern -- */

#define PAT_BYTES   4096UL
#define PAT_MASK    (PAT_BYTES - 1UL)

static UBYTE s_pat[PAT_BYTES];

static ULONG s_xs(ULONG *s)
{
    ULONG x = *s;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;

    return x;
}

static VOID s_pat_init(VOID)
{
    ULONG seed = 0x5A17F00DUL;
    ULONG i;

    for (i = 0UL; i < PAT_BYTES; i++)
        s_pat[i] = (UBYTE)(s_xs(&seed) >> 19);
}

static VOID s_pat_fill(UBYTE *dst, ULONG off, ULONG len)
{
    while (len-- > 0UL)
        *dst++ = s_pat[(off++) & PAT_MASK];
}

static ULONG s_pat_check(const UBYTE *src, ULONG off, ULONG len)
{
    ULONG bad = 0UL;

    while (len-- > 0UL)
    {
        if (*src++ != s_pat[(off++) & PAT_MASK])
            bad++;
    }

    return bad;
}

/* 1..limit, never 0: a zero-length transfer tests nothing and hides a bug. */
static ULONG s_pick(ULONG *seed, ULONG limit)
{
    if (limit < 2UL)
        return 1UL;

    return 1UL + (s_xs(seed) % limit);
}


/* ---------------------------------------------------------- the filers --- */

static ULONG s_strcpy(char *dst, const char *src, ULONG max)
{
    ULONG i = 0UL;

    while (src[i] != '\0' && i + 1UL < max)
    {
        dst[i] = src[i];
        i++;
    }

    dst[i] = '\0';

    return i;
}

static VOID s_filename(char *dst, const char *dir, ULONG idx, ULONG slot)
{
    ULONG i = s_strcpy(dst, dir, S_NAME);
    char  digits[12];
    ULONG v = idx * 100UL + slot;
    int   d = 0;

    do
    {
        digits[d++] = (char)('0' + (v % 10UL));
        v /= 10UL;
    } while (v != 0UL);

    while (d > 0 && i + 6UL < S_NAME)
        dst[i++] = digits[--d];

    dst[i++] = '.';
    dst[i++] = 'd';
    dst[i++] = 'a';
    dst[i++] = 't';
    dst[i]   = '\0';
}

#define CALL_OPEN_W     1
#define CALL_WRITE      2
#define CALL_OPEN_R     3
#define CALL_READ       4
#define CALL_CLOSE      5
#define CALL_DELETE     6

static BOOL s_filer_write(SoakFiler *f, const char *name, ULONG len)
{
    BPTR  fh;
    ULONG done = 0UL;

    f->f_CallStart = s_ticks() | 1UL;
    f->f_CallKind  = CALL_OPEN_W;
    fh = Open((CONST_STRPTR)name, MODE_NEWFILE);
    f->f_CallStart = 0UL;

    if (fh == (BPTR)0)
    {
        f->f_Errors = f->f_Errors + 1UL;
        s_fail(f->f_Base, f->f_Arm, "open-write", IoErr(), len);
        return FALSE;
    }

    while (done < len)
    {
        ULONG chunk = s_pick(&f->f_Seed, SS->ss_MaxIo);
        LONG  rc;

        if (chunk > len - done)
            chunk = len - done;

        s_pat_fill(f->f_Buf, done, chunk);

        f->f_CallStart = s_ticks() | 1UL;
        f->f_CallKind  = CALL_WRITE;
        rc = Write(fh, f->f_Buf, (LONG)chunk);
        f->f_CallStart = 0UL;

        if (rc != (LONG)chunk)
        {
            f->f_Errors = f->f_Errors + 1UL;
            s_fail(f->f_Base, f->f_Arm, "write", IoErr(), done);
            Close(fh);
            return FALSE;
        }

        done += chunk;
        f->f_Bytes = f->f_Bytes + chunk;
        if (f->f_Bytes >= 1048576UL)
        {
            f->f_Bytes = f->f_Bytes - 1048576UL;
            f->f_Mega  = f->f_Mega + 1UL;
        }
    }

    f->f_CallStart = s_ticks() | 1UL;
    f->f_CallKind  = CALL_CLOSE;
    if (Close(fh) == 0)
    {
        f->f_CallStart = 0UL;
        f->f_Errors = f->f_Errors + 1UL;
        s_fail(f->f_Base, f->f_Arm, "close-write", IoErr(), len);
        return FALSE;
    }
    f->f_CallStart = 0UL;

    return TRUE;
}

static BOOL s_filer_read(SoakFiler *f, const char *name, ULONG len)
{
    BPTR  fh;
    ULONG done = 0UL;
    ULONG bad  = 0UL;

    f->f_CallStart = s_ticks() | 1UL;
    f->f_CallKind  = CALL_OPEN_R;
    fh = Open((CONST_STRPTR)name, MODE_OLDFILE);
    f->f_CallStart = 0UL;

    if (fh == (BPTR)0)
    {
        f->f_Errors = f->f_Errors + 1UL;
        s_fail(f->f_Base, f->f_Arm, "open-read", IoErr(), len);
        return FALSE;
    }

    while (done < len)
    {
        ULONG chunk = s_pick(&f->f_Seed, SS->ss_MaxIo);
        LONG  rc;

        if (chunk > len - done)
            chunk = len - done;

        f->f_CallStart = s_ticks() | 1UL;
        f->f_CallKind  = CALL_READ;
        rc = Read(fh, f->f_Buf, (LONG)chunk);
        f->f_CallStart = 0UL;

        if (rc != (LONG)chunk)
        {
            f->f_Errors = f->f_Errors + 1UL;
            s_fail(f->f_Base, f->f_Arm, "read", IoErr(), done);
            Close(fh);
            return FALSE;
        }

        bad += s_pat_check(f->f_Buf, done, chunk);

        done += chunk;
        f->f_Bytes = f->f_Bytes + chunk;
        if (f->f_Bytes >= 1048576UL)
        {
            f->f_Bytes = f->f_Bytes - 1048576UL;
            f->f_Mega  = f->f_Mega + 1UL;
        }
    }

    f->f_CallStart = s_ticks() | 1UL;
    f->f_CallKind  = CALL_CLOSE;
    (VOID)Close(fh);
    f->f_CallStart = 0UL;

    if (bad != 0UL)
    {
        f->f_Corrupt = f->f_Corrupt + bad;
        s_fail(f->f_Base, f->f_Arm, "payload-corrupt", 0, bad);
        return FALSE;
    }

    return TRUE;
}

static VOID s_filer_cycle(SoakFiler *f)
{
    char  name[S_NAME];
    ULONG len = s_pick(&f->f_Seed, SS->ss_MaxXfer);

    s_filename(name, f->f_Dir, (ULONG)f->f_Index, f->f_Files % 8UL);

    if (!s_filer_write(f, name, len))
        return;

    if (!s_filer_read(f, name, len))
        return;

    f->f_CallStart = s_ticks() | 1UL;
    f->f_CallKind  = CALL_DELETE;
    if (DeleteFile((CONST_STRPTR)name) == 0)
    {
        f->f_CallStart = 0UL;
        f->f_Errors = f->f_Errors + 1UL;
        s_fail(f->f_Base, f->f_Arm, "delete", IoErr(), len);
        return;
    }
    f->f_CallStart = 0UL;

    f->f_Files = f->f_Files + 1UL;
}

static VOID s_filer_body(SoakFiler *f)
{
    while (f->f_Stop == 0U)
    {
        if (f->f_Run == 0U)
        {
            Delay(25UL);
            continue;
        }

        s_filer_cycle(f);
    }
}

static VOID s_filer_entry(VOID)
{
    struct Process *me = (struct Process *)FindTask((STRPTR)0);
    SoakFiler      *f;

    Wait(SIGF_SINGLE);

    f = (SoakFiler *)me->pr_Task.tc_UserData;
    if (f == NULL)
        return;

    f->f_Buf = (UBYTE *)AllocVec(SS->ss_MaxIo, MEMF_PUBLIC);
    if (f->f_Buf == NULL)
    {
        f->f_Done = 1U;
        return;
    }

    f->f_Base = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4UL);

    f->f_Alive = 1U;
    s_filer_body(f);

    if (f->f_Base != NULL)
    {
        CloseLibrary(f->f_Base);
        f->f_Base = NULL;
    }

    FreeVec(f->f_Buf);
    f->f_Buf   = NULL;
    f->f_Alive = 0U;
    f->f_Done  = 1U;
}


/* ---------------------------------------------------------- the churner --- */

#define CHURN_STACK     (128UL * 1024UL)

static struct TagItem s_churn_tags[] =
{
    { SYS_Input,    0           },
    { SYS_Output,   0           },
    { NP_StackSize, CHURN_STACK },
    { NP_WindowPtr, (ULONG)-1   },
    { TAG_DONE,     0           }
};

static LONG s_run_query(const char *host, ULONG port)
{
    char  cmd[128];
    ULONG i = 0UL;
    char  digits[8];
    int   d = 0;
    ULONG v = port;
    BPTR  in, out;
    LONG  rc;

    i += s_strcpy(&cmd[i], "SYS:fitz query ", sizeof(cmd) - i);
    i += s_strcpy(&cmd[i], host, sizeof(cmd) - i);
    cmd[i++] = ':';

    do
    {
        digits[d++] = (char)('0' + (v % 10UL));
        v /= 10UL;
    } while (v != 0UL);

    while (d > 0)
        cmd[i++] = digits[--d];

    cmd[i] = '\0';

    in  = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
    out = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);

    s_churn_tags[0].ti_Data = (ULONG)in;
    s_churn_tags[1].ti_Data = (ULONG)out;

    rc = SystemTagList((CONST_STRPTR)cmd, s_churn_tags);

    /* SystemTagList() closes the streams it was given, on every path. */
    return rc;
}

static VOID s_churn_once(ULONG arm)
{
    const char *host = (arm == ARM_WIRE) ? SS->ss_WireHost : "127.0.0.1";
    ULONG       port = (arm == ARM_WIRE) ? SS->ss_WirePort : SS->ss_ServePort;
    LONG        rc;

    SS->ss_ChurnCallStart = s_ticks() | 1UL;
    rc = s_run_query(host, port);
    SS->ss_ChurnCallStart = 0UL;

    SS->ss_Queries[arm] = SS->ss_Queries[arm] + 1UL;

    if (rc != 0)
    {
        SS->ss_QueryFail[arm] = SS->ss_QueryFail[arm] + 1UL;
        s_fail(SS->ss_ChurnBase, arm, "query", rc, SS->ss_Queries[arm]);
    }
}

static VOID s_churn_body(VOID)
{
    ULONG arm  = 0UL;
    ULONG idle = 0UL;

    while (SS->ss_ChurnStop == 0U)
    {
        if (SS->ss_ChurnRun == 0U)
        {
            if (SS->ss_ChurnEvery != 0UL && idle >= SS->ss_ChurnEvery)
            {
                idle = 0UL;
                s_churn_once(arm);
                arm = (arm + 1UL) % ARM_COUNT;
            }
            else
            {
                Delay(50UL);
                idle += 1UL;
            }
            continue;
        }

        s_churn_once(arm);
        arm = (arm + 1UL) % ARM_COUNT;
    }
}

static VOID s_churn_entry(VOID)
{
    Wait(SIGF_SINGLE);

    SS->ss_ChurnBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4UL);

    SS->ss_ChurnAlive = 1U;
    s_churn_body();
    SS->ss_ChurnAlive = 0U;

    if (SS->ss_ChurnBase != NULL)
    {
        CloseLibrary(SS->ss_ChurnBase);
        SS->ss_ChurnBase = NULL;
    }

    SS->ss_ChurnDone  = 1U;
}


/* ------------------------------------------------------------- sampling --- */

static VOID s_header(VOID)
{
    LONG args[1];

    args[0] = 0;
    s_emit(F_TIMELINE,
           "t_s,phase,avail_pub,avail_largest,pool_total,pool_free,"
           "pool_empty_req,pool_empty_susp,sockets,tcp_conn,tcp_disc,"
           "tcp_dropped,tcp_retrans,tcp_rx_drop,tcp_cksum,ip_rx_drop,"
           "ip_tx_drop,sana_alloc_fail,sana_overrun,sana_rxerr,sana_txerr,"
           "files_wire,files_local,mb_wire,mb_local,errors,queries,"
           "query_fail,est_socks\n", args);
}

static VOID s_sample(struct Library *base, ULONG t)
{
    SoakSysBuf  sys;
    SoakStatBuf st;
    SoakIfBuf   ifb;
    SoakSockBuf sk;
    LONG        args[29];
    ULONG       i;
    ULONG       files[ARM_COUNT];
    ULONG       mb[ARM_COUNT];
    ULONG       errs = 0UL;
    ULONG       socks = 0UL, est = 0UL;
    ULONG       alloc_fail = 0UL, overrun = 0UL, rxerr = 0UL, txerr = 0UL;

    files[0] = files[1] = 0UL;
    mb[0]    = mb[1]    = 0UL;

    for (i = 0UL; i < (ULONG)SS->ss_FilerCount; i++)
    {
        SoakFiler *f = &SS->ss_F[i];

        files[f->f_Arm] += f->f_Files;
        mb[f->f_Arm]    += f->f_Mega;
        errs            += f->f_Errors + f->f_Corrupt;
    }

    s_zero(&sys, sizeof(sys));
    s_zero(&st,  sizeof(st));

    (VOID)s_ask(base, NETSTATUS_SYSTEM, &sys, sizeof(sys));
    (VOID)s_ask(base, NETSTATUS_STATS,  &st,  sizeof(st));

    if (s_ask(base, NETSTATUS_INTERFACES, &ifb, sizeof(ifb)) > 0)
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

    if (s_ask(base, NETSTATUS_SOCKETS, &sk, sizeof(sk)) >= 0)
    {
        UWORD n = sk.hdr.nsh_Count;
        UWORD j;

        socks = (ULONG)sk.hdr.nsh_Available;

        for (j = 0; j < n; j++)
        {
            if ((sk.e[j].nso_Flags & NETSTATUS_SOCK_TCP) != 0 &&
                sk.e[j].nso_State == NETSTATUS_TCP_ESTABLISHED)
                est++;
        }
    }

    args[0]  = (LONG)t;
    args[1]  = (LONG)s_phase_name[SS->ss_CurPhase];
    args[2]  = (LONG)AvailMem(MEMF_PUBLIC);
    args[3]  = (LONG)AvailMem(MEMF_PUBLIC | MEMF_LARGEST);
    args[4]  = (LONG)sys.e.nss_PoolTotal;
    args[5]  = (LONG)sys.e.nss_PoolFree;
    args[6]  = (LONG)sys.e.nss_PoolEmptyRequests;
    args[7]  = (LONG)sys.e.nss_PoolEmptySuspensions;
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
    args[21] = (LONG)files[ARM_WIRE];
    args[22] = (LONG)files[ARM_LOCAL];
    args[23] = (LONG)mb[ARM_WIRE];
    args[24] = (LONG)mb[ARM_LOCAL];
    args[25] = (LONG)errs;
    args[26] = (LONG)(SS->ss_Queries[0] + SS->ss_Queries[1]);
    args[27] = (LONG)(SS->ss_QueryFail[0] + SS->ss_QueryFail[1]);
    args[28] = (LONG)est;

    s_emit(F_TIMELINE,
           "%lu,%s,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,"
           "%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu\n", args);
}

#define S_STALL_TICKS   (180UL * 50UL)

static ULONG s_in_call(ULONG start)
{
    ULONG now = s_ticks();

    if (start == 0UL || now < start)
        return 0UL;

    return now - start;
}

static VOID s_check_stalls(struct Library *base, UWORD *reported)
{
    ULONG i;

    for (i = 0UL; i < (ULONG)SS->ss_FilerCount; i++)
    {
        SoakFiler *f       = &SS->ss_F[i];
        ULONG      elapsed = s_in_call(f->f_CallStart);

        if (elapsed == 0UL || reported[i] != 0U)
            continue;

        if (elapsed > S_STALL_TICKS)
        {
            LONG args[5];

            reported[i] = 1U;

            args[0] = (LONG)s_secs();
            args[1] = (LONG)((f->f_Arm == ARM_WIRE) ? "wire" : "local");
            args[2] = (LONG)f->f_Index;
            args[3] = (LONG)f->f_CallKind;
            args[4] = (LONG)(elapsed / 50UL);

            s_emit(F_EVENTS, "%lu STALL arm=%s filer=%lu call=%lu "
                             "seconds=%lu\n", args);
            s_snapshot(base, "stall");
        }
        else if (elapsed < S_STALL_TICKS / 4UL)
        {
            reported[i] = 0U;
        }
    }
}


/* --------------------------------------------------------- the post-idle -- */

static VOID s_post_idle_probe(struct Library *base, ULONG arm)
{
    char           name[S_NAME];
    BPTR           fh;
    ULONG          t0 = s_ticks();
    ULONG          took;
    LONG           args[5];
    LONG           err = 0;
    static UBYTE   probe[64];
    ULONG          at = s_strcpy(name, SS->ss_Vol[arm], S_NAME);

    s_strcpy(&name[at], "postidle.dat", S_NAME - at);

    fh = Open((CONST_STRPTR)name, MODE_NEWFILE);
    if (fh == (BPTR)0)
    {
        err  = IoErr();
        took = (s_ticks() - t0) / 50UL;
    }
    else
    {
        s_pat_fill(probe, 0UL, sizeof(probe));

        if (Write(fh, probe, (LONG)sizeof(probe)) != (LONG)sizeof(probe))
            err = IoErr();

        (VOID)Close(fh);
        took = (s_ticks() - t0) / 50UL;

        if (err == 0 && DeleteFile((CONST_STRPTR)name) == 0)
            err = IoErr();
    }

    args[0] = (LONG)s_secs();
    args[1] = (LONG)((arm == ARM_WIRE) ? "wire" : "local");
    args[2] = err;
    args[3] = (LONG)took;
    args[4] = (LONG)SS->ss_Phase;

    s_emit(F_EVENTS, "%lu POSTIDLE arm=%s ioerr=%ld seconds=%lu after=%lu\n",
           args);

    if (err != 0)
        s_fail(base, arm, "post-idle", err, took);
}


/* ------------------------------------------------------------ the config -- */

static ULONG s_atoi(const char *s)
{
    ULONG v = 0UL;

    while (*s >= '0' && *s <= '9')
        v = v * 10UL + (ULONG)(*s++ - '0');

    return v;
}

static BOOL s_key(char *line, const char *key, char **val)
{
    ULONG i = 0UL;

    while (key[i] != '\0')
    {
        if (line[i] != key[i])
            return FALSE;
        i++;
    }

    if (line[i] != ' ' && line[i] != '\t')
        return FALSE;

    while (line[i] == ' ' || line[i] == '\t')
        i++;

    *val = &line[i];

    return TRUE;
}

static VOID s_defaults(VOID)
{
    SS->ss_Seconds    = 3600UL;
    SS->ss_Sample     = 15UL;
    SS->ss_Phase      = 300UL;
    SS->ss_MaxIo      = 32768UL;
    SS->ss_MaxXfer    = 262144UL;
    SS->ss_Seed       = 20260729UL;
    SS->ss_ChurnEvery = 30UL;
    SS->ss_Filers     = 1U;
    SS->ss_ServePort  = 17822U;
    SS->ss_WirePort   = 17821U;

    s_strcpy(SS->ss_WireHost, "10.0.2.2", sizeof(SS->ss_WireHost));
    s_strcpy(SS->ss_Vol[ARM_WIRE],  "FITZW:", S_PATH);
    s_strcpy(SS->ss_Vol[ARM_LOCAL], "FITZL:", S_PATH);
}

static VOID s_read_config(VOID)
{
    BPTR  f = Open((CONST_STRPTR)"DH0:fitzsoak.cfg", MODE_OLDFILE);
    char  line[128];
    char *v;

    if (f == (BPTR)0)
        return;

    while (FGets(f, (STRPTR)line, (ULONG)sizeof(line)) != NULL)
    {
        ULONG n = 0UL;

        while (line[n] != '\0' && line[n] != '\n' && line[n] != '\r')
            n++;
        line[n] = '\0';

        if (line[0] == '#' || line[0] == '\0')
            continue;

        if      (s_key(line, "seconds",    &v)) SS->ss_Seconds    = s_atoi(v);
        else if (s_key(line, "sample",     &v)) SS->ss_Sample     = s_atoi(v);
        else if (s_key(line, "phase",      &v)) SS->ss_Phase      = s_atoi(v);
        else if (s_key(line, "maxio",      &v)) SS->ss_MaxIo      = s_atoi(v);
        else if (s_key(line, "maxxfer",    &v)) SS->ss_MaxXfer    = s_atoi(v);
        else if (s_key(line, "seed",       &v)) SS->ss_Seed       = s_atoi(v);
        else if (s_key(line, "churnevery", &v)) SS->ss_ChurnEvery = s_atoi(v);
        else if (s_key(line, "filers",     &v)) SS->ss_Filers     = (UWORD)s_atoi(v);
        else if (s_key(line, "serveport",  &v)) SS->ss_ServePort  = (UWORD)s_atoi(v);
        else if (s_key(line, "wireport",   &v)) SS->ss_WirePort   = (UWORD)s_atoi(v);
        else if (s_key(line, "wirehost",   &v)) s_strcpy(SS->ss_WireHost, v, sizeof(SS->ss_WireHost));
        else if (s_key(line, "wirevol",    &v)) s_strcpy(SS->ss_Vol[ARM_WIRE],  v, S_PATH);
        else if (s_key(line, "localvol",   &v)) s_strcpy(SS->ss_Vol[ARM_LOCAL], v, S_PATH);
    }

    Close(f);

    if (SS->ss_Filers > S_MAX_FILERS) SS->ss_Filers = S_MAX_FILERS;
    if (SS->ss_Filers < 1U)           SS->ss_Filers = 1U;
    if (SS->ss_Sample < 5UL)          SS->ss_Sample = 5UL;
    if (SS->ss_Phase  < 30UL)         SS->ss_Phase  = 30UL;
    if (SS->ss_MaxIo  < 512UL)        SS->ss_MaxIo  = 512UL;
}


/* --------------------------------------------------------------- bring-up -- */

static struct TagItem s_bg_tags[] =
{
    { SYS_Input,    0           },
    { SYS_Output,   0           },
    { SYS_Asynch,   (ULONG)TRUE },
    { NP_StackSize, 512UL * 1024UL },
    { NP_WindowPtr, (ULONG)-1   },
    { TAG_DONE,     0           }
};

static struct TagItem s_fg_tags[] =
{
    { SYS_Input,    0           },
    { SYS_Output,   0           },
    { NP_StackSize, 512UL * 1024UL },
    { NP_WindowPtr, (ULONG)-1   },
    { TAG_DONE,     0           }
};

/* 512 KB: on a Shell default stack a third-party Amiga binary fails in a way
   that looks like a network fault rather than a crash. */
static LONG s_run_bg(const char *cmd)
{
    BPTR in  = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
    BPTR out = Open((CONST_STRPTR)"DH0:soak-fitz.txt", MODE_READWRITE);

    if (out != (BPTR)0)
        Seek(out, 0, OFFSET_END);

    s_bg_tags[0].ti_Data = (ULONG)in;
    s_bg_tags[1].ti_Data = (ULONG)out;

    return SystemTagList((CONST_STRPTR)cmd, s_bg_tags);
}

static LONG s_run_fg(const char *cmd)
{
    BPTR in  = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
    BPTR out = Open((CONST_STRPTR)"DH0:soak-fitz.txt", MODE_READWRITE);

    if (out != (BPTR)0)
        Seek(out, 0, OFFSET_END);

    s_fg_tags[0].ti_Data = (ULONG)in;
    s_fg_tags[1].ti_Data = (ULONG)out;

    return SystemTagList((CONST_STRPTR)cmd, s_fg_tags);
}

static VOID s_note(const char *what, LONG detail)
{
    LONG args[3];

    args[0] = (LONG)s_secs();
    args[1] = (LONG)what;
    args[2] = detail;

    s_emit(F_EVENTS, "%lu NOTE %s %ld\n", args);
}

static BOOL s_wait_online(struct Library *base, ULONG limit_s)
{
    SoakIfBuf ifb;
    ULONG     waited = 0UL;

    while (waited < limit_s)
    {
        if (s_ask(base, NETSTATUS_INTERFACES, &ifb, sizeof(ifb)) > 0)
        {
            UWORD n = ifb.hdr.nsh_Count;
            UWORD j;

            for (j = 0; j < n && j < 4; j++)
            {
                if ((ifb.e[j].nsi_Flags & NETSTATUS_IF_ONLINE) != 0 &&
                    ifb.e[j].nsi_Address != 0UL)
                {
                    s_note("online", (LONG)ifb.e[j].nsi_Address);
                    return TRUE;
                }
            }
        }

        Delay(50UL);
        waited++;
    }

    return FALSE;
}

static VOID s_number(char *dst, ULONG *at, ULONG v)
{
    char digits[8];
    int  d = 0;

    do
    {
        digits[d++] = (char)('0' + (v % 10UL));
        v /= 10UL;
    } while (v != 0UL);

    while (d > 0)
        dst[(*at)++] = digits[--d];
}


/* --------------------------------------------------------------- spawning -- */

static struct Process *s_spawn(APTR entry, APTR user, const char *name)
{
    struct Process *p;

    Forbid();

    p = CreateNewProcTags(NP_Entry,     (ULONG)entry,
                          NP_Name,      (ULONG)name,
                          NP_Priority,  (ULONG)0,
                          NP_StackSize, (ULONG)32768,
                          NP_Cli,       (ULONG)FALSE,
                          TAG_DONE);

    if (p != NULL)
        p->pr_Task.tc_UserData = user;

    Permit();

    if (p != NULL)
        Signal(&p->pr_Task, SIGF_SINGLE);

    return p;
}


/* ------------------------------------------------------------- the summary -- */

static VOID s_summary(struct Library *base, ULONG ran)
{
    SoakSysBuf sys;
    LONG       args[16];
    ULONG      i;
    ULONG      files[ARM_COUNT];
    ULONG      mb[ARM_COUNT];
    ULONG      errs = 0UL, corrupt = 0UL;

    files[0] = files[1] = 0UL;
    mb[0]    = mb[1]    = 0UL;

    for (i = 0UL; i < (ULONG)SS->ss_FilerCount; i++)
    {
        SoakFiler *f = &SS->ss_F[i];

        files[f->f_Arm] += f->f_Files;
        mb[f->f_Arm]    += f->f_Mega;
        errs            += f->f_Errors;
        corrupt         += f->f_Corrupt;
    }

    s_zero(&sys, sizeof(sys));
    (VOID)s_ask(base, NETSTATUS_SYSTEM, &sys, sizeof(sys));

    args[0]  = (LONG)ran;
    args[1]  = (LONG)files[ARM_WIRE];
    args[2]  = (LONG)mb[ARM_WIRE];
    args[3]  = (LONG)files[ARM_LOCAL];
    args[4]  = (LONG)mb[ARM_LOCAL];
    args[5]  = (LONG)SS->ss_Queries[ARM_WIRE];
    args[6]  = (LONG)SS->ss_QueryFail[ARM_WIRE];
    args[7]  = (LONG)SS->ss_Queries[ARM_LOCAL];
    args[8]  = (LONG)SS->ss_QueryFail[ARM_LOCAL];
    args[9]  = (LONG)errs;
    args[10] = (LONG)corrupt;
    args[11] = (LONG)SS->ss_Drops;
    args[12] = (LONG)AvailMem(MEMF_PUBLIC);
    args[13] = (LONG)AvailMem(MEMF_PUBLIC | MEMF_LARGEST);
    args[14] = (LONG)sys.e.nss_PoolFree;
    args[15] = (LONG)sys.e.nss_PoolTotal;

    s_emit(F_SUMMARY,
           "seconds %lu\n"
           "wire_files %lu\nwire_mb %lu\n"
           "local_files %lu\nlocal_mb %lu\n"
           "wire_queries %lu\nwire_query_fail %lu\n"
           "local_queries %lu\nlocal_query_fail %lu\n"
           "io_errors %lu\ncorrupt_bytes %lu\nfailures %lu\n"
           "avail_end %lu\nlargest_end %lu\n"
           "pool_free_end %lu\npool_total %lu\n", args);
}

/* AmigaOS does not reclaim AllocVec() memory when a process exits, and every
   filer lives inside SS, so this frees only once no worker is left running. */
static VOID s_release(VOID)
{
    ULONG live = 0UL;
    ULONG i;

    if (SS == NULL)
        return;

    for (i = 0UL; i < (ULONG)SS->ss_FilerCount; i++)
    {
        if (SS->ss_F[i].f_Proc != NULL && SS->ss_F[i].f_Done == 0U)
            live++;
    }

    if (SS->ss_ChurnProc != NULL && SS->ss_ChurnDone == 0U)
        live++;

    if (live == 0UL)
    {
        FreeVec(SS);
        SS = NULL;
    }
}


/* ------------------------------------------------------------------ main -- */

int main(void)
{
    struct Library *base;
    char            cmd[160];
    ULONG           i, at;
    ULONG           t, next_sample;
    UWORD           stall_reported[ARM_COUNT * S_MAX_FILERS];
    ULONG           last_phase;
    ULONG           wedged;         /* workers still inside a call at the end */

    s_t0 = 0UL;
    s_t0 = s_ticks();

    SS = (SoakState *)AllocVec(sizeof(SoakState), MEMF_PUBLIC | MEMF_CLEAR);
    if (SS == NULL)
    {
        PutStr((CONST_STRPTR)"FitzSoak: out of memory\n");
        return RETURN_FAIL;
    }
    (VOID)atexit(s_release);

    InitSemaphore(&s_out_sem);
    s_pat_init();
    s_defaults();
    s_read_config();

    s_truncate(F_TIMELINE);
    s_truncate(F_EVENTS);
    s_truncate(F_SUMMARY);
    s_truncate("DH0:soak-fitz.txt");
    s_header();

    s_note("start", (LONG)SS->ss_Seconds);

    /* ---- the network -------------------------------------------------- */

    (VOID)s_run_fg("SYS:AddNetInterface eth0");

    base = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4UL);
    if (base == NULL)
    {
        s_note("no-bsdsocket", 0);
        PutStr((CONST_STRPTR)"FitzSoak: no bsdsocket.library\n");
        return RETURN_FAIL;
    }
    SS->ss_Base = base;

    if (!s_wait_online(base, 60UL))
    {
        s_note("interface-never-online", 0);
        PutStr((CONST_STRPTR)"FitzSoak: eth0 never came online\n");
        CloseLibrary(base);
        return RETURN_FAIL;
    }

    /* ---- the guest's own server --------------------------------------- */

    at = 0UL;
    at += s_strcpy(&cmd[at], "SYS:fitz serve DH0:localshare PORT ", sizeof(cmd) - at);
    s_number(cmd, &at, (ULONG)SS->ss_ServePort);
    cmd[at] = '\0';

    (VOID)s_run_bg(cmd);
    s_note("serve-started", (LONG)SS->ss_ServePort);
    Delay(250UL);

    /* ---- the two mounts ------------------------------------------------ */

    at = 0UL;
    at += s_strcpy(&cmd[at], "SYS:fitz mount ", sizeof(cmd) - at);
    at += s_strcpy(&cmd[at], SS->ss_WireHost, sizeof(cmd) - at);
    cmd[at++] = ':';
    s_number(cmd, &at, (ULONG)SS->ss_WirePort);
    cmd[at++] = ' ';
    at += s_strcpy(&cmd[at], SS->ss_Vol[ARM_WIRE], sizeof(cmd) - at);
    cmd[at] = '\0';
    (VOID)s_run_bg(cmd);
    s_note("wire-mount-started", (LONG)SS->ss_WirePort);

    at = 0UL;
    at += s_strcpy(&cmd[at], "SYS:fitz mount 127.0.0.1:", sizeof(cmd) - at);
    s_number(cmd, &at, (ULONG)SS->ss_ServePort);
    cmd[at++] = ' ';
    at += s_strcpy(&cmd[at], SS->ss_Vol[ARM_LOCAL], sizeof(cmd) - at);
    cmd[at] = '\0';
    (VOID)s_run_bg(cmd);
    s_note("local-mount-started", (LONG)SS->ss_ServePort);

    Delay(500UL);
    s_snapshot(base, "after-mounts");

    /* ---- the workers --------------------------------------------------- */

    SS->ss_FilerCount = 0U;

    for (i = 0UL; i < (ULONG)ARM_COUNT; i++)
    {
        ULONG k;

        for (k = 0UL; k < (ULONG)SS->ss_Filers; k++)
        {
            SoakFiler *f = &SS->ss_F[SS->ss_FilerCount];
            ULONG      n = 0UL;

            f->f_Index = SS->ss_FilerCount;
            f->f_Arm   = (UWORD)i;
            f->f_Seed  = SS->ss_Seed + SS->ss_FilerCount * 7919UL + 1UL;

            n = s_strcpy(f->f_Dir, SS->ss_Vol[i], S_PATH);
            f->f_Dir[n] = '\0';

            stall_reported[SS->ss_FilerCount] = 0U;

            f->f_Proc = s_spawn((APTR)s_filer_entry, (APTR)f, "fitzsoak filer");
            if (f->f_Proc == NULL)
            {
                s_note("filer-spawn-failed", (LONG)SS->ss_FilerCount);
                break;
            }

            SS->ss_FilerCount++;
        }
    }

    SS->ss_ChurnProc = s_spawn((APTR)s_churn_entry, NULL, "fitzsoak churner");
    if (SS->ss_ChurnProc == NULL)
        s_note("churner-spawn-failed", 0);

    Delay(100UL);

    /* ---- the run ------------------------------------------------------- */

    t           = 0UL;
    next_sample = 0UL;
    last_phase  = (ULONG)-1;

    while (t < SS->ss_Seconds)
    {
        ULONG phase = ((t / SS->ss_Phase) % (ULONG)PH_COUNT);

        if (phase != last_phase)
        {
            LONG args[3];
            ULONG k;

            if (last_phase == (ULONG)PH_IDLE1 || last_phase == (ULONG)PH_IDLE2)
            {
                s_post_idle_probe(base, ARM_WIRE);
                s_post_idle_probe(base, ARM_LOCAL);
            }

            SS->ss_CurPhase = phase;

            args[0] = (LONG)t;
            args[1] = (LONG)s_phase_name[phase];
            args[2] = (LONG)phase;
            s_emit(F_EVENTS, "%lu PHASE %s (%lu)\n", args);

            for (k = 0UL; k < (ULONG)SS->ss_FilerCount; k++)
            {
                SoakFiler *f  = &SS->ss_F[k];
                UWORD      go = 0U;

                if (phase == (ULONG)PH_WIRE  && f->f_Arm == ARM_WIRE)  go = 1U;
                if (phase == (ULONG)PH_LOCAL && f->f_Arm == ARM_LOCAL) go = 1U;
                if (phase == (ULONG)PH_BOTH)                           go = 1U;

                f->f_Run = go;
            }

            SS->ss_ChurnRun = (phase == (ULONG)PH_CHURN) ? 1U : 0U;

            last_phase = phase;
        }

        if (t >= next_sample)
        {
            s_sample(base, t);
            next_sample = t + SS->ss_Sample;
        }

        s_check_stalls(base, stall_reported);

        Delay(50UL);
        t = s_secs();

        /* Delay() is a floor, not a clock; a busy machine can put t past the
           next sample by more than one interval and the CSV would then have
           holes with no row saying so. */
        if (t > next_sample + SS->ss_Sample * 4UL)
        {
            s_note("sampling-fell-behind", (LONG)(t - next_sample));
            next_sample = t;
        }
    }

    /* ---- teardown ------------------------------------------------------ */

    s_snapshot(base, "end-of-run");

    for (i = 0UL; i < (ULONG)SS->ss_FilerCount; i++)
    {
        SS->ss_F[i].f_Run  = 0U;
        SS->ss_F[i].f_Stop = 1U;
    }
    SS->ss_ChurnRun  = 0U;
    SS->ss_ChurnStop = 1U;

    /* A filer inside a DOS call on a mount that has gone will not come back
       until the handler times out, so this waits and then reports rather than
       hanging the run. */
    for (i = 0UL; i < 600UL; i++)
    {
        ULONG live = 0UL;
        ULONG k;

        for (k = 0UL; k < (ULONG)SS->ss_FilerCount; k++)
        {
            if (SS->ss_F[k].f_Done == 0U)
                live++;
        }

        if (SS->ss_ChurnDone == 0U)
            live++;

        if (live == 0UL)
            break;

        Delay(50UL);
    }

    wedged = 0UL;
    {
        ULONG k;

        for (k = 0UL; k < (ULONG)SS->ss_FilerCount; k++)
        {
            if (SS->ss_F[k].f_Done == 0U)
                wedged++;
        }

        if (wedged != 0UL || SS->ss_ChurnDone == 0U)
            s_note("workers-still-in-a-call", (LONG)wedged);
    }

    s_summary(base, s_secs());

    Printf((CONST_STRPTR)"FitzSoak: %lu s, %lu failures, %lu queries "
                         "(%lu failed)\n",
           s_secs(), SS->ss_Drops,
           SS->ss_Queries[0] + SS->ss_Queries[1],
           SS->ss_QueryFail[0] + SS->ss_QueryFail[1]);

    CloseLibrary(base);

    /* Nonzero is reserved for a failure of the harness itself; a drop is a
       result and is read out of soak-events.txt. */
    if (wedged != 0UL || SS->ss_ChurnDone == 0U)
    {
        PutStr((CONST_STRPTR)"FitzSoak: workers are still inside a call\n");
        return RETURN_ERROR;
    }
    if (SS->ss_FilerCount == 0U)
    {
        PutStr((CONST_STRPTR)"FitzSoak: no filer ever started\n");
        return RETURN_ERROR;
    }
    if ((SS->ss_Queries[0] + SS->ss_Queries[1]) == 0UL)
    {
        PutStr((CONST_STRPTR)"FitzSoak: no query completed in either arm, so "
                             "nothing was measured\n");
        return RETURN_ERROR;
    }

    return RETURN_OK;
}
