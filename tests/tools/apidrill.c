/*
 * ApiDrill, every LVO in bsdsocket.library called in a loop with the machine
 * counted before and after.
 *
 * The commands reach about thirty entry points.  The library has 150, and the
 * ones no command touches are the ones nothing has ever watched.  AmigaOS
 * reclaims nothing when a process exits, so a vector that allocates and does
 * not free is a permanent loss until reboot, and a vector that leaks only on
 * the path where it refuses is the shape that survives longest, because the
 * refusal is what a caller expected anyway.
 *
 * The table below has one row per LVO, in LVO order, and the row's offset is
 * checked against its position at startup.  A vector nobody wrote a row for
 * therefore shifts every later row and fails that check, rather than quietly
 * not being covered.  Rows that cannot be called carry the reason and are
 * counted as uncovered; they are not passes.
 *
 * Each row is called once and the result thrown away, then called `iters`
 * times with a sample taken either side.  The first call is excluded because a
 * lazily built cache, a first-touch buffer or a table parsed on demand is paid
 * once and is not a leak.  What is measured across the remaining calls:
 *
 *   AvailMem(MEMF_ANY)         everything, including allocations the library
 *                              makes outside ami_alloc()
 *   NETSTATUS_HEALTH           nsl_AllocLive, nsl_Sockets, nsl_PoolBadRelease
 *   tc_SigAlloc                signal bits taken out of THIS Task
 *   SysBase->PortList          message ports
 *   SysBase->SemaphoreList     semaphores
 *   TaskReady + TaskWait       tasks and processes
 *
 * The last four are on the caller, not in the library, and are the ones
 * AvailMem cannot separate.  bsd_child_destroy() is only correct on the owning
 * task -- ami_signal_free() would free a bit in the CALLING task's tc_SigAlloc
 * -- so an asymmetry there shows in tc_SigAlloc and nowhere else.
 *
 * Iteration count.  The gate is bytes-per-call, computed as the AvailMem drop
 * divided by `iters`, and the smallest thing AmigaOS can lose is one allocator
 * quantum, 8 bytes.  iters is chosen so 8 bytes a call is well clear of the
 * noise floor: the two `!noise` control rows run the identical sampling
 * bracket around no library call at all, at both iteration counts in use, and
 * assert the floor is zero.  If the machine is noisy those rows go red and say
 * so rather than every row going red for the wrong reason.  64 is the default,
 * 16 for rows whose bracket is expensive (a route added and deleted, a DHCP
 * message created and replied, a library opened and closed); at 16 an 8-byte
 * leak is still 128 bytes against a floor of zero.
 *
 * Failure paths are variants, not afterthoughts.  Most rows have two: the call
 * as it is meant to be made, and the call refused.  SocketBaseTagList()
 * discards every tag after one it refuses (errno.c), so its refusing variant
 * asks for a list whose refused tag sits in the middle, which is where a
 * half-applied allocation would hide.
 *
 * The stubs are rows too.  bsd_enosys() must return -1, bsd_enosys_ptr() NULL
 * and bsd_enosys_bool() FALSE, and each is asserted on every call: a stub that
 * allocates before failing is a leak with no feature attached to it.
 *
 * BROKEN runs the deliberately-broken control rows instead of the table: one
 * leaks memory, one a signal bit, one a socket, one a message port, one a
 * semaphore, and one asserts the wrong value for a stub.  Every gate has to
 * fire.  A drill whose assertions have quietly stopped firing reports no
 * failures either way, and that is the only thing that tells the two apart.
 *
 * Vectors are called by hand at their LVOs, the way cycledrill.c and ifprobe.c
 * do: the NDK inlines assume a global SocketBase and go through the compiler's
 * idea of each prototype, and the point here is to call the vector table.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/libraries.h>
#include <exec/tasks.h>
#include <exec/ports.h>
#include <exec/semaphores.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <devices/timer.h>
#include <utility/tagitem.h>
#include <utility/hooks.h>

/* <libraries/bsdsocket.h> pulls in <sys/socket.h>, which uses size_t and
   ssize_t without declaring them.  Same ordering note as ifprobe.c. */
#include <stddef.h>
#include <sys/types.h>
#include <libraries/bsdsocket.h>
#include <netdb.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include "aminetxduo/netstatus.h"

static const char version_tag[] __attribute__((used)) =
    "$VER: ApiDrill 1.0 (9.8.2026)";

#define TEMPLATE    "ITERS/N,IFACE/K,ONLY/K,BROKEN/S"

enum
{
    ARG_ITERS = 0,
    ARG_IFACE,
    ARG_ONLY,
    ARG_BROKEN,
    ARG_COUNT
};

#define LIB_NAME        "bsdsocket.library"

#define DEF_ITERS       64
#define FEW_ITERS       16
#define MAX_ITERS       512

#define DEF_IFACE       "eth0"

/* ------------------------------------------------------------ the ABI ---- */

/* Open-coded rather than taken from the NDK, for the same reason
   src/tools/toolsock.h open-codes them: this program must build against a
   stock NDK and must not share a definition with the library it is testing. */
#define P_AF_UNSPEC     0
#define P_AF_INET       2
#define P_AF_INET6      23
#define P_SOCK_STREAM   1
#define P_SOCK_DGRAM    2
#define P_IPPROTO_TCP   6

#define P_SOL_SOCKET    0xffff
#define P_SO_REUSEADDR  0x0004
#define P_SO_TYPE       0x1008

#define P_IOC_VOID      0x20000000UL
#define P_IOC_OUT       0x40000000UL
#define P_IOC_IN        0x80000000UL
#define P_IOC(io, g, n, l) \
    ((ULONG)(io) | ((((ULONG)(l)) & 0x1fffUL) << 16) | \
     (((ULONG)(g)) << 8) | (ULONG)(n))

#define P_FIONBIO       P_IOC(P_IOC_IN,  'f', 126, 4)
#define P_FIONREAD      P_IOC(P_IOC_OUT, 'f', 127, 4)
#define P_BIOCSETIF     P_IOC(P_IOC_IN,  'B', 108, 32)
#define P_BIOCGDLT      P_IOC(P_IOC_OUT, 'B', 106, 4)

typedef struct ProbeAddr
{
    UBYTE   sin_len;
    UBYTE   sin_family;
    UWORD   sin_port;
    ULONG   sin_addr;
    UBYTE   sin_zero[8];
} ProbeAddr;

typedef struct PIoVec
{
    APTR    iov_base;
    ULONG   iov_len;
} PIoVec;

/* 4.4BSD's struct msghdr, which is what the vector reads. */
typedef struct PMsgHdr
{
    APTR    msg_name;
    ULONG   msg_namelen;
    PIoVec *msg_iov;
    LONG    msg_iovlen;
    APTR    msg_control;
    ULONG   msg_controllen;
    LONG    msg_flags;
} PMsgHdr;

/* struct ifreq is 32 bytes with a 16-byte name; BIOCSETIF wants exactly that
   many, and only the name is read. */
typedef struct PIfReq
{
    char    ifr_name[16];
    UBYTE   ifr_pad[16];
} PIfReq;

/* ------------------------------------------------------------- registers -- */

/*
 * One shape for every call.  d0..d3 and a0..a3 are the whole argument surface
 * the bsdsocket ABI uses, a6 is the base, and the result is always d0
 * whatever the C type says.
 */
typedef struct Regs
{
    ULONG   d0;
    ULONG   d1;
    ULONG   d2;
    ULONG   d3;
    APTR    a0;
    APTR    a1;
    APTR    a2;
    APTR    a3;
} Regs;

typedef ULONG (*ThunkFn)(struct Library *base, const Regs *r);

/* Struct assignment rather than a byte loop, for the reason cycledrill.c
   records: -fanalyzer does not follow zero() through a struct and reports
   every field the callee then reads as uninitialised. */
static const Regs regs_zero;

/*
 * `jsr a6@(-N:W)` needs a literal, so there is one of these per LVO and the
 * table names the one it wants.  Every argument register is declared both as
 * an input and as a clobbered output: the ABI says a vector preserves d2-d7
 * and a2-a6, but this program is here to find out where the library does not
 * do what it says, and a wrong assumption in the caller would be scored
 * against the vector.
 */
#define TH(n)                                                               \
static ULONG t_##n(struct Library *base, const Regs *r) __attribute__((unused)); \
static ULONG t_##n(struct Library *base, const Regs *r)                     \
{                                                                           \
    register struct Library *ra6 __asm("a6") = base;                        \
    register ULONG rd0 __asm("d0") = r->d0;                                 \
    register ULONG rd1 __asm("d1") = r->d1;                                 \
    register ULONG rd2 __asm("d2") = r->d2;                                 \
    register ULONG rd3 __asm("d3") = r->d3;                                 \
    register APTR  ra0 __asm("a0") = r->a0;                                 \
    register APTR  ra1 __asm("a1") = r->a1;                                 \
    register APTR  ra2 __asm("a2") = r->a2;                                 \
    register APTR  ra3 __asm("a3") = r->a3;                                 \
    register ULONG od0 __asm("d0");                                         \
    register LONG  od1 __asm("d1");                                         \
    register LONG  od2 __asm("d2");                                         \
    register LONG  od3 __asm("d3");                                         \
    register LONG  oa0 __asm("a0");                                         \
    register LONG  oa1 __asm("a1");                                         \
    register LONG  oa2 __asm("a2");                                         \
    register LONG  oa3 __asm("a3");                                         \
                                                                            \
    __asm __volatile ("jsr a6@(-" #n ":W)"                                  \
                      : "=r" (od0), "=r" (od1), "=r" (od2), "=r" (od3),     \
                        "=r" (oa0), "=r" (oa1), "=r" (oa2), "=r" (oa3)      \
                      : "r" (ra6), "r" (rd0), "r" (rd1), "r" (rd2),         \
                        "r" (rd3), "r" (ra0), "r" (ra1), "r" (ra2),         \
                        "r" (ra3)                                           \
                      : "cc", "memory");                                    \
    return od0;                                                             \
}

TH(6)   TH(12)  TH(18)  TH(24)  TH(30)  TH(36)  TH(42)  TH(48)  TH(54)  TH(60)
TH(66)  TH(72)  TH(78)  TH(84)  TH(90)  TH(96)  TH(102) TH(108) TH(114) TH(120)
TH(126) TH(132) TH(138) TH(144) TH(150) TH(156) TH(162) TH(168) TH(174) TH(180)
TH(186) TH(192) TH(198) TH(204) TH(210) TH(216) TH(222) TH(228) TH(234) TH(240)
TH(246) TH(252) TH(258) TH(264) TH(270) TH(276) TH(282) TH(288) TH(294) TH(300)
TH(306) TH(312) TH(318) TH(324) TH(330) TH(336) TH(342) TH(348) TH(354) TH(360)
TH(366) TH(372) TH(378) TH(384) TH(390) TH(396) TH(402) TH(408) TH(414) TH(420)
TH(426) TH(432) TH(438) TH(444) TH(450) TH(456) TH(462) TH(468) TH(474) TH(480)
TH(486) TH(492) TH(498) TH(504) TH(510) TH(516) TH(522) TH(528) TH(534) TH(540)
TH(546) TH(552) TH(558) TH(564) TH(570) TH(576) TH(582) TH(588) TH(594) TH(600)
TH(606) TH(612) TH(618) TH(624) TH(630) TH(636) TH(642) TH(648) TH(654) TH(660)
TH(666) TH(672) TH(678) TH(684) TH(690) TH(696) TH(702) TH(708) TH(714) TH(720)
TH(726) TH(732) TH(738) TH(744) TH(750) TH(756) TH(762) TH(768) TH(774) TH(780)
TH(786) TH(792) TH(798) TH(804) TH(810) TH(816) TH(822) TH(828) TH(834) TH(840)
TH(846) TH(852) TH(858) TH(864) TH(870) TH(876) TH(882) TH(888) TH(894) TH(900)

/* ------------------------------------------------------------- reporting -- */

static LONG g_def = DEF_ITERS;      /* ITERS overrides both, keeping the 4:1 */
static LONG g_few = FEW_ITERS;

static LONG checks;
static LONG failures;
static LONG covered;        /* rows actually called                          */
static LONG uncovered;      /* rows with a reason they could not be called   */
static LONG variants_run;

static VOID sayv(const char *fmt, LONG *args)
{
    VPrintf((CONST_STRPTR)fmt, (APTR)args);     /* (APTR): see tool_util.c */
}

static VOID say(const char *fmt, LONG a, LONG b, LONG c, LONG d)
{
    LONG args[4];

    args[0] = a;
    args[1] = b;
    args[2] = c;
    args[3] = d;
    sayv(fmt, args);
}

static VOID flushout(VOID)
{
    Flush(Output());
}

/* Every assertion goes through here, so the count printed at the end is the
   real one and cannot drift away from the FAILs in the transcript. */
static BOOL check(BOOL ok, const char *what, LONG a, LONG b)
{
    checks++;
    if (ok)
        return TRUE;

    failures++;
    say("FAIL: %s (%ld, %ld)\n", (LONG)what, a, b, 0);
    return FALSE;
}

/* -------------------------------------------------------------- sampling -- */

typedef struct Sample
{
    BOOL    ok;             /* the health query answered                     */
    ULONG   free_mem;
    ULONG   sigs;
    ULONG   ports;
    ULONG   sems;
    ULONG   tasks;
    ULONG   alloc_live;
    ULONG   sockets;
    ULONG   pool_free;
    ULONG   pool_bad;
} Sample;

static struct
{
    NetStatusHeader hdr;
    NetStatusHealth e;
} q_health;

static VOID zero(APTR p, ULONG n)
{
    UBYTE *b = (UBYTE *)p;

    while (n-- != 0)
        *b++ = 0;
}

static ULONG own_signals(VOID)
{
    struct Task *me = FindTask(NULL);
    ULONG        mask;
    ULONG        n = 0;

    if (me == NULL)
        return 0;

    for (mask = me->tc_SigAlloc; mask != 0; mask >>= 1)
    {
        if (mask & 1UL)
            n++;
    }

    return n;
}

/* Forbid() rather than Disable(): the lists are only walked. */
static ULONG list_len(struct List *l)
{
    struct Node *n;
    ULONG        total = 0;

    Forbid();
    for (n = l->lh_Head; n->ln_Succ != NULL; n = n->ln_Succ)
        total++;
    Permit();

    return total;
}

static ULONG count_tasks(VOID)
{
    return list_len(&SysBase->TaskReady) + list_len(&SysBase->TaskWait);
}

static LONG p_query(struct Library *base, ULONG what, APTR buffer, ULONG size)
{
    register struct Library *a6  __asm("a6") = base;
    register ULONG           d0  __asm("d0") = AMI_NETSTATUS_MAGIC;
    register ULONG           d1  __asm("d1") = what;
    register APTR            a0  __asm("a0") = buffer;
    register ULONG           d2  __asm("d2") = size;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-870:W)"     /* AMI_NETSTATUS_QUERY_LVO   */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (a0), "r" (d2)
                      : "a1", "cc", "memory");
    return res;
}

_Static_assert(AMI_NETSTATUS_QUERY_LVO   == -870, "NetStackQuery LVO moved");
_Static_assert(AMI_NETSTATUS_CONTROL_LVO == -876, "NetStackControl LVO moved");

static VOID sample(struct Library *base, Sample *out)
{
    static const Sample empty;

    *out = empty;

    out->free_mem = AvailMem(MEMF_ANY);
    out->sigs     = own_signals();
    out->ports    = list_len(&SysBase->PortList);
    out->sems     = list_len(&SysBase->SemaphoreList);
    out->tasks    = count_tasks();

    zero(&q_health, sizeof(q_health));
    q_health.hdr.nsh_Magic   = AMI_NETSTATUS_MAGIC;
    q_health.hdr.nsh_Version = AMI_NETSTATUS_VERSION;

    if (p_query(base, NETSTATUS_HEALTH, &q_health, sizeof(q_health)) <= 0)
        return;

    out->ok         = TRUE;
    out->alloc_live = q_health.e.nsl_AllocLive;
    out->sockets    = q_health.e.nsl_Sockets;
    out->pool_free  = q_health.e.nsl_PoolFree;
    out->pool_bad   = q_health.e.nsl_PoolBadRelease;
}

/* ---------------------------------------------------------------- context - */

typedef struct Ctx
{
    struct Library *base;

    LONG    udp;            /* bound, connected, non-blocking                */
    LONG    tcp;            /* created, never connected                      */
    LONG    dead;           /* a descriptor that has been closed             */
    LONG    bpf;            /* an open bpf channel, or -1                    */

    char    iface[16];
    ULONG   selfaddr;       /* network order, or 0                           */
    ULONG   ifindex;

    struct MsgPort *port;   /* for the AddrAlloc messages                    */

    ULONG   leaked_mem;     /* BROKEN mode bookkeeping                       */
} Ctx;

static Ctx ctx;

/* Scratch the prep functions point the registers at.  File scope so a row's
   arguments outlive the prep call and so the drill's own stack does not move
   between iterations. */
static ProbeAddr    sa_local;
static ProbeAddr    sa_peer;
static ProbeAddr    sa_out;
static LONG         alen;
static LONG         optlen;
static LONG         optval;
static ULONG        evmask;
static ULONG        fdset[8];
static struct timeval tv_zero;
static UBYTE        buf_small[64];
static UBYTE        buf_big[512];
static char         namebuf[256];
static struct TagItem tags[8];
static struct hostent hent;
static UBYTE        hbuf[192];
static LONG         herr;
static APTR         ptr_out;
static PIoVec       iov;
static PMsgHdr      mhdr;
static struct Hook  monhook;
static APTR         aam;
static struct in_addr inaddr;

/* ---------------------------------------------------------- small helpers - */

/* A short-hand for the preps that need to reach the library themselves. */
static LONG do_socket(LONG dom, LONG type, LONG proto)
{
    Regs r;

    r = regs_zero;
    r.d0 = (ULONG)dom;
    r.d1 = (ULONG)type;
    r.d2 = (ULONG)proto;
    return (LONG)t_30(ctx.base, &r);
}

static LONG do_close(LONG s)
{
    Regs r;

    r = regs_zero;
    r.d0 = (ULONG)s;
    return (LONG)t_120(ctx.base, &r);
}

static LONG do_ioctl(LONG s, ULONG req, APTR arg)
{
    Regs r;

    r = regs_zero;
    r.d0 = (ULONG)s;
    r.d1 = req;
    r.a0 = arg;
    return (LONG)t_114(ctx.base, &r);
}

static LONG do_release(LONG s, LONG id)
{
    Regs r;

    r = regs_zero;
    r.d0 = (ULONG)s;
    r.d1 = (ULONG)id;
    return (LONG)t_150(ctx.base, &r);
}

static LONG do_obtain(LONG id, LONG dom, LONG type, LONG proto)
{
    Regs r;

    r = regs_zero;
    r.d0 = (ULONG)id;
    r.d1 = (ULONG)dom;
    r.d2 = (ULONG)type;
    r.d3 = (ULONG)proto;
    return (LONG)t_144(ctx.base, &r);
}

static APTR do_getrouteinfo(LONG af, LONG flags)
{
    Regs r;

    r = regs_zero;
    r.d0 = (ULONG)af;
    r.d1 = (ULONG)flags;
    return (APTR)t_438(ctx.base, &r);
}

static VOID do_freerouteinfo(APTR p)
{
    Regs r;

    r = regs_zero;
    r.a0 = p;
    (VOID)t_432(ctx.base, &r);
}

static APTR do_obtainiflist(VOID)
{
    Regs r;

    r = regs_zero;
    return (APTR)t_462(ctx.base, &r);
}

static VOID do_releaseiflist(APTR p)
{
    Regs r;

    r = regs_zero;
    r.a0 = p;
    (VOID)t_456(ctx.base, &r);
}

static APTR do_obtaindnslist(VOID)
{
    Regs r;

    r = regs_zero;
    return (APTR)t_534(ctx.base, &r);
}

static VOID do_releasednslist(APTR p)
{
    Regs r;

    r = regs_zero;
    r.a0 = p;
    (VOID)t_528(ctx.base, &r);
}

static LONG do_adddns(const char *addr)
{
    Regs r;

    r = regs_zero;
    r.a0 = (APTR)addr;
    return (LONG)t_516(ctx.base, &r);
}

static LONG do_removedns(const char *addr)
{
    Regs r;

    r = regs_zero;
    r.a0 = (APTR)addr;
    return (LONG)t_522(ctx.base, &r);
}

static LONG do_addroute(VOID)
{
    Regs r;

    tags[0].ti_Tag  = RTA_Destination;
    tags[0].ti_Data = (ULONG)"192.0.2.0";
    tags[1].ti_Tag  = RTA_Gateway;
    tags[1].ti_Data = (ULONG)"10.0.2.2";
    tags[2].ti_Tag  = TAG_DONE;
    tags[2].ti_Data = 0;

    r = regs_zero;
    r.a0 = (APTR)tags;
    return (LONG)t_414(ctx.base, &r);
}

static LONG do_delroute(VOID)
{
    Regs r;

    tags[4].ti_Tag  = RTA_Destination;
    tags[4].ti_Data = (ULONG)"192.0.2.0";
    tags[5].ti_Tag  = TAG_DONE;
    tags[5].ti_Data = 0;

    r = regs_zero;
    r.a0 = (APTR)&tags[4];
    return (LONG)t_420(ctx.base, &r);
}

static LONG do_createaam(LONG version, APTR *out)
{
    Regs r;
    struct TagItem t[2];

    t[0].ti_Tag  = CAAMTA_ReplyPort;
    t[0].ti_Data = (ULONG)ctx.port;
    t[1].ti_Tag  = TAG_DONE;
    t[1].ti_Data = 0;

    *out = NULL;

    r = regs_zero;
    r.d0 = (ULONG)version;
    r.d1 = (ULONG)AAMP_BOOTP;
    r.a0 = (APTR)ctx.iface;
    r.a1 = (APTR)out;
    r.a2 = (APTR)t;
    return (LONG)t_474(ctx.base, &r);
}

static VOID do_deleteaam(APTR m)
{
    Regs r;

    r = regs_zero;
    r.a0 = m;
    (VOID)t_480(ctx.base, &r);
}

static LONG do_addmonhook(VOID)
{
    Regs r;

    r = regs_zero;
    r.d0 = MHT_Bind;
    r.a0 = (APTR)&monhook;
    r.a1 = NULL;
    return (LONG)t_498(ctx.base, &r);
}

static VOID do_removemonhook(VOID)
{
    Regs r;

    r = regs_zero;
    r.a0 = (APTR)&monhook;
    (VOID)t_504(ctx.base, &r);
}

static LONG do_getaddrinfo(const char *host, APTR *out)
{
    Regs r;
    static struct addrinfo hints;

    zero(&hints, sizeof(hints));
    hints.ai_family   = P_AF_INET;
    hints.ai_socktype = P_SOCK_DGRAM;
    hints.ai_flags    = AI_NUMERICHOST;

    *out = NULL;

    r = regs_zero;
    r.a0 = (APTR)host;
    r.a1 = NULL;
    r.a2 = (APTR)&hints;
    r.a3 = (APTR)out;
    return (LONG)t_810(ctx.base, &r);
}

static VOID do_freeaddrinfo(APTR p)
{
    Regs r;

    r = regs_zero;
    r.a0 = p;
    (VOID)t_804(ctx.base, &r);
}

static APTR do_nameindex(VOID)
{
    Regs r;

    r = regs_zero;
    return (APTR)t_894(ctx.base, &r);
}

static VOID do_freenameindex(APTR p)
{
    Regs r;

    r = regs_zero;
    r.a0 = p;
    (VOID)t_900(ctx.base, &r);
}

static LONG do_bpfopen(LONG chan)
{
    Regs r;

    r = regs_zero;
    r.d0 = (ULONG)chan;
    return (LONG)t_366(ctx.base, &r);
}

static LONG do_bpfclose(LONG chan)
{
    Regs r;

    r = regs_zero;
    r.d0 = (ULONG)chan;
    return (LONG)t_372(ctx.base, &r);
}

/* ------------------------------------------------------------- the preps -- */

#define PREP_OK 1
#define PREP_NA 0

typedef LONG (*PrepFn)(LONG variant, Regs *r);
typedef VOID (*PostFn)(LONG variant, ULONG res);

static VOID fill_peer(VOID)
{
    zero(&sa_peer, sizeof(sa_peer));
    sa_peer.sin_len    = (UBYTE)sizeof(sa_peer);
    sa_peer.sin_family = P_AF_INET;
    sa_peer.sin_port   = 9;                     /* discard, network order */
    sa_peer.sin_addr   = 0x0A000202UL;          /* 10.0.2.2 */
}

static VOID fill_local(VOID)
{
    zero(&sa_local, sizeof(sa_local));
    sa_local.sin_len    = (UBYTE)sizeof(sa_local);
    sa_local.sin_family = P_AF_INET;
    sa_local.sin_port   = 0;
    sa_local.sin_addr   = 0;
}

/* socket(), and the descriptor is closed again in the post. */
static LONG pr_socket(LONG v, Regs *r)
{
    if (v == 0)
    {
        r->d0 = P_AF_INET;
        r->d1 = P_SOCK_DGRAM;
        r->d2 = 0;
    }
    else
    {
        r->d0 = 99;                             /* no such family */
        r->d1 = 99;
        r->d2 = 99;
    }
    return PREP_OK;
}

static VOID po_socket(LONG v, ULONG res)
{
    (VOID)v;
    if ((LONG)res >= 0)
        (VOID)do_close((LONG)res);
}

/* A socket made in the prep and closed in the post, so the row measures the
   whole bracket and a leak in it cannot hide behind a descriptor left open. */
static LONG scratch_fd = -1;

static LONG pr_fresh_udp(LONG v, Regs *r)
{
    (VOID)v;
    scratch_fd = do_socket(P_AF_INET, P_SOCK_DGRAM, 0);
    r->d0 = (ULONG)scratch_fd;
    return (scratch_fd >= 0) ? PREP_OK : PREP_NA;
}

static VOID po_fresh(LONG v, ULONG res)
{
    (VOID)v;
    (VOID)res;
    if (scratch_fd >= 0)
    {
        (VOID)do_close(scratch_fd);
        scratch_fd = -1;
    }
}

static LONG pr_bind(LONG v, Regs *r)
{
    fill_local();
    if (v == 0)
    {
        if (pr_fresh_udp(0, r) == PREP_NA)
            return PREP_NA;
    }
    else
    {
        r->d0 = (ULONG)ctx.dead;
    }
    r->a0 = (APTR)&sa_local;
    r->d1 = (ULONG)sizeof(sa_local);
    return PREP_OK;
}

static VOID po_bind(LONG v, ULONG res)
{
    if (v == 0)
        po_fresh(v, res);
}

static LONG pr_listen(LONG v, Regs *r)
{
    if (v == 0)
    {
        scratch_fd = do_socket(P_AF_INET, P_SOCK_STREAM, 0);
        if (scratch_fd < 0)
            return PREP_NA;
        r->d0 = (ULONG)scratch_fd;
    }
    else
    {
        r->d0 = (ULONG)ctx.dead;
    }
    r->d1 = 5;
    return PREP_OK;
}

static VOID po_listen(LONG v, ULONG res)
{
    if (v == 0)
        po_fresh(v, res);
}

/* accept() has no success path without a peer; both variants refuse. */
static LONG pr_accept(LONG v, Regs *r)
{
    alen  = (LONG)sizeof(sa_out);
    r->d0 = (v == 0) ? (ULONG)ctx.tcp : (ULONG)ctx.dead;
    r->a0 = (APTR)&sa_out;
    r->a1 = (APTR)&alen;
    return PREP_OK;
}

static VOID po_accept(LONG v, ULONG res)
{
    (VOID)v;
    if ((LONG)res >= 0)
        (VOID)do_close((LONG)res);
}

static LONG pr_connect(LONG v, Regs *r)
{
    fill_peer();
    if (v == 0)
    {
        if (pr_fresh_udp(0, r) == PREP_NA)
            return PREP_NA;
    }
    else
    {
        r->d0 = (ULONG)ctx.dead;
    }
    r->a0 = (APTR)&sa_peer;
    r->d1 = (ULONG)sizeof(sa_peer);
    return PREP_OK;
}

static VOID po_connect(LONG v, ULONG res)
{
    if (v == 0)
        po_fresh(v, res);
}

static LONG pr_sendto(LONG v, Regs *r)
{
    fill_peer();
    r->d0 = (v == 0) ? (ULONG)ctx.udp : (ULONG)ctx.dead;
    r->a0 = (APTR)buf_small;
    r->d1 = 8;
    r->d2 = 0;
    r->a1 = (APTR)&sa_peer;
    r->d3 = (ULONG)sizeof(sa_peer);
    return PREP_OK;
}

static LONG pr_send(LONG v, Regs *r)
{
    r->d0 = (v == 0) ? (ULONG)ctx.udp : (ULONG)ctx.dead;
    r->a0 = (APTR)buf_small;
    r->d1 = 8;
    r->d2 = 0;
    return PREP_OK;
}

static LONG pr_recvfrom(LONG v, Regs *r)
{
    alen  = (LONG)sizeof(sa_out);
    r->d0 = (v == 0) ? (ULONG)ctx.udp : (ULONG)ctx.dead;
    r->a0 = (APTR)buf_big;
    r->d1 = (ULONG)sizeof(buf_big);
    r->d2 = 0;
    r->a1 = (APTR)&sa_out;
    r->a2 = (APTR)&alen;
    return PREP_OK;
}

static LONG pr_recv(LONG v, Regs *r)
{
    r->d0 = (v == 0) ? (ULONG)ctx.udp : (ULONG)ctx.dead;
    r->a0 = (APTR)buf_big;
    r->d1 = (ULONG)sizeof(buf_big);
    r->d2 = 0;
    return PREP_OK;
}

static LONG pr_shutdown(LONG v, Regs *r)
{
    if (v == 0)
    {
        scratch_fd = do_socket(P_AF_INET, P_SOCK_STREAM, 0);
        if (scratch_fd < 0)
            return PREP_NA;
        r->d0 = (ULONG)scratch_fd;
    }
    else
    {
        r->d0 = (ULONG)ctx.dead;
    }
    r->d1 = 2;
    return PREP_OK;
}

static VOID po_shutdown(LONG v, ULONG res)
{
    if (v == 0)
        po_fresh(v, res);
}

static LONG pr_setsockopt(LONG v, Regs *r)
{
    optval = 1;
    r->d0  = (ULONG)ctx.udp;
    r->d1  = (v == 0) ? (ULONG)P_SOL_SOCKET : 0x7ffful;
    r->d2  = P_SO_REUSEADDR;
    r->a0  = (APTR)&optval;
    r->d3  = (ULONG)sizeof(optval);
    return PREP_OK;
}

static LONG pr_getsockopt(LONG v, Regs *r)
{
    optlen = (LONG)sizeof(optval);
    r->d0  = (ULONG)ctx.udp;
    r->d1  = P_SOL_SOCKET;
    r->d2  = (v == 0) ? (ULONG)P_SO_TYPE : 0x7ffful;
    r->a0  = (APTR)&optval;
    r->a1  = (APTR)&optlen;
    return PREP_OK;
}

static LONG pr_getsockname(LONG v, Regs *r)
{
    alen  = (LONG)sizeof(sa_out);
    r->d0 = (v == 0) ? (ULONG)ctx.udp : (ULONG)ctx.dead;
    r->a0 = (APTR)&sa_out;
    r->a1 = (APTR)&alen;
    return PREP_OK;
}

static LONG pr_getpeername(LONG v, Regs *r)
{
    alen  = (LONG)sizeof(sa_out);
    r->d0 = (v == 0) ? (ULONG)ctx.udp : (ULONG)ctx.tcp;   /* v1: not connected */
    r->a0 = (APTR)&sa_out;
    r->a1 = (APTR)&alen;
    return PREP_OK;
}

static LONG pr_ioctl(LONG v, Regs *r)
{
    optval = 1;
    r->d0  = (ULONG)ctx.udp;
    r->d1  = (v == 0) ? P_FIONREAD : 0xdeadbeefUL;
    r->a0  = (APTR)&optval;
    return PREP_OK;
}

static LONG pr_close(LONG v, Regs *r)
{
    if (v == 0)
    {
        scratch_fd = do_socket(P_AF_INET, P_SOCK_DGRAM, 0);
        if (scratch_fd < 0)
            return PREP_NA;
        r->d0 = (ULONG)scratch_fd;
        scratch_fd = -1;            /* the call under test does the closing */
    }
    else
    {
        r->d0 = (ULONG)ctx.dead;
    }
    return PREP_OK;
}

static LONG pr_waitselect(LONG v, Regs *r)
{
    zero(fdset, sizeof(fdset));
    fdset[0] = 0;
    tv_zero.tv_secs  = 0;
    tv_zero.tv_micro = 0;

    r->d0 = (v == 0) ? (ULONG)(ctx.udp + 1) : (ULONG)-1;
    r->a0 = (APTR)fdset;
    r->a1 = NULL;
    r->a2 = NULL;
    r->a3 = (APTR)&tv_zero;
    r->d1 = 0;
    return PREP_OK;
}

static LONG pr_setsocketsignals(LONG v, Regs *r)
{
    (VOID)v;
    r->d0 = 0;
    r->d1 = 0;
    r->d2 = 0;
    return PREP_OK;
}

static LONG pr_none(LONG v, Regs *r)
{
    (VOID)v;
    (VOID)r;
    return PREP_OK;
}

/* ReleaseSocket parks it, ObtainSocket takes it back, and the descriptor is
   closed again: a triple whose net effect on the machine must be nothing. */
static LONG pr_obtainsocket(LONG v, Regs *r)
{
    LONG id;

    if (v != 0)
    {
        r->d0 = 0x7f000000;                     /* no such id */
        r->d1 = P_AF_INET;
        r->d2 = P_SOCK_DGRAM;
        r->d3 = 0;
        return PREP_OK;
    }

    scratch_fd = do_socket(P_AF_INET, P_SOCK_DGRAM, 0);
    if (scratch_fd < 0)
        return PREP_NA;

    id = do_release(scratch_fd, -1);
    scratch_fd = -1;
    if (id < 0)
        return PREP_NA;

    r->d0 = (ULONG)id;
    r->d1 = P_AF_INET;
    r->d2 = P_SOCK_DGRAM;
    r->d3 = 0;
    return PREP_OK;
}

static VOID po_obtainsocket(LONG v, ULONG res)
{
    (VOID)v;
    if ((LONG)res >= 0)
        (VOID)do_close((LONG)res);
}

static LONG pr_releasesocket(LONG v, Regs *r)
{
    if (v != 0)
    {
        r->d0 = (ULONG)ctx.dead;
        r->d1 = (ULONG)-1;
        return PREP_OK;
    }

    scratch_fd = do_socket(P_AF_INET, P_SOCK_DGRAM, 0);
    if (scratch_fd < 0)
        return PREP_NA;

    r->d0 = (ULONG)scratch_fd;
    r->d1 = (ULONG)-1;
    scratch_fd = -1;
    return PREP_OK;
}

static VOID po_releasesocket(LONG v, ULONG res)
{
    LONG fd;

    if (v != 0 || (LONG)res < 0)
        return;

    fd = do_obtain((LONG)res, P_AF_INET, P_SOCK_DGRAM, 0);
    if (fd >= 0)
        (VOID)do_close(fd);
}

static LONG pr_releasecopy(LONG v, Regs *r)
{
    if (v != 0)
    {
        r->d0 = (ULONG)ctx.dead;
        r->d1 = (ULONG)-1;
        return PREP_OK;
    }

    scratch_fd = do_socket(P_AF_INET, P_SOCK_DGRAM, 0);
    if (scratch_fd < 0)
        return PREP_NA;

    r->d0 = (ULONG)scratch_fd;
    r->d1 = (ULONG)-1;
    return PREP_OK;
}

/* The copy leaves both the caller's descriptor and the parked reference, so
   both have to go: obtain the copy, close it, close the original. */
static VOID po_releasecopy(LONG v, ULONG res)
{
    LONG fd;

    if (v != 0)
        return;

    if ((LONG)res >= 0)
    {
        fd = do_obtain((LONG)res, P_AF_INET, P_SOCK_DGRAM, 0);
        if (fd >= 0)
            (VOID)do_close(fd);
    }
    if (scratch_fd >= 0)
    {
        (VOID)do_close(scratch_fd);
        scratch_fd = -1;
    }
}

static LONG pr_seterrnoptr(LONG v, Regs *r)
{
    r->a0 = (APTR)&optval;
    r->d0 = (v == 0) ? 4 : 3;       /* 3 is not a legal width: it clears */
    return PREP_OK;
}

/* Whatever the variant did, put the base back on its own errno. */
static VOID po_seterrnoptr(LONG v, ULONG res)
{
    Regs r;

    (VOID)v;
    (VOID)res;
    r = regs_zero;
    r.a0 = NULL;
    r.d0 = 4;
    (VOID)t_168(ctx.base, &r);
}

static LONG pr_inet_ntoa(LONG v, Regs *r)
{
    (VOID)v;
    r->d0 = 0x0A000202UL;
    return PREP_OK;
}

static LONG pr_inet_addr(LONG v, Regs *r)
{
    r->a0 = (v == 0) ? (APTR)"10.0.2.15" : (APTR)"not.an.address";
    return PREP_OK;
}

static LONG pr_lnaof(LONG v, Regs *r)
{
    r->d0 = (v == 0) ? 0x0A000202UL : 0;
    return PREP_OK;
}

static LONG pr_makeaddr(LONG v, Regs *r)
{
    r->d0 = (v == 0) ? 10 : 0;
    r->d1 = (v == 0) ? 0x000202UL : 0;
    return PREP_OK;
}

static LONG pr_gethostbyname(LONG v, Regs *r)
{
    r->a0 = (v == 0) ? (APTR)"127.0.0.1" : (APTR)"no.such.host.invalid";
    return PREP_OK;
}

static LONG pr_gethostbyaddr(LONG v, Regs *r)
{
    buf_small[0] = 127;
    buf_small[1] = 0;
    buf_small[2] = 0;
    buf_small[3] = 1;
    r->a0 = (APTR)buf_small;
    r->d0 = 4;
    r->d1 = (v == 0) ? P_AF_INET : 99;
    return PREP_OK;
}

static LONG pr_getnetbyname(LONG v, Regs *r)
{
    r->a0 = (v == 0) ? (APTR)"loopback" : (APTR)"no-such-network";
    return PREP_OK;
}

static LONG pr_getnetbyaddr(LONG v, Regs *r)
{
    r->d0 = 127;
    r->d1 = (v == 0) ? P_AF_INET : 99;
    return PREP_OK;
}

static LONG pr_getservbyname(LONG v, Regs *r)
{
    r->a0 = (v == 0) ? (APTR)"telnet" : (APTR)"no-such-service";
    r->a1 = (APTR)"tcp";
    return PREP_OK;
}

static LONG pr_getservbyport(LONG v, Regs *r)
{
    r->d0 = (v == 0) ? 23 : 64000;              /* network order == host on m68k */
    r->a0 = (APTR)"tcp";
    return PREP_OK;
}

static LONG pr_getprotobyname(LONG v, Regs *r)
{
    r->a0 = (v == 0) ? (APTR)"tcp" : (APTR)"no-such-protocol";
    return PREP_OK;
}

static LONG pr_getprotobynumber(LONG v, Regs *r)
{
    r->d0 = (v == 0) ? (ULONG)P_IPPROTO_TCP : 250;
    return PREP_OK;
}

static LONG pr_dup2(LONG v, Regs *r)
{
    r->d0 = (v == 0) ? (ULONG)ctx.udp : (ULONG)ctx.dead;
    r->d1 = (ULONG)-1;                          /* any free descriptor */
    return PREP_OK;
}

static VOID po_dup2(LONG v, ULONG res)
{
    (VOID)v;
    if ((LONG)res >= 0)
        (VOID)do_close((LONG)res);
}

static VOID fill_msg(VOID)
{
    fill_peer();
    iov.iov_base = (APTR)buf_small;
    iov.iov_len  = 8;

    zero(&mhdr, sizeof(mhdr));
    mhdr.msg_name    = (APTR)&sa_peer;
    mhdr.msg_namelen = (ULONG)sizeof(sa_peer);
    mhdr.msg_iov     = &iov;
    mhdr.msg_iovlen  = 1;
}

static LONG pr_sendmsg(LONG v, Regs *r)
{
    fill_msg();
    r->d0 = (v == 0) ? (ULONG)ctx.udp : (ULONG)ctx.dead;
    r->a0 = (APTR)&mhdr;
    r->d1 = 0;
    return PREP_OK;
}

static LONG pr_recvmsg(LONG v, Regs *r)
{
    fill_msg();
    iov.iov_base = (APTR)buf_big;
    iov.iov_len  = (ULONG)sizeof(buf_big);
    mhdr.msg_name    = (APTR)&sa_out;
    mhdr.msg_namelen = (ULONG)sizeof(sa_out);
    r->d0 = (v == 0) ? (ULONG)ctx.udp : (ULONG)ctx.dead;
    r->a0 = (APTR)&mhdr;
    r->d1 = 0;
    return PREP_OK;
}

static LONG pr_gethostname(LONG v, Regs *r)
{
    r->a0 = (v == 0) ? (APTR)namebuf : NULL;
    r->d0 = (ULONG)sizeof(namebuf);
    return PREP_OK;
}

/*
 * Variant 0 is a list every tag of which is serviced.  Variant 1 puts a tag
 * that is refused in the MIDDLE: SBTC_HAVE_KERNEL_MEMORY_API is query-only, so
 * the SET is refused, the call returns that tag's 1-based index and everything
 * after it is discarded (errno.c).  That discard is where a half-applied
 * allocation would hide, which is the whole reason this variant exists.
 */
static LONG pr_sbtaglist(LONG v, Regs *r)
{
    if (v == 0)
    {
        tags[0].ti_Tag  = SBTM_GETVAL(SBTC_DTABLESIZE);
        tags[0].ti_Data = 0;
        tags[1].ti_Tag  = SBTM_GETVAL(SBTC_ERRNO);
        tags[1].ti_Data = 0;
        tags[2].ti_Tag  = TAG_DONE;
        tags[2].ti_Data = 0;
    }
    else
    {
        tags[0].ti_Tag  = SBTM_GETVAL(SBTC_DTABLESIZE);
        tags[0].ti_Data = 0;
        tags[1].ti_Tag  = SBTM_SETVAL(SBTC_HAVE_KERNEL_MEMORY_API);
        tags[1].ti_Data = 1;
        tags[2].ti_Tag  = SBTM_GETVAL(SBTC_ERRNO);
        tags[2].ti_Data = 0;
        tags[3].ti_Tag  = TAG_DONE;
        tags[3].ti_Data = 0;
    }
    r->a0 = (APTR)tags;
    return PREP_OK;
}

static LONG pr_getsocketevents(LONG v, Regs *r)
{
    evmask = 0;
    r->a0 = (v == 0) ? (APTR)&evmask : NULL;
    return PREP_OK;
}

static LONG pr_bpf_open(LONG v, Regs *r)
{
    r->d0 = (v == 0) ? (ULONG)-1 : 99;          /* -1 = any free channel */
    return PREP_OK;
}

static VOID po_bpf_open(LONG v, ULONG res)
{
    (VOID)v;
    if ((LONG)res >= 0)
        (VOID)do_bpfclose((LONG)res);
}

static LONG bpf_scratch = -1;

static LONG pr_bpf_close(LONG v, Regs *r)
{
    if (v == 0)
    {
        bpf_scratch = do_bpfopen(-1);
        if (bpf_scratch < 0)
            return PREP_NA;
        r->d0 = (ULONG)bpf_scratch;
        bpf_scratch = -1;
    }
    else
    {
        r->d0 = 3;                              /* never opened */
    }
    return PREP_OK;
}

/* The rest of the bpf family runs against a channel held open for the whole
   drill, so what is measured is the call and not the open/close pair. */
static LONG pr_bpf_chan(LONG v, Regs *r)
{
    if (v == 0)
    {
        if (ctx.bpf < 0)
            return PREP_NA;
        r->d0 = (ULONG)ctx.bpf;
    }
    else
    {
        r->d0 = 3;
    }
    return PREP_OK;
}

static LONG pr_bpf_read(LONG v, Regs *r)
{
    if (pr_bpf_chan(v, r) == PREP_NA)
        return PREP_NA;
    r->a0 = (APTR)buf_big;
    r->d1 = (ULONG)sizeof(buf_big);
    return PREP_OK;
}

static LONG pr_bpf_write(LONG v, Regs *r)
{
    if (pr_bpf_chan(v, r) == PREP_NA)
        return PREP_NA;
    zero(buf_big, 64);
    buf_big[0]  = 0xff; buf_big[1]  = 0xff; buf_big[2] = 0xff;
    buf_big[3]  = 0xff; buf_big[4]  = 0xff; buf_big[5] = 0xff;
    buf_big[12] = 0x88; buf_big[13] = 0xb5;     /* local experimental */
    r->a0 = (APTR)buf_big;
    r->d1 = 60;
    return PREP_OK;
}

/* set_notify_mask is the odd one out: channel in d1, mask in d0. */
static LONG pr_bpf_notify(LONG v, Regs *r)
{
    if (v == 0)
    {
        if (ctx.bpf < 0)
            return PREP_NA;
        r->d1 = (ULONG)ctx.bpf;
    }
    else
    {
        r->d1 = 3;
    }
    r->d0 = 0;
    return PREP_OK;
}

static LONG pr_bpf_interrupt(LONG v, Regs *r)
{
    if (pr_bpf_chan(v, r) == PREP_NA)
        return PREP_NA;
    r->d1 = 0;
    return PREP_OK;
}

static LONG pr_bpf_ioctl(LONG v, Regs *r)
{
    if (pr_bpf_chan(v, r) == PREP_NA)
        return PREP_NA;
    optval = 0;
    r->d1  = P_BIOCGDLT;
    r->a0  = (APTR)&optval;
    return PREP_OK;
}

static LONG pr_addroute(LONG v, Regs *r)
{
    if (v == 0)
    {
        tags[0].ti_Tag  = RTA_Destination;
        tags[0].ti_Data = (ULONG)"192.0.2.0";
        tags[1].ti_Tag  = RTA_Gateway;
        tags[1].ti_Data = (ULONG)"10.0.2.2";
        tags[2].ti_Tag  = TAG_DONE;
        tags[2].ti_Data = 0;
        r->a0 = (APTR)tags;
    }
    else
    {
        r->a0 = NULL;
    }
    return PREP_OK;
}

static VOID po_addroute(LONG v, ULONG res)
{
    (VOID)res;
    if (v == 0)
        (VOID)do_delroute();
}

static LONG pr_delroute(LONG v, Regs *r)
{
    if (v == 0)
    {
        (VOID)do_addroute();
        tags[4].ti_Tag  = RTA_Destination;
        tags[4].ti_Data = (ULONG)"192.0.2.0";
        tags[5].ti_Tag  = TAG_DONE;
        tags[5].ti_Data = 0;
        r->a0 = (APTR)&tags[4];
    }
    else
    {
        r->a0 = NULL;
    }
    return PREP_OK;
}

static LONG pr_freerouteinfo(LONG v, Regs *r)
{
    if (v == 0)
    {
        ptr_out = do_getrouteinfo(P_AF_UNSPEC, 0);
        if (ptr_out == NULL)
            return PREP_NA;
        r->a0 = ptr_out;
    }
    else
    {
        r->a0 = NULL;                           /* documented no-op */
    }
    return PREP_OK;
}

static LONG pr_getrouteinfo(LONG v, Regs *r)
{
    r->d0 = (v == 0) ? (ULONG)P_AF_UNSPEC : (ULONG)P_AF_INET6;
    r->d1 = 0;
    return PREP_OK;
}

static VOID po_getrouteinfo(LONG v, ULONG res)
{
    (VOID)v;
    if (res != 0)
        do_freerouteinfo((APTR)res);
}

/*
 * AddInterfaceTagList has no success variant here.  A success adds a second
 * interface to the machine and changes what every later row sees, and the
 * cost of a FAILED add is what run-addifleak.sh already watches.  Both
 * variants are refusals, on the two different paths: an unknown device, and a
 * name that is already taken.
 */
static LONG pr_addiface(LONG v, Regs *r)
{
    r->a0 = (v == 0) ? (APTR)"drl0" : (APTR)ctx.iface;
    r->a1 = (APTR)"nosuch.device";
    r->d0 = 0;
    r->a2 = NULL;
    return PREP_OK;
}

static LONG pr_configiface(LONG v, Regs *r)
{
    r->a0 = (v == 0) ? (APTR)ctx.iface : (APTR)"nosuchif";
    r->a1 = NULL;                               /* legal no-op tag list */
    return PREP_OK;
}

static LONG pr_releaseiflist(LONG v, Regs *r)
{
    if (v == 0)
    {
        ptr_out = do_obtainiflist();
        if (ptr_out == NULL)
            return PREP_NA;
        r->a0 = ptr_out;
    }
    else
    {
        r->a0 = NULL;
    }
    return PREP_OK;
}

static VOID po_obtainiflist(LONG v, ULONG res)
{
    (VOID)v;
    if (res != 0)
        do_releaseiflist((APTR)res);
}

static LONG pr_queryiface(LONG v, Regs *r)
{
    tags[0].ti_Tag  = IFQ_State;
    tags[0].ti_Data = (ULONG)&optval;
    tags[1].ti_Tag  = TAG_DONE;
    tags[1].ti_Data = 0;
    r->a0 = (v == 0) ? (APTR)ctx.iface : (APTR)"nosuchif";
    r->a1 = (APTR)tags;
    return PREP_OK;
}

static LONG pr_createaam(LONG v, Regs *r)
{
    static struct TagItem t[2];

    if (ctx.port == NULL)
        return PREP_NA;

    t[0].ti_Tag  = CAAMTA_ReplyPort;
    t[0].ti_Data = (ULONG)ctx.port;
    t[1].ti_Tag  = TAG_DONE;
    t[1].ti_Data = 0;

    ptr_out = NULL;
    r->d0 = (v == 0) ? (ULONG)AAM_VERSION : 99;
    r->d1 = (ULONG)AAMP_BOOTP;
    r->a0 = (APTR)ctx.iface;
    r->a1 = (APTR)&ptr_out;
    r->a2 = (APTR)t;
    return PREP_OK;
}

static VOID po_createaam(LONG v, ULONG res)
{
    (VOID)v;
    (VOID)res;
    if (ptr_out != NULL)
    {
        do_deleteaam(ptr_out);
        ptr_out = NULL;
    }
}

static LONG pr_deleteaam(LONG v, Regs *r)
{
    if (v == 0)
    {
        if (ctx.port == NULL)
            return PREP_NA;
        if (do_createaam(AAM_VERSION, &aam) != CAAME_Success || aam == NULL)
            return PREP_NA;
        r->a0 = aam;
        aam = NULL;
    }
    else
    {
        r->a0 = NULL;                           /* documented no-op */
    }
    return PREP_OK;
}

/*
 * BeginInterfaceConfig on an interface that already has an address is replied
 * inside the call, so this needs no worker and no waiting: the message comes
 * straight back on the port.  A drill that used the DHCP path would be
 * measuring SLIRP.
 */
static LONG pr_beginconfig(LONG v, Regs *r)
{
    if (v != 0)
    {
        r->a0 = NULL;
        return PREP_OK;
    }

    if (ctx.port == NULL)
        return PREP_NA;
    if (do_createaam(AAM_VERSION, &aam) != CAAME_Success || aam == NULL)
        return PREP_NA;
    r->a0 = aam;
    return PREP_OK;
}

static VOID po_beginconfig(LONG v, ULONG res)
{
    struct Message *m;

    (VOID)res;
    if (v != 0 || aam == NULL)
        return;

    /* The refusal was replied inside the call, so the message is on the port
       already; GetMsg rather than WaitPort, because a WaitPort here would
       hang the drill if it were not. */
    m = GetMsg(ctx.port);
    (VOID)m;
    do_deleteaam(aam);
    aam = NULL;
}

static LONG pr_abortconfig(LONG v, Regs *r)
{
    if (v != 0)
    {
        r->a0 = NULL;
        return PREP_OK;
    }

    if (ctx.port == NULL)
        return PREP_NA;
    if (do_createaam(AAM_VERSION, &aam) != CAAME_Success || aam == NULL)
        return PREP_NA;
    r->a0 = aam;
    return PREP_OK;
}

static VOID po_abortconfig(LONG v, ULONG res)
{
    (VOID)res;
    if (v != 0 || aam == NULL)
        return;

    while (GetMsg(ctx.port) != NULL)
        ;
    do_deleteaam(aam);
    aam = NULL;
}

/* The hook register convention is Hook in a0, NULL in a2, message in a1; see
   tests/tools/monprobe.c, which is where the pair a2/a1 is asserted. */
typedef LONG (*DrillHookFn)(register struct Hook *hook __asm("a0"),
                            register APTR reserved __asm("a2"),
                            register APTR message __asm("a1"));

/* h_Entry is `ULONG (*)()`, no parameters, because a Hook carries whatever
   shape the two ends agreed on.  The union says that without claiming the two
   function types are compatible, which a cast cannot. */
typedef union DrillEntry
{
    ULONG       (*de_Raw)(VOID);
    DrillHookFn   de_Fn;
} DrillEntry;

static LONG mon_entry(register struct Hook *h __asm("a0"),
                      register APTR reserved  __asm("a2"),
                      register APTR msg       __asm("a1"))
{
    (VOID)h;
    (VOID)reserved;
    (VOID)msg;
    return 0;                                   /* allow */
}

static VOID fill_hook(VOID)
{
    DrillEntry entry;

    entry.de_Fn = mon_entry;

    zero(&monhook, sizeof(monhook));
    monhook.h_Entry    = entry.de_Raw;
    monhook.h_SubEntry = NULL;
    monhook.h_Data     = NULL;
}

static LONG pr_addmonhook(LONG v, Regs *r)
{
    fill_hook();
    r->d0 = MHT_Bind;
    r->a0 = (v == 0) ? (APTR)&monhook : NULL;
    r->a1 = NULL;
    return PREP_OK;
}

static VOID po_addmonhook(LONG v, ULONG res)
{
    (VOID)v;
    if ((LONG)res == 0)
        do_removemonhook();
}

static LONG pr_removemonhook(LONG v, Regs *r)
{
    if (v == 0)
    {
        fill_hook();
        if (do_addmonhook() != 0)
            return PREP_NA;
        r->a0 = (APTR)&monhook;
    }
    else
    {
        r->a0 = NULL;                           /* documented no-op */
    }
    return PREP_OK;
}

static LONG pr_netstats(LONG v, Regs *r)
{
    r->d0 = NETSTATUS_ip;
    r->d1 = (v == 0) ? (ULONG)NETWORKSTATUS_VERSION : 0;
    r->a0 = (APTR)buf_big;
    r->d2 = (ULONG)sizeof(buf_big);
    return PREP_OK;
}

static LONG pr_adddns(LONG v, Regs *r)
{
    r->a0 = (v == 0) ? (APTR)"192.0.2.53" : (APTR)"not.an.address";
    return PREP_OK;
}

static VOID po_adddns(LONG v, ULONG res)
{
    if (v == 0 && (LONG)res == 0)
        (VOID)do_removedns("192.0.2.53");
}

static LONG pr_removedns(LONG v, Regs *r)
{
    if (v == 0)
    {
        if (do_adddns("192.0.2.53") != 0)
            return PREP_NA;
        r->a0 = (APTR)"192.0.2.53";
    }
    else
    {
        r->a0 = (APTR)"198.51.100.7";           /* never added */
    }
    return PREP_OK;
}

static LONG pr_releasednslist(LONG v, Regs *r)
{
    if (v == 0)
    {
        if (do_adddns("192.0.2.53") != 0)
            return PREP_NA;
        ptr_out = do_obtaindnslist();
        if (ptr_out == NULL)
        {
            (VOID)do_removedns("192.0.2.53");
            return PREP_NA;
        }
        r->a0 = ptr_out;
    }
    else
    {
        r->a0 = NULL;
    }
    return PREP_OK;
}

static VOID po_releasednslist(LONG v, ULONG res)
{
    (VOID)res;
    if (v == 0)
        (VOID)do_removedns("192.0.2.53");
}

static LONG pr_obtaindnslist(LONG v, Regs *r)
{
    (VOID)r;
    if (v == 0 && do_adddns("192.0.2.53") != 0)
        return PREP_NA;
    return PREP_OK;
}

static VOID po_obtaindnslist(LONG v, ULONG res)
{
    if (res != 0)
        do_releasednslist((APTR)res);
    if (v == 0)
        (VOID)do_removedns("192.0.2.53");
}

static LONG pr_setent(LONG v, Regs *r)
{
    r->d0 = (v == 0) ? 1 : 0;
    return PREP_OK;
}

static LONG pr_inet_aton(LONG v, Regs *r)
{
    r->a0 = (v == 0) ? (APTR)"10.0.2.15" : (APTR)"not.an.address";
    r->a1 = (APTR)&inaddr;
    return PREP_OK;
}

static LONG pr_inet_ntop(LONG v, Regs *r)
{
    inaddr.s_addr = 0x0A00020FUL;
    r->d0 = (v == 0) ? (ULONG)P_AF_INET : 99;
    r->a0 = (APTR)&inaddr;
    r->a1 = (APTR)namebuf;
    r->d1 = (ULONG)sizeof(namebuf);
    return PREP_OK;
}

static LONG pr_inet_pton(LONG v, Regs *r)
{
    r->d0 = P_AF_INET;
    r->a0 = (v == 0) ? (APTR)"10.0.2.15" : (APTR)"10.0.2";
    r->a1 = (APTR)&inaddr;
    return PREP_OK;
}

static LONG pr_inaddr(LONG v, Regs *r)
{
    r->d0 = (v == 0) ? 0x7F000001UL : 0x08080808UL;
    return PREP_OK;
}

static LONG pr_processisserver(LONG v, Regs *r)
{
    r->a0 = (v == 0) ? (APTR)FindTask(NULL) : NULL;
    return PREP_OK;
}

static LONG pr_getdomain(LONG v, Regs *r)
{
    r->a0 = (v == 0) ? (APTR)namebuf : NULL;
    r->d0 = (ULONG)sizeof(namebuf);
    return PREP_OK;
}

static char saved_domain[128];

static LONG pr_setdomain(LONG v, Regs *r)
{
    static char toolong[300];
    ULONG       i;

    if (v == 0)
    {
        r->a0 = (APTR)"drill.invalid";
    }
    else
    {
        for (i = 0; i < sizeof(toolong) - 1; i++)
            toolong[i] = 'x';
        toolong[sizeof(toolong) - 1] = '\0';
        r->a0 = (APTR)toolong;
    }
    return PREP_OK;
}

static VOID po_setdomain(LONG v, ULONG res)
{
    Regs r;

    (VOID)v;
    (VOID)res;
    r = regs_zero;
    r.a0 = (APTR)saved_domain;
    (VOID)t_708(ctx.base, &r);
}

/*
 * RemoveInterface has no success variant: removing the only interface takes
 * the network away and every later row would be measuring a different
 * machine.  tests/tools/run-ifremove.sh drives that path.
 */
static LONG pr_removeiface(LONG v, Regs *r)
{
    (VOID)v;
    r->a0 = (APTR)"nosuchif";
    r->d0 = 0;
    return PREP_OK;
}

static LONG pr_gethostbyname_r(LONG v, Regs *r)
{
    herr  = 0;
    r->a0 = (APTR)"127.0.0.1";
    r->a1 = (APTR)&hent;
    r->a2 = (APTR)hbuf;
    r->d0 = (v == 0) ? (ULONG)sizeof(hbuf) : 8;     /* 8 is ERANGE */
    r->a3 = (APTR)&herr;
    return PREP_OK;
}

static LONG pr_gethostbyaddr_r(LONG v, Regs *r)
{
    herr = 0;
    buf_small[0] = 127;
    buf_small[1] = 0;
    buf_small[2] = 0;
    buf_small[3] = 1;
    r->a0 = (APTR)buf_small;
    r->d0 = 4;
    r->d1 = (v == 0) ? (ULONG)P_AF_INET : 99;
    r->a1 = (APTR)&hent;
    r->a2 = (APTR)hbuf;
    r->d2 = (ULONG)sizeof(hbuf);
    r->a3 = (APTR)&herr;
    return PREP_OK;
}

static LONG pr_freeaddrinfo(LONG v, Regs *r)
{
    if (v == 0)
    {
        if (do_getaddrinfo("10.0.2.15", &ptr_out) != 0 || ptr_out == NULL)
            return PREP_NA;
        r->a0 = ptr_out;
        ptr_out = NULL;
    }
    else
    {
        r->a0 = NULL;
    }
    return PREP_OK;
}

static LONG pr_getaddrinfo(LONG v, Regs *r)
{
    static struct addrinfo hints;

    zero(&hints, sizeof(hints));
    hints.ai_family   = P_AF_INET;
    hints.ai_socktype = P_SOCK_DGRAM;
    hints.ai_flags    = AI_NUMERICHOST;

    ptr_out = NULL;
    r->a0 = (v == 0) ? (APTR)"10.0.2.15" : (APTR)"no.such.host.invalid";
    r->a1 = NULL;
    r->a2 = (APTR)&hints;
    r->a3 = (APTR)&ptr_out;
    return PREP_OK;
}

static VOID po_getaddrinfo(LONG v, ULONG res)
{
    (VOID)v;
    (VOID)res;
    if (ptr_out != NULL)
    {
        do_freeaddrinfo(ptr_out);
        ptr_out = NULL;
    }
}

static LONG pr_gaistrerror(LONG v, Regs *r)
{
    r->a0 = (APTR)((v == 0) ? (LONG)EAI_NONAME : 12345L);
    return PREP_OK;
}

static LONG pr_getnameinfo(LONG v, Regs *r)
{
    fill_peer();
    r->a0 = (APTR)&sa_peer;
    r->d0 = (v == 0) ? (ULONG)sizeof(sa_peer) : 0;
    r->a1 = (APTR)namebuf;
    r->d1 = (ULONG)sizeof(namebuf);
    r->a2 = (APTR)buf_small;
    r->d2 = (ULONG)sizeof(buf_small);
    r->d3 = (ULONG)(NI_NUMERICHOST | NI_NUMERICSERV);
    return PREP_OK;
}

static LONG pr_nxcontext(LONG v, Regs *r)
{
    ptr_out = NULL;
    r->d0 = (v == 0) ? 0x414E5844UL : 0xdeadbeefUL;     /* 'ANXD' */
    r->d1 = 0x00020001UL;
    r->a0 = (APTR)&ptr_out;
    return PREP_OK;
}

static LONG pr_netstackquery(LONG v, Regs *r)
{
    zero(&q_health, sizeof(q_health));
    q_health.hdr.nsh_Magic   = AMI_NETSTATUS_MAGIC;
    q_health.hdr.nsh_Version = AMI_NETSTATUS_VERSION;

    r->d0 = (v == 0) ? (ULONG)AMI_NETSTATUS_MAGIC : 0xdeadbeefUL;
    r->d1 = NETSTATUS_HEALTH;
    r->a0 = (APTR)&q_health;
    r->d2 = (ULONG)sizeof(q_health);
    return PREP_OK;
}

static NetStatusControl ctl;

static LONG pr_netstackcontrol(LONG v, Regs *r)
{
    zero(&ctl, sizeof(ctl));
    ctl.nsc_Magic   = AMI_NETSTATUS_MAGIC;
    ctl.nsc_Version = AMI_NETSTATUS_VERSION;

    r->d0 = (v == 0) ? (ULONG)AMI_NETSTATUS_MAGIC : 0xdeadbeefUL;
    r->d1 = NETCTRL_ARP_FLUSH;          /* the cache refills itself */
    r->a0 = (APTR)&ctl;
    r->d2 = (ULONG)sizeof(ctl);
    return PREP_OK;
}

static LONG pr_nametoindex(LONG v, Regs *r)
{
    r->a0 = (v == 0) ? (APTR)ctx.iface : (APTR)"nosuchif";
    return PREP_OK;
}

static LONG pr_indextoname(LONG v, Regs *r)
{
    r->d0 = (v == 0) ? ctx.ifindex : 999;
    r->a0 = (APTR)namebuf;
    return (v == 0 && ctx.ifindex == 0) ? PREP_NA : PREP_OK;
}

static VOID po_nameindex(LONG v, ULONG res)
{
    (VOID)v;
    if (res != 0)
        do_freenameindex((APTR)res);
}

static LONG pr_freenameindex(LONG v, Regs *r)
{
    if (v == 0)
    {
        ptr_out = do_nameindex();
        if (ptr_out == NULL)
            return PREP_NA;
        r->a0 = ptr_out;
        ptr_out = NULL;
    }
    else
    {
        r->a0 = NULL;
    }
    return PREP_OK;
}

/* ---------------------------------------------------------- the specials -- */

/* lib_open and lib_close are exec's, and calling them at their LVOs behind
   exec's back would corrupt the open count.  They are driven the only way a
   program may drive them, and both rows report the pair. */
static ULONG t_openclose(struct Library *base, const Regs *r)
{
    struct Library *l;

    (VOID)base;
    (VOID)r;

    l = OpenLibrary((CONST_STRPTR)LIB_NAME, 0);
    if (l == NULL)
        return 0;
    CloseLibrary(l);
    return 1;
}

/* The sampling bracket around nothing at all: the noise floor. */
static ULONG t_noop(struct Library *base, const Regs *r)
{
    (VOID)base;
    (VOID)r;
    return 0;
}

/* ------------------------------------------------------------- the table -- */

enum
{
    VC_IMPL = 0,        /* implemented; no return value is asserted        */
    VC_STUB_L,          /* bsd_enosys:      must return -1                 */
    VC_STUB_P,          /* bsd_enosys_ptr:  must return NULL               */
    VC_STUB_B,          /* bsd_enosys_bool: must return FALSE              */
    VC_REFUSE_L,        /* implemented, and documented always to fail (-1) */
    VC_REFUSE_B,        /* implemented, and documented always to be FALSE  */
    VC_EXEC             /* exec's own four                                 */
};

typedef struct VecRow
{
    const char *name;
    LONG        lvomag;     /* the positive magnitude: LVO is -lvomag      */
    ThunkFn     thunk;
    UBYTE       cls;
    UBYTE       variants;
    UWORD       iters;
    PrepFn      prep;
    PostFn      post;
    const char *skip;       /* non-NULL: not called, and why               */
} VecRow;

#define V_(name, mag, th, cls, nv, it, prep, post) \
    { name, mag, th, cls, nv, it, prep, post, NULL }
#define S_(name, mag, th, why) \
    { name, mag, th, VC_IMPL, 0, 0, NULL, NULL, why }
#define E_(name, mag, th, cls) \
    { name, mag, th, cls, 1, DEF_ITERS, NULL, NULL, NULL }

static const VecRow vectors[] =
{
    /* exec's four */
    V_("lib_open",  6, t_openclose, VC_EXEC, 1, FEW_ITERS, NULL, NULL),
    V_("lib_close", 12, t_openclose, VC_EXEC, 1, FEW_ITERS, NULL, NULL),
    S_("lib_expunge", 18, t_18,
       "unloads the segment this program is calling into; run-cycledrill.sh"),
    E_("lib_reserved", 24, t_24, VC_IMPL),

    V_("socket",              30, t_30,  VC_IMPL, 2, DEF_ITERS, pr_socket,      po_socket),
    V_("bind",                36, t_36,  VC_IMPL, 2, DEF_ITERS, pr_bind,        po_bind),
    V_("listen",              42, t_42,  VC_IMPL, 2, DEF_ITERS, pr_listen,      po_listen),
    V_("accept",              48, t_48,  VC_IMPL, 2, DEF_ITERS, pr_accept,      po_accept),
    V_("connect",             54, t_54,  VC_IMPL, 2, DEF_ITERS, pr_connect,     po_connect),
    V_("sendto",              60, t_60,  VC_IMPL, 2, DEF_ITERS, pr_sendto,      NULL),
    V_("send",                66, t_66,  VC_IMPL, 2, DEF_ITERS, pr_send,        NULL),
    V_("recvfrom",            72, t_72,  VC_IMPL, 2, DEF_ITERS, pr_recvfrom,    NULL),
    V_("recv",                78, t_78,  VC_IMPL, 2, DEF_ITERS, pr_recv,        NULL),
    V_("shutdown",            84, t_84,  VC_IMPL, 2, DEF_ITERS, pr_shutdown,    po_shutdown),
    V_("setsockopt",          90, t_90,  VC_IMPL, 2, DEF_ITERS, pr_setsockopt,  NULL),
    V_("getsockopt",          96, t_96,  VC_IMPL, 2, DEF_ITERS, pr_getsockopt,  NULL),
    V_("getsockname",        102, t_102, VC_IMPL, 2, DEF_ITERS, pr_getsockname, NULL),
    V_("getpeername",        108, t_108, VC_IMPL, 2, DEF_ITERS, pr_getpeername, NULL),
    V_("IoctlSocket",        114, t_114, VC_IMPL, 2, DEF_ITERS, pr_ioctl,       NULL),
    V_("CloseSocket",        120, t_120, VC_IMPL, 2, DEF_ITERS, pr_close,       NULL),
    V_("WaitSelect",         126, t_126, VC_IMPL, 2, FEW_ITERS, pr_waitselect,  NULL),
    V_("SetSocketSignals",   132, t_132, VC_IMPL, 1, DEF_ITERS, pr_setsocketsignals, NULL),
    E_("getdtablesize",      138, t_138, VC_IMPL),
    V_("ObtainSocket",       144, t_144, VC_IMPL, 2, FEW_ITERS, pr_obtainsocket, po_obtainsocket),
    V_("ReleaseSocket",      150, t_150, VC_IMPL, 2, FEW_ITERS, pr_releasesocket, po_releasesocket),
    V_("ReleaseCopyOfSocket",156, t_156, VC_IMPL, 2, FEW_ITERS, pr_releasecopy, po_releasecopy),
    E_("Errno",              162, t_162, VC_IMPL),
    V_("SetErrnoPtr",        168, t_168, VC_IMPL, 2, DEF_ITERS, pr_seterrnoptr, po_seterrnoptr),
    V_("Inet_NtoA",          174, t_174, VC_IMPL, 1, DEF_ITERS, pr_inet_ntoa,   NULL),
    V_("inet_addr",          180, t_180, VC_IMPL, 2, DEF_ITERS, pr_inet_addr,   NULL),
    V_("Inet_LnaOf",         186, t_186, VC_IMPL, 2, DEF_ITERS, pr_lnaof,       NULL),
    V_("Inet_NetOf",         192, t_192, VC_IMPL, 2, DEF_ITERS, pr_lnaof,       NULL),
    V_("Inet_MakeAddr",      198, t_198, VC_IMPL, 2, DEF_ITERS, pr_makeaddr,    NULL),
    V_("inet_network",       204, t_204, VC_IMPL, 2, DEF_ITERS, pr_inet_addr,   NULL),
    V_("gethostbyname",      210, t_210, VC_IMPL, 2, FEW_ITERS, pr_gethostbyname, NULL),
    V_("gethostbyaddr",      216, t_216, VC_IMPL, 2, FEW_ITERS, pr_gethostbyaddr, NULL),
    V_("getnetbyname",       222, t_222, VC_IMPL, 2, DEF_ITERS, pr_getnetbyname, NULL),
    V_("getnetbyaddr",       228, t_228, VC_IMPL, 2, DEF_ITERS, pr_getnetbyaddr, NULL),
    V_("getservbyname",      234, t_234, VC_IMPL, 2, DEF_ITERS, pr_getservbyname, NULL),
    V_("getservbyport",      240, t_240, VC_IMPL, 2, DEF_ITERS, pr_getservbyport, NULL),
    V_("getprotobyname",     246, t_246, VC_IMPL, 2, DEF_ITERS, pr_getprotobyname, NULL),
    V_("getprotobynumber",   252, t_252, VC_IMPL, 2, DEF_ITERS, pr_getprotobynumber, NULL),
    E_("vsyslog",            258, t_258, VC_STUB_L),
    V_("Dup2Socket",         264, t_264, VC_IMPL, 2, DEF_ITERS, pr_dup2,        po_dup2),
    V_("sendmsg",            270, t_270, VC_IMPL, 2, DEF_ITERS, pr_sendmsg,     NULL),
    V_("recvmsg",            276, t_276, VC_IMPL, 2, DEF_ITERS, pr_recvmsg,     NULL),
    V_("gethostname",        282, t_282, VC_IMPL, 2, FEW_ITERS, pr_gethostname, NULL),
    E_("gethostid",          288, t_288, VC_IMPL),
    V_("SocketBaseTagList",  294, t_294, VC_IMPL, 2, DEF_ITERS, pr_sbtaglist,   NULL),
    V_("GetSocketEvents",    300, t_300, VC_IMPL, 2, DEF_ITERS, pr_getsocketevents, NULL),

    E_("reserved.50",        306, t_306, VC_STUB_L),
    E_("reserved.51",        312, t_312, VC_STUB_L),
    E_("reserved.52",        318, t_318, VC_STUB_L),
    E_("reserved.53",        324, t_324, VC_STUB_L),
    E_("reserved.54",        330, t_330, VC_STUB_L),
    E_("reserved.55",        336, t_336, VC_STUB_L),
    E_("reserved.56",        342, t_342, VC_STUB_L),
    E_("reserved.57",        348, t_348, VC_STUB_L),
    E_("reserved.58",        354, t_354, VC_STUB_L),
    E_("reserved.59",        360, t_360, VC_STUB_L),

    V_("bpf_open",           366, t_366, VC_IMPL, 2, DEF_ITERS, pr_bpf_open,    po_bpf_open),
    V_("bpf_close",          372, t_372, VC_IMPL, 2, DEF_ITERS, pr_bpf_close,   NULL),
    V_("bpf_read",           378, t_378, VC_IMPL, 2, DEF_ITERS, pr_bpf_read,    NULL),
    V_("bpf_write",          384, t_384, VC_IMPL, 2, DEF_ITERS, pr_bpf_write,   NULL),
    V_("bpf_set_notify_mask",390, t_390, VC_IMPL, 2, DEF_ITERS, pr_bpf_notify,  NULL),
    V_("bpf_set_interrupt_mask",396, t_396, VC_IMPL, 2, DEF_ITERS, pr_bpf_interrupt, NULL),
    V_("bpf_ioctl",          402, t_402, VC_IMPL, 2, DEF_ITERS, pr_bpf_ioctl,   NULL),
    V_("bpf_data_waiting",   408, t_408, VC_IMPL, 2, DEF_ITERS, pr_bpf_chan,    NULL),

    V_("AddRouteTagList",    414, t_414, VC_IMPL, 2, FEW_ITERS, pr_addroute,    po_addroute),
    V_("DeleteRouteTagList", 420, t_420, VC_IMPL, 2, FEW_ITERS, pr_delroute,    NULL),
    E_("ChangeRouteTagList", 426, t_426, VC_STUB_L),
    V_("FreeRouteInfo",      432, t_432, VC_IMPL, 2, FEW_ITERS, pr_freerouteinfo, NULL),
    V_("GetRouteInfo",       438, t_438, VC_IMPL, 2, FEW_ITERS, pr_getrouteinfo, po_getrouteinfo),
    V_("AddInterfaceTagList",444, t_444, VC_IMPL, 2, FEW_ITERS, pr_addiface,    NULL),
    V_("ConfigureInterfaceTagList",450, t_450, VC_IMPL, 2, FEW_ITERS, pr_configiface, NULL),
    V_("ReleaseInterfaceList",456, t_456, VC_IMPL, 2, FEW_ITERS, pr_releaseiflist, NULL),
    V_("ObtainInterfaceList",462, t_462, VC_IMPL, 1, FEW_ITERS, pr_none,        po_obtainiflist),
    V_("QueryInterfaceTagList",468, t_468, VC_IMPL, 2, FEW_ITERS, pr_queryiface, NULL),
    V_("CreateAddrAllocMessageA",474, t_474, VC_IMPL, 2, FEW_ITERS, pr_createaam, po_createaam),
    V_("DeleteAddrAllocMessage",480, t_480, VC_IMPL, 2, FEW_ITERS, pr_deleteaam, NULL),
    V_("BeginInterfaceConfig",486, t_486, VC_IMPL, 2, FEW_ITERS, pr_beginconfig, po_beginconfig),
    V_("AbortInterfaceConfig",492, t_492, VC_IMPL, 2, FEW_ITERS, pr_abortconfig, po_abortconfig),
    V_("AddNetMonitorHookTagList",498, t_498, VC_IMPL, 2, FEW_ITERS, pr_addmonhook, po_addmonhook),
    V_("RemoveNetMonitorHook",504, t_504, VC_IMPL, 2, FEW_ITERS, pr_removemonhook, NULL),
    V_("GetNetworkStatistics",510, t_510, VC_IMPL, 2, FEW_ITERS, pr_netstats,   NULL),
    V_("AddDomainNameServer",516, t_516, VC_IMPL, 2, FEW_ITERS, pr_adddns,      po_adddns),
    V_("RemoveDomainNameServer",522, t_522, VC_IMPL, 2, FEW_ITERS, pr_removedns, NULL),
    V_("ReleaseDomainNameServerList",528, t_528, VC_IMPL, 2, FEW_ITERS, pr_releasednslist, po_releasednslist),
    V_("ObtainDomainNameServerList",534, t_534, VC_IMPL, 1, FEW_ITERS, pr_obtaindnslist, po_obtaindnslist),
    V_("setnetent",          540, t_540, VC_IMPL, 2, DEF_ITERS, pr_setent,      NULL),
    E_("endnetent",          546, t_546, VC_IMPL),
    E_("getnetent",          552, t_552, VC_IMPL),
    V_("setprotoent",        558, t_558, VC_IMPL, 2, DEF_ITERS, pr_setent,      NULL),
    E_("endprotoent",        564, t_564, VC_IMPL),
    E_("getprotoent",        570, t_570, VC_IMPL),
    V_("setservent",         576, t_576, VC_IMPL, 2, DEF_ITERS, pr_setent,      NULL),
    E_("endservent",         582, t_582, VC_IMPL),
    E_("getservent",         588, t_588, VC_IMPL),
    V_("inet_aton",          594, t_594, VC_IMPL, 2, DEF_ITERS, pr_inet_aton,   NULL),
    V_("inet_ntop",          600, t_600, VC_IMPL, 2, DEF_ITERS, pr_inet_ntop,   NULL),
    V_("inet_pton",          606, t_606, VC_IMPL, 2, DEF_ITERS, pr_inet_pton,   NULL),
    V_("In_LocalAddr",       612, t_612, VC_IMPL, 2, DEF_ITERS, pr_inaddr,      NULL),
    V_("In_CanForward",      618, t_618, VC_IMPL, 2, DEF_ITERS, pr_inaddr,      NULL),

    E_("mbuf_copym",         624, t_624, VC_STUB_P),
    E_("mbuf_copyback",      630, t_630, VC_STUB_L),
    E_("mbuf_copydata",      636, t_636, VC_STUB_L),
    E_("mbuf_free",          642, t_642, VC_STUB_P),
    E_("mbuf_freem",         648, t_648, VC_STUB_L),
    E_("mbuf_get",           654, t_654, VC_STUB_P),
    E_("mbuf_gethdr",        660, t_660, VC_STUB_P),
    E_("mbuf_prepend",       666, t_666, VC_STUB_P),
    E_("mbuf_cat",           672, t_672, VC_STUB_L),
    E_("mbuf_adj",           678, t_678, VC_STUB_L),
    E_("mbuf_pullup",        684, t_684, VC_STUB_P),

    V_("ProcessIsServer",    690, t_690, VC_REFUSE_B, 2, DEF_ITERS, pr_processisserver, NULL),
    E_("ObtainServerSocket", 696, t_696, VC_REFUSE_L),
    V_("GetDefaultDomainName",702, t_702, VC_IMPL, 2, DEF_ITERS, pr_getdomain,  NULL),
    V_("SetDefaultDomainName",708, t_708, VC_IMPL, 2, DEF_ITERS, pr_setdomain,  po_setdomain),
    E_("ObtainRoadshowData",  714, t_714, VC_STUB_P),
    E_("ReleaseRoadshowData", 720, t_720, VC_STUB_L),
    E_("ChangeRoadshowData",  726, t_726, VC_STUB_B),
    V_("RemoveInterface",     732, t_732, VC_IMPL, 1, FEW_ITERS, pr_removeiface, NULL),
    V_("gethostbyname_r",     738, t_738, VC_IMPL, 2, FEW_ITERS, pr_gethostbyname_r, NULL),
    V_("gethostbyaddr_r",     744, t_744, VC_IMPL, 2, FEW_ITERS, pr_gethostbyaddr_r, NULL),

    E_("reserved.124",        750, t_750, VC_STUB_L),
    E_("reserved.125",        756, t_756, VC_STUB_L),
    E_("ipf_open",            762, t_762, VC_STUB_L),
    E_("ipf_close",           768, t_768, VC_STUB_L),
    E_("ipf_ioctl",           774, t_774, VC_STUB_L),
    E_("ipf_log_read",        780, t_780, VC_STUB_L),
    E_("ipf_log_data_waiting",786, t_786, VC_STUB_L),
    E_("ipf_set_notify_mask", 792, t_792, VC_STUB_L),
    E_("ipf_set_interrupt_mask",798, t_798, VC_STUB_L),

    V_("freeaddrinfo",        804, t_804, VC_IMPL, 2, FEW_ITERS, pr_freeaddrinfo, NULL),
    V_("getaddrinfo",         810, t_810, VC_IMPL, 2, FEW_ITERS, pr_getaddrinfo, po_getaddrinfo),
    V_("gai_strerror",        816, t_816, VC_IMPL, 2, DEF_ITERS, pr_gaistrerror, NULL),
    V_("getnameinfo",         822, t_822, VC_IMPL, 2, FEW_ITERS, pr_getnameinfo, NULL),

    E_("reserved.137",        828, t_828, VC_STUB_L),
    E_("reserved.138",        834, t_834, VC_STUB_L),
    E_("reserved.139",        840, t_840, VC_STUB_L),
    E_("reserved.140",        846, t_846, VC_STUB_L),
    E_("reserved.141",        852, t_852, VC_STUB_L),
    E_("reserved.142",        858, t_858, VC_STUB_L),

    V_("ObtainNetXDuoContext",864, t_864, VC_IMPL, 2, DEF_ITERS, pr_nxcontext,  NULL),
    V_("NetStackQuery",       870, t_870, VC_IMPL, 2, FEW_ITERS, pr_netstackquery, NULL),
    V_("NetStackControl",     876, t_876, VC_IMPL, 2, FEW_ITERS, pr_netstackcontrol, NULL),

    V_("if_nametoindex",      882, t_882, VC_IMPL, 2, DEF_ITERS, pr_nametoindex, NULL),
    V_("if_indextoname",      888, t_888, VC_IMPL, 2, DEF_ITERS, pr_indextoname, NULL),
    V_("if_nameindex",        894, t_894, VC_IMPL, 1, FEW_ITERS, pr_none,       po_nameindex),
    V_("if_freenameindex",    900, t_900, VC_IMPL, 2, FEW_ITERS, pr_freenameindex, NULL)
};

#define NVECTORS ((LONG)(sizeof(vectors) / sizeof(vectors[0])))

/* ------------------------------------------------------ the broken rows --- */

/*
 * Deliberately broken, so a run can show every gate going red.  Nothing here
 * calls the library: each one takes a resource on the caller's side and does
 * not give it back, which is exactly what a leaking vector does, and the gate
 * that has to notice is named in the row.
 */
static ULONG t_leak_mem(struct Library *base, const Regs *r)
{
    (VOID)base;
    (VOID)r;
    return (ULONG)AllocVec(64, MEMF_ANY);
}

static ULONG t_leak_sig(struct Library *base, const Regs *r)
{
    (VOID)base;
    (VOID)r;
    return (ULONG)AllocSignal(-1);
}

static ULONG t_leak_sock(struct Library *base, const Regs *r)
{
    (VOID)base;
    (VOID)r;
    return (ULONG)do_socket(P_AF_INET, P_SOCK_DGRAM, 0);
}

/* AllocVec + AddPort rather than CreateMsgPort(): a CreateMsgPort() port is
   private and never reaches SysBase->PortList, so it would prove the signal
   gate twice over and the port gate not at all. */
static ULONG t_leak_port(struct Library *base, const Regs *r)
{
    struct MsgPort *mp;

    (VOID)base;
    (VOID)r;

    mp = (struct MsgPort *)AllocVec(sizeof(*mp), MEMF_ANY | MEMF_CLEAR);
    if (mp == NULL)
        return 0;

    mp->mp_Node.ln_Name = (char *)"apidrill.broken";
    mp->mp_Node.ln_Type = NT_MSGPORT;
    mp->mp_Flags        = PA_IGNORE;
    mp->mp_MsgList.lh_Head     = (struct Node *)&mp->mp_MsgList.lh_Tail;
    mp->mp_MsgList.lh_Tail     = NULL;
    mp->mp_MsgList.lh_TailPred = (struct Node *)&mp->mp_MsgList;
    AddPort(mp);
    return (ULONG)mp;
}

static ULONG t_leak_sem(struct Library *base, const Regs *r)
{
    struct SignalSemaphore *s;

    (VOID)base;
    (VOID)r;

    s = (struct SignalSemaphore *)AllocVec(sizeof(*s), MEMF_ANY | MEMF_CLEAR);
    if (s == NULL)
        return 0;
    s->ss_Link.ln_Name = (char *)"apidrill.broken";
    AddSemaphore(s);
    return (ULONG)s;
}

static const VecRow controls[] =
{
    V_("!noise64", 0, t_noop, VC_IMPL, 1, DEF_ITERS, NULL, NULL),
    V_("!noise16", 0, t_noop, VC_IMPL, 1, FEW_ITERS, NULL, NULL)
};

static const VecRow broken[] =
{
    V_("!broken-mem",  0, t_leak_mem,  VC_IMPL, 1, FEW_ITERS, NULL, NULL),
    V_("!broken-sig",  0, t_leak_sig,  VC_IMPL, 1, 4,         NULL, NULL),
    V_("!broken-sock", 0, t_leak_sock, VC_IMPL, 1, FEW_ITERS, NULL, NULL),
    V_("!broken-port", 0, t_leak_port, VC_IMPL, 1, FEW_ITERS, NULL, NULL),
    V_("!broken-sem",  0, t_leak_sem,  VC_IMPL, 1, FEW_ITERS, NULL, NULL),
    /* The same stub the table calls, asserted against the wrong value. */
    V_("!broken-rc",   258, t_258, VC_STUB_B, 1, FEW_ITERS, NULL, NULL)
};

#define NCONTROLS ((LONG)(sizeof(controls) / sizeof(controls[0])))
#define NBROKEN   ((LONG)(sizeof(broken)   / sizeof(broken[0])))

/* -------------------------------------------------------------- the run --- */

static LONG expected_of(UBYTE cls, BOOL *has)
{
    *has = TRUE;
    switch (cls)
    {
        case VC_STUB_L:   return -1;
        case VC_STUB_P:   return 0;
        case VC_STUB_B:   return 0;
        case VC_REFUSE_L: return -1;
        case VC_REFUSE_B: return 0;
        default:          break;
    }
    *has = FALSE;
    return 0;
}

static VOID run_variant(const VecRow *v, LONG idx, LONG variant)
{
    Regs   r;
    Sample before;
    Sample after;
    LONG   i;
    LONG   iters = (LONG)v->iters;

    if (iters == DEF_ITERS)
        iters = g_def;
    else if (iters == FEW_ITERS)
        iters = g_few;
    ULONG  res   = 0;
    LONG   badrc = 0;
    LONG   dbytes;
    LONG   bpc;
    BOOL   has_expect;
    LONG   expect;
    LONG   args[14];
    LONG   ok;

    expect = expected_of(v->cls, &has_expect);

    say("> %ld %s v%ld\n", idx, (LONG)v->name, variant, 0);
    flushout();

    /* The one-off: a cache built on first use is not a leak. */
    r = regs_zero;
    if (v->prep != NULL && v->prep(variant, &r) == PREP_NA)
    {
        say("V %ld %s v%ld it0 NOTAPPLICABLE\n",
            idx, (LONG)v->name, variant, 0);
        flushout();
        return;
    }
    res = v->thunk(ctx.base, &r);
    if (v->post != NULL)
        v->post(variant, res);

    variants_run++;

    flushout();
    sample(ctx.base, &before);

    for (i = 0; i < iters; i++)
    {
        r = regs_zero;
        if (v->prep != NULL)
            (VOID)v->prep(variant, &r);
        res = v->thunk(ctx.base, &r);
        if (has_expect && (LONG)res != expect)
            badrc++;
        if (v->post != NULL)
            v->post(variant, res);
    }

    sample(ctx.base, &after);

    dbytes = (LONG)before.free_mem - (LONG)after.free_mem;
    bpc    = dbytes / iters;

    args[0]  = idx;
    args[1]  = (LONG)v->name;
    args[2]  = variant;
    args[3]  = iters;
    args[4]  = (LONG)res;
    args[5]  = bpc;
    args[6]  = dbytes;
    args[7]  = (LONG)after.alloc_live - (LONG)before.alloc_live;
    args[8]  = (LONG)after.sockets    - (LONG)before.sockets;
    args[9]  = (LONG)after.sigs       - (LONG)before.sigs;
    args[10] = (LONG)after.ports      - (LONG)before.ports;
    args[11] = (LONG)after.sems       - (LONG)before.sems;
    args[12] = (LONG)after.tasks      - (LONG)before.tasks;
    args[13] = (LONG)after.pool_free  - (LONG)before.pool_free;

    sayv("V %ld %s v%ld it=%ld rc=%ld bytes=%ld total=%ld alloc=%ld sock=%ld "
         "sig=%ld port=%ld sem=%ld task=%ld pool=%ld\n", args);

    /* Loss only.  A window in which the machine handed memory BACK is noise,
       not a negative leak, and failing on it would only teach people that a
       red row means nothing. */
    ok  = check(bpc <= 0, v->name, bpc, dbytes) ? 1 : 0;
    ok &= check(after.alloc_live == before.alloc_live, v->name,
                (LONG)before.alloc_live, (LONG)after.alloc_live) ? 1 : 0;
    ok &= check(after.sockets == before.sockets, v->name,
                (LONG)before.sockets, (LONG)after.sockets) ? 1 : 0;
    ok &= check(after.sigs == before.sigs, v->name,
                (LONG)before.sigs, (LONG)after.sigs) ? 1 : 0;
    ok &= check(after.ports == before.ports, v->name,
                (LONG)before.ports, (LONG)after.ports) ? 1 : 0;
    ok &= check(after.sems == before.sems, v->name,
                (LONG)before.sems, (LONG)after.sems) ? 1 : 0;
    ok &= check(after.tasks == before.tasks, v->name,
                (LONG)before.tasks, (LONG)after.tasks) ? 1 : 0;
    ok &= check(after.pool_bad == before.pool_bad, v->name,
                (LONG)before.pool_bad, (LONG)after.pool_bad) ? 1 : 0;
    ok &= check(before.ok && after.ok, v->name, (LONG)before.ok,
                (LONG)after.ok) ? 1 : 0;

    if (has_expect)
        ok &= check(badrc == 0, v->name, badrc, expect) ? 1 : 0;

    say("= %ld %s %s\n", idx, (LONG)v->name,
        (LONG)(ok ? "ok" : "LEAK"), 0);
    flushout();
}

static VOID run_row(const VecRow *v, LONG idx)
{
    LONG variant;

    if (v->skip != NULL)
    {
        uncovered++;
        say("V %ld %s UNCOVERED %s\n", idx, (LONG)v->name, (LONG)v->skip, 0);
        flushout();
        return;
    }

    covered++;
    for (variant = 0; variant < (LONG)v->variants; variant++)
        run_variant(v, idx, variant);
}

/* --------------------------------------------------------- bring-up ------- */

/*
 * The table has to be dense and in LVO order, because that is the only thing
 * that makes a vector nobody wrote a row for visible: a missing row shifts
 * every later one and this check fails naming the first that moved.
 */
static BOOL table_is_dense(VOID)
{
    LONG i;
    BOOL ok = TRUE;

    for (i = 0; i < NVECTORS; i++)
    {
        if (vectors[i].lvomag != 6 * (i + 1))
        {
            say("FAIL: row %ld (%s) is at -%ld, expected -%ld\n",
                i, (LONG)vectors[i].name, vectors[i].lvomag, 6 * (i + 1));
            ok = FALSE;
        }
    }

    checks++;
    if (!ok)
        failures++;

    return ok;
}

static VOID find_interface(const char *want)
{
    Regs  r;
    ULONG idx;
    LONG  i;

    for (i = 0; want[i] != '\0' && i < 15; i++)
        ctx.iface[i] = want[i];
    ctx.iface[i] = '\0';

    r = regs_zero;
    r.a0 = (APTR)ctx.iface;
    idx = t_882(ctx.base, &r);          /* if_nametoindex */
    ctx.ifindex = idx;
}

static BOOL bring_up(const char *want_iface)
{
    Regs r;
    LONG nb = 1;

    ctx.udp  = -1;
    ctx.tcp  = -1;
    ctx.dead = -1;
    ctx.bpf  = -1;

    ctx.udp = do_socket(P_AF_INET, P_SOCK_DGRAM, 0);
    ctx.tcp = do_socket(P_AF_INET, P_SOCK_STREAM, 0);
    if (ctx.udp < 0 || ctx.tcp < 0)
        return FALSE;

    /* Non-blocking, so every recv on it refuses at once rather than waiting
       for a datagram that is not coming. */
    (VOID)do_ioctl(ctx.udp, P_FIONBIO, &nb);

    fill_peer();
    r = regs_zero;
    r.d0 = (ULONG)ctx.udp;
    r.a0 = (APTR)&sa_peer;
    r.d1 = (ULONG)sizeof(sa_peer);
    (VOID)t_54(ctx.base, &r);           /* connect, so send() has a peer */

    /* A descriptor that is guaranteed to be closed, for every failure path
       that needs an EBADF. */
    ctx.dead = do_socket(P_AF_INET, P_SOCK_DGRAM, 0);
    if (ctx.dead >= 0)
        (VOID)do_close(ctx.dead);

    ctx.bpf  = do_bpfopen(-1);
    ctx.port = CreateMsgPort();

    find_interface(want_iface);

    /* Remember the domain name so the SetDefaultDomainName row can put it
       back; an empty answer is a legal one and is restored as empty. */
    zero(saved_domain, sizeof(saved_domain));
    r = regs_zero;
    r.a0 = (APTR)saved_domain;
    r.d0 = (ULONG)sizeof(saved_domain) - 1;
    (VOID)t_702(ctx.base, &r);          /* GetDefaultDomainName */

    return TRUE;
}

static VOID tear_down(VOID)
{
    if (ctx.bpf >= 0)
        (VOID)do_bpfclose(ctx.bpf);
    if (ctx.udp >= 0)
        (VOID)do_close(ctx.udp);
    if (ctx.tcp >= 0)
        (VOID)do_close(ctx.tcp);
    if (ctx.port != NULL)
    {
        while (GetMsg(ctx.port) != NULL)
            ;
        DeleteMsgPort(ctx.port);
        ctx.port = NULL;
    }
}

/* -------------------------------------------------------------- main ------ */

int main(void)
{
    struct RDArgs *rd;
    LONG           args[ARG_COUNT];
    LONG           iters   = DEF_ITERS;
    const char    *iface   = DEF_IFACE;
    const char    *only    = NULL;
    BOOL           do_broken = FALSE;
    LONG           i;
    LONG           rc = RETURN_OK;

    zero(args, sizeof(args));
    rd = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rd != NULL)
    {
        if (args[ARG_ITERS] != 0)
            iters = *(LONG *)args[ARG_ITERS];
        if (args[ARG_IFACE] != 0)
            iface = (const char *)args[ARG_IFACE];
        if (args[ARG_ONLY] != 0)
            only = (const char *)args[ARG_ONLY];
        do_broken = (args[ARG_BROKEN] != 0);
    }

    if (iters < 2)
        iters = 2;
    if (iters > MAX_ITERS)
        iters = MAX_ITERS;
    g_def = iters;
    g_few = iters / 4;
    if (g_few < 2)
        g_few = 2;

    say("ApiDrill: %ld rows, iters %ld (few %ld), iface %s\n",
        NVECTORS, g_def, g_few, (LONG)iface);
    flushout();

    ctx.base = OpenLibrary((CONST_STRPTR)LIB_NAME, 0);
    if (ctx.base == NULL)
    {
        say("SKIPPED: %s would not open\n", (LONG)LIB_NAME, 0, 0, 0);
        if (rd != NULL)
            FreeArgs(rd);
        return RETURN_FAIL;
    }

    (VOID)table_is_dense();

    if (!bring_up(iface))
    {
        say("SKIPPED: no sockets, the stack is not up\n", 0, 0, 0, 0);
        tear_down();
        CloseLibrary(ctx.base);
        if (rd != NULL)
            FreeArgs(rd);
        return RETURN_FAIL;
    }

    say("bring-up: udp %ld tcp %ld dead %ld bpf %ld\n",
        ctx.udp, ctx.tcp, ctx.dead, ctx.bpf);
    say("bring-up: iface %s index %ld port %ld\n",
        (LONG)ctx.iface, (LONG)ctx.ifindex, (LONG)ctx.port, 0);
    flushout();

    /* The noise floor first, so a machine too busy to measure on says so
       before three hundred variants blame the library for it. */
    for (i = 0; i < NCONTROLS; i++)
        run_variant(&controls[i], -1 - i, 0);

    if (do_broken)
    {
        say("BROKEN: every row below must fail\n", 0, 0, 0, 0);
        for (i = 0; i < NBROKEN; i++)
            run_variant(&broken[i], -100 - i, 0);
    }
    else
    {
        for (i = 0; i < NVECTORS; i++)
        {
            if (only != NULL)
            {
                const char *a = only;
                const char *b = vectors[i].name;

                while (*a != '\0' && *a == *b)
                {
                    a++;
                    b++;
                }
                if (*a != '\0' || *b != '\0')
                    continue;
            }
            run_row(&vectors[i], i);
        }
    }

    tear_down();

    say("covered: %ld of %ld vectors, %ld uncovered, %ld variants run\n",
        covered, NVECTORS, uncovered, variants_run);
    say("ApiDrill: %ld checks, %ld failures\n", checks, failures, 0, 0);
    flushout();

    if (failures != 0)
        rc = RETURN_ERROR;

    CloseLibrary(ctx.base);
    if (rd != NULL)
        FreeArgs(rd);

    return (int)rc;
}
