/*
 * CycleDrill, open/expunge/reopen and interface Online/Offline, repeatedly,
 * with the counters read between every cycle.
 *
 * The soak suite covers steady state.  What kills a long-lived Amiga stack is
 * cycling: there is no MMU, so an unclean expunge is leaked signals and memory
 * at best and a crash on the next open at worst, and the failure is not
 * visible until the tenth cycle rather than the first.
 *
 * WHAT IS DRIVEN, AND WHY IN THIS ORDER
 *
 *   Phase E, expunge/reopen, runs FIRST and cold.  bsd_lib_expunge() only
 *   proceeds at lib_OpenCnt 0, and AddNetInterface deliberately leaks an
 *   OpenLibrary() reference to keep the network up (docs/RESEARCH.md 22), so
 *   after that command has run on this machine no expunge can ever be reached
 *   again.  A drill that ran this phase second would print "declined" every
 *   time and look like a pass.  Each iteration is: OpenLibrary (the stack
 *   comes up on the first open, interfaces and all), CloseLibrary (last close,
 *   so netstack_shutdown() runs and the SANA-II readers are stopped),
 *   ACTION_DIE to TCP:, RemLibrary, and the library has to be gone from
 *   SysBase->LibList afterwards.  Then OpenLibrary again, which reloads the
 *   segment from LIBS: and starts a second stack in the same machine.
 *
 *   ACTION_DIE is not optional: bsd_lib_expunge() declines while the TCP:
 *   handler process exists, because that process runs out of the segment
 *   expunge is about to hand back and holds no open count.  Sending it is the
 *   supported way down and is what makes the expunge reachable at all.
 *
 *   Phase L is phase E without the expunge: open, last close, open again, and
 *   the segment never unloaded.  It is the control that tells the two halves
 *   of phase E apart.  A last close already runs the whole teardown,
 *   netstack_shutdown(), the IP stack, the packet pool, the SANA-II readers,
 *   ThreadX, so a loss that shows in both phases is in the teardown, and a
 *   loss that shows only in phase E is in the unload and reload, which is to
 *   say in something whose only owner was a file-scope static in the segment.
 *   Phase C cannot make this split: its pairs are nested inside the anchor and
 *   never reach a last close.
 *
 *   Phase C, the cycling proper, runs under ONE held reference so the stack
 *   stays up and the counters accumulate across cycles.  That is the only
 *   arrangement in which drift is measurable: an expunge resets every counter
 *   to zero because the AmiSocketBase they live in has been freed.  Each cycle
 *   nests eight OpenLibrary/CloseLibrary pairs, bounces the interface down and
 *   up through NETCTRL_INTERFACE_DOWN/UP, holds sockets open across a bounce,
 *   and removes and re-adds the interface with those sockets still open.
 *
 *   Phase G is the interleaving the other two cannot show: RemLibrary while
 *   this program still holds the library open.  It must decline, set
 *   LIBF_DELEXP, leave the library in the list and leave our base callable.
 *
 * WHAT IS MEASURED
 *
 *   NETSTATUS_HEALTH already reports allocations outstanding, sockets alive,
 *   packet buffers free and the high-water mark of each, the same numbers
 *   `netstat -h` and `ShowNetStatus MEMORY` print, so nothing new is
 *   instrumented here.  Two things it cannot report are read from Exec
 *   instead, because both are leaked ONTO THE CALLER by a bad close and
 *   neither lives in the library:
 *
 *     * free memory, AvailMem(MEMF_ANY).  An orphaned SANA-II reader stack is
 *       32 KB of it (src/sana2/sana2_rx.c), and after an expunge it is the
 *       only yardstick left, since the library's own counters went with it.
 *     * signal bits allocated to this Process, tc_SigAlloc.  A library that
 *       AllocSignal()s on its opener and does not free it shows here and
 *       nowhere else.
 *
 *   Every phase samples at the SAME point of each cycle, so a cost that is
 *   paid once, a segment, a pool, cancels between cycles and only a trend
 *   survives.  The verdict at the end is that trend, not a pass/fail on one
 *   reading.
 *
 * WHAT IT COUNTS ABOUT ITSELF
 *
 *   The last line reports how many opens, bounces, round trips and expunges
 *   actually happened.  A drill that stopped cycling, because an interface
 *   would not come back, or an expunge was declined every time, would
 *   otherwise report "no drift" and pass.  tests/tools/run-cycledrill.sh
 *   asserts those counts, not just the absence of the word FAIL.
 *
 * Vectors are called by hand at the LVOs the ABI assigns, the same way
 * tests/tools/ifprobe.c and routeprobe.c do: the NDK inlines assume a global
 * SocketBase, and this program holds several bases at once on purpose.
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
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <devices/timer.h>      /* struct timeval, for WaitSelect */
#include <utility/tagitem.h>

/* <libraries/bsdsocket.h> pulls in <sys/socket.h>, which uses size_t and
   ssize_t without declaring them.  Same ordering note as ifprobe.c. */
#include <stddef.h>
#include <sys/types.h>
#include <libraries/bsdsocket.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include "aminetxduo/netstatus.h"

static const char version_tag[] __attribute__((used)) =
    "$VER: CycleDrill 1.0 (31.7.2026)";

#define TEMPLATE    "CYCLES/N,EXPUNGE/N,IFACE/K,SOCKETS/N"

enum
{
    ARG_CYCLES = 0,
    ARG_EXPUNGE,
    ARG_IFACE,
    ARG_SOCKETS,
    ARG_COUNT
};

/* Defaults sized for the normal suite; run-cycledrill.sh raises them from
   AMINETXDUO_CYCLE_CYCLES / AMINETXDUO_CYCLE_EXPUNGES for a soak. */
#define DEF_CYCLES      3
#define DEF_EXPUNGE     2
#define DEF_SOCKETS     2
#define DEF_IFACE       "eth0"

/* Nested OpenLibrary()s per cycle.  Eight is past any plausible off-by-one in
   an open count and still cheap: only the first one starts a stack. */
/*
 * Four, not eight. A base costs the calling task signal bits, one for its
 * event signal and, the first time a caller passes a timeout, one more for the
 * timer, out of the 32 a Task has. With the drill's own bits already spent,
 * eight nested bases each doing a timed WaitSelect() runs the Process out and
 * OpenLibrary() correctly refuses the sixth. That ceiling is real and is
 * recorded in docs/EXECRESOURCES.md; asserting past it tests the platform's
 * limit rather than this library's behaviour.
 */
#define NEST_OPENS      4

#define MAX_CYCLES      16
#define MAX_IFACES      4
#define MAX_SOCKETS     8

#define LIB_NAME        "bsdsocket.library"

#define P_AF_INET       2
#define P_SOCK_STREAM   1
#define P_SOCK_DGRAM    2

/* Enough for a sockaddr_in; getsockname() only has to answer, not be read. */
typedef struct ProbeAddr
{
    UBYTE   sin_len;
    UBYTE   sin_family;
    UWORD   sin_port;
    ULONG   sin_addr;
    UBYTE   sin_zero[8];
} ProbeAddr;

/* ------------------------------------------------------------- vectors ---- */

static LONG p_errno(struct Library *base)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            res __asm("d0");

    __asm __volatile ("jsr a6@(-162:W)"     /* Errno -0x0a2 */
                      : "=r" (res)
                      : "r" (a6)
                      : "d1", "a0", "a1", "cc", "memory");
    return res;
}

static LONG p_socket(struct Library *base, LONG domain, LONG type, LONG proto)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = domain;
    register LONG            d1  __asm("d1") = type;
    register LONG            d2  __asm("d2") = proto;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");

    __asm __volatile ("jsr a6@(-30:W)"      /* socket -0x01e */
                      : "=r" (res), "=r" (_clob_d1)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (d2)
                      : "a0", "a1", "cc", "memory");
    return res;
}

static LONG p_close(struct Library *base, LONG s)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register LONG            res __asm("d0");

    __asm __volatile ("jsr a6@(-120:W)"     /* CloseSocket -0x078 */
                      : "=r" (res)
                      : "r" (a6), "r" (d0)
                      : "d1", "a0", "a1", "cc", "memory");
    return res;
}

/* The liveness probe for a descriptor held across a bounce: it reads the
   socket without changing it, so the same descriptor can be asked again. */
static LONG p_getsockname(struct Library *base, LONG s, ProbeAddr *name,
                          LONG *len)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = (APTR)name;
    register APTR            a1  __asm("a1") = (APTR)len;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-102:W)"     /* getsockname -0x066 */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (a1)
                      : "cc", "memory");
    return res;
}

/*
 * WaitSelect(nfds, read, write, except, timeout, signals).
 *
 * Here for one reason: a timeout is what makes the library open timer.device
 * and AllocSignal() a bit out of THIS Process, lazily, per base (select.c).
 * A drill that never passes one never reaches that code, so the signal check
 * below had nothing to check, and the bit was leaked on every close.
 */
static LONG p_waitselect(struct Library *base, LONG nfds, ULONG *readfds,
                         struct timeval *tv)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = nfds;
    register APTR            a0  __asm("a0") = (APTR)readfds;
    register APTR            a1  __asm("a1") = NULL;
    register APTR            a2  __asm("a2") = NULL;
    register APTR            a3  __asm("a3") = (APTR)tv;
    register APTR            d1  __asm("d1") = NULL;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");
    register LONG _clob_a2 __asm("a2");
    register LONG _clob_a3 __asm("a3");

    __asm __volatile ("jsr a6@(-126:W)"     /* WaitSelect -0x07e */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1), "=r" (_clob_a2), "=r" (_clob_a3)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (a1), "r" (a2),
                        "r" (a3), "r" (d1)
                      : "cc", "memory");
    return res;
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

static LONG p_control(struct Library *base, ULONG op, NetStatusControl *ctl)
{
    register struct Library *a6  __asm("a6") = base;
    register ULONG           d0  __asm("d0") = AMI_NETSTATUS_MAGIC;
    register ULONG           d1  __asm("d1") = op;
    register APTR            a0  __asm("a0") = (APTR)ctl;
    register ULONG           d2  __asm("d2") = (ULONG)sizeof(*ctl);
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-876:W)"     /* AMI_NETSTATUS_CONTROL_LVO */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (a0), "r" (d2)
                      : "a1", "cc", "memory");
    return res;
}

static LONG p_add_interface(struct Library *base, const char *name,
                            const char *device, LONG unit,
                            struct TagItem *tags)
{
    register struct Library *a6  __asm("a6") = base;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)name;
    register CONST_APTR      a1  __asm("a1") = (CONST_APTR)device;
    register LONG            d0  __asm("d0") = unit;
    register APTR            a2  __asm("a2") = (APTR)tags;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");
    register LONG _clob_a2 __asm("a2");

    __asm __volatile ("jsr a6@(-444:W)"     /* AddInterfaceTagList -0x1bc */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1), "=r" (_clob_a2)
                      : "r" (a6), "r" (a0), "r" (a1), "r" (d0), "r" (a2)
                      : "cc", "memory");
    return res;
}

static LONG p_configure_interface(struct Library *base, const char *name,
                                  struct TagItem *tags)
{
    register struct Library *a6  __asm("a6") = base;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)name;
    register APTR            a1  __asm("a1") = (APTR)tags;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-450:W)"     /* ConfigureInterfaceTagList -0x1c2 */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1)
                      : "r" (a6), "r" (a0), "r" (a1)
                      : "cc", "memory");
    return res;
}

static LONG p_remove_interface(struct Library *base, const char *name,
                               LONG force)
{
    register struct Library *a6  __asm("a6") = base;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)name;
    register LONG            d0  __asm("d0") = force;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-732:W)"     /* RemoveInterface -0x2dc */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0), "r" (d0)
                      : "a1", "cc", "memory");
    return res;
}

/*
 * The LVOs above are written as literals because `jsr a6@(-870:W)` needs one,
 * so the header's own numbers are checked against them here.  A slot that
 * moved would otherwise land on whatever is next.
 */
_Static_assert(AMI_NETSTATUS_QUERY_LVO   == -870, "NetStackQuery LVO moved");
_Static_assert(AMI_NETSTATUS_CONTROL_LVO == -876, "NetStackControl LVO moved");

/* ------------------------------------------------------------- reporting -- */

static LONG checks;
static LONG failures;

/* Four longwords, because that is what every format below needs at most.
   VPrintf reads only as many as the string names. */
static VOID say(const char *fmt, LONG a, LONG b, LONG c, LONG d)
{
    LONG args[4];

    args[0] = a;
    args[1] = b;
    args[2] = c;
    args[3] = d;

    VPrintf((CONST_STRPTR)fmt, (APTR)args);     /* (APTR): see tool_util.c */
}

/* Every assertion goes through here, so the count printed at the end is the
   real one and cannot drift away from the FAILs in the transcript. */
static BOOL check(BOOL ok, const char *what, LONG a, LONG b)
{
    checks++;
    if (!ok)
        failures++;

    say(ok ? "  ok: %s (%ld, %ld)\n" : "FAIL: %s (%ld, %ld)\n",
        (LONG)what, a, b, 0);

    return ok;
}

/* -------------------------------------------------------------- sampling -- */

typedef struct Sample
{
    BOOL    ok;
    ULONG   alloc_live;
    ULONG   alloc_peak;
    ULONG   alloc_refused;
    ULONG   sockets;
    ULONG   sockets_peak;
    ULONG   opens;
    ULONG   pool_total;
    ULONG   pool_free;
    ULONG   pool_low;
    ULONG   pool_empty;
    ULONG   free_mem;       /* from Exec, not the library */
    ULONG   sigs;           /* signal bits held by this Process */
    ULONG   tasks;          /* Tasks and Processes on the system lists */
} Sample;

static struct
{
    NetStatusHeader hdr;
    NetStatusHealth e;
} q_health;

static struct
{
    NetStatusHeader     hdr;
    NetStatusInterface  e[MAX_IFACES];
} q_ifaces;

static VOID zero(APTR p, ULONG n)
{
    UBYTE *b = (UBYTE *)p;

    while (n-- != 0)
        *b++ = 0;
}

/* Signal bits allocated to the calling Process.  A library that AllocSignal()s
   on its opener and forgets to give it back moves this and nothing else. */
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

/*
 * Tasks and Processes on the system lists.  A Process left running after an
 * expunge is the worst of the failures this drill looks for, it is executing
 * out of a segment that has been handed back, and it is also the cheapest
 * explanation for a fixed amount of memory going missing every cycle, since a
 * Process is its control block plus its stack.
 *
 * Disable(), not Forbid().  Forbid() was here, on the premise that "the lists
 * are only walked" -- which is exactly the case it does not cover.  Forbid()
 * stops the scheduler; it does not stop interrupts, and Signal() called from
 * one takes a task off TaskWait and Enqueue()s it onto TaskReady there and
 * then.  A2065's receive interrupt and the timer do that continuously while
 * the stack is up, so a node can move from the list still to be walked into
 * the list already walked (missed) or the other way (counted twice), and this
 * loop reports a number that was never true of the machine at any instant.
 *
 * Measured, on a build with no leak in it: 20, 17, 20, 20 over four cycles,
 * with AvailMem and the library's own allocation count byte-identical
 * throughout.  Three of the stack's threads happened to be in flight at cycle
 * 1's sample.  The check downstream is `later <= earlier`, so an undercount
 * anywhere but the last sample fails the run, and it did.
 *
 * Interrupts stay off for a walk of about twenty nodes.  The running task is
 * on neither list, so the count is one short of the truth and consistently so.
 */
static ULONG count_tasks(VOID)
{
    struct Node *n;
    ULONG        total = 0;

    Disable();
    for (n = SysBase->TaskReady.lh_Head; n->ln_Succ != NULL; n = n->ln_Succ)
        total++;
    for (n = SysBase->TaskWait.lh_Head; n->ln_Succ != NULL; n = n->ln_Succ)
        total++;
    Enable();

    return total;
}

/*
 * `base` NULL means the library is not open right now: the Exec numbers are
 * still read, and they are the only ones that survive an expunge.
 *
 * Struct assignment rather than zero(): -fanalyzer does not follow a byte loop
 * through a caller's struct and reports every field the caller then reads as
 * uninitialised.
 */
static VOID sample(struct Library *base, Sample *out)
{
    static const Sample empty;

    *out = empty;

    out->free_mem = AvailMem(MEMF_ANY);
    out->sigs     = own_signals();
    out->tasks    = count_tasks();

    if (base == NULL)
        return;

    zero(&q_health, sizeof(q_health));
    q_health.hdr.nsh_Magic   = AMI_NETSTATUS_MAGIC;
    q_health.hdr.nsh_Version = AMI_NETSTATUS_VERSION;

    if (p_query(base, NETSTATUS_HEALTH, &q_health, sizeof(q_health)) <= 0)
        return;

    out->ok            = TRUE;
    out->alloc_live    = q_health.e.nsl_AllocLive;
    out->alloc_peak    = q_health.e.nsl_AllocPeak;
    out->alloc_refused = q_health.e.nsl_AllocRefused;
    out->sockets       = q_health.e.nsl_Sockets;
    out->sockets_peak  = q_health.e.nsl_SocketsPeak;
    out->opens         = q_health.e.nsl_Opens;
    out->pool_total    = q_health.e.nsl_PoolTotal;
    out->pool_free     = q_health.e.nsl_PoolFree;
    out->pool_low      = q_health.e.nsl_PoolLow;
    out->pool_empty    = q_health.e.nsl_PoolEmpty;
}

/* One fixed shape, so the shell can read any of these lines the same way. */
static VOID show(const char *label, LONG n, const Sample *s)
{
    say("%s %ld: alloc %lu peak %lu\n",
        (LONG)label, n, (LONG)s->alloc_live, (LONG)s->alloc_peak);
    say("           refused %lu sockets %lu peak %lu opens %lu\n",
        (LONG)s->alloc_refused, (LONG)s->sockets, (LONG)s->sockets_peak,
        (LONG)s->opens);
    say("           pool %lu of %lu low %lu empty %lu\n",
        (LONG)s->pool_free, (LONG)s->pool_total, (LONG)s->pool_low,
        (LONG)s->pool_empty);
    say("           free %lu sigs %lu tasks %lu\n",
        (LONG)s->free_mem, (LONG)s->sigs, (LONG)s->tasks, 0);
}

/* ------------------------------------------------------------ the library -- */

static struct Library *lib_find(VOID)
{
    struct Library *lib;

    Forbid();
    lib = (struct Library *)FindName(&SysBase->LibList, (STRPTR)LIB_NAME);
    Permit();

    return lib;
}

/*
 * Take TCP: down.  bsd_lib_expunge() declines while the handler process
 * exists, its code is in the segment expunge would hand back and it holds no
 * open count, so this is the documented way to make an expunge reachable.
 *
 * 1 the handler answered and went, 0 it refused (a session is running),
 * -1 there was no TCP: to begin with.
 */
/* Is there a TCP: device in the DOS list right now? */
static BOOL tcp_present(VOID)
{
    struct DosList *dl;
    BOOL            found;

    dl    = LockDosList(LDF_DEVICES | LDF_READ);
    found = (FindDosEntry(dl, (STRPTR)"TCP", LDF_DEVICES) != NULL);
    UnLockDosList(LDF_DEVICES | LDF_READ);

    return found;
}

static LONG tcp_die(VOID)
{
    struct DosList *dl;
    struct MsgPort *port = NULL;

    dl = LockDosList(LDF_DEVICES | LDF_READ);
    dl = FindDosEntry(dl, (STRPTR)"TCP", LDF_DEVICES);
    if (dl != NULL)
        port = dl->dol_Task;
    UnLockDosList(LDF_DEVICES | LDF_READ);

    if (port == NULL)
        return -1;

    return DoPkt(port, ACTION_DIE, 0, 0, 0, 0, 0) ? 1 : 0;
}

/*
 * Ask Exec to expunge the library, and say what it did.  *held is the open
 * count seen just before, so a decline can be told apart from a decline for
 * the reason the caller expected.
 *
 * No Forbid() around RemLibrary(): the expunge vector calls FreeMem() and
 * CloseLibrary(), so the Forbid would be broken inside it anyway, and holding
 * it would only make that harder to reason about.  Nothing else on this
 * machine opens bsdsocket.library while the drill runs.
 */
static BOOL lib_expunge(ULONG *held)
{
    struct Library *lib = lib_find();

    *held = 0;
    if (lib == NULL)
        return TRUE;            /* already gone */

    *held = lib->lib_OpenCnt;
    RemLibrary(lib);

    return (lib_find() == NULL) ? TRUE : FALSE;
}

/* --------------------------------------------------------- the interface -- */

typedef struct IfInfo
{
    BOOL    found;
    UWORD   index;
    BOOL    linkup;
    ULONG   address;
    ULONG   netmask;
    char    name[NETSTATUS_NAME_LEN];
    char    device[NETSTATUS_DEVICE_LEN];
    ULONG   unit;
} IfInfo;

/* Interface names are file names in DEVS:NetInterfaces, and AmigaDOS file
   names are case-insensitive, so this is how the library compares them. */
static BOOL same_name(const char *a, const char *b)
{
    ULONG i;

    for (i = 0; ; i++)
    {
        char ca = a[i];
        char cb = b[i];

        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + ('a' - 'A'));

        if (ca != cb)   return FALSE;
        if (ca == '\0') return TRUE;
    }
}

static VOID copy_str(char *dst, const char *src, ULONG len)
{
    ULONG i;

    for (i = 0; i + 1 < len && src[i] != '\0'; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* Re-read every time rather than cached: the index changes across a remove and
   re-add, which is exactly what this drill puts it through. */
static VOID if_look(struct Library *base, const char *want, IfInfo *out)
{
    static const IfInfo empty;      /* see the note in sample() */
    LONG n;
    LONG i;

    *out = empty;

    zero(&q_ifaces, sizeof(q_ifaces));
    q_ifaces.hdr.nsh_Magic   = AMI_NETSTATUS_MAGIC;
    q_ifaces.hdr.nsh_Version = AMI_NETSTATUS_VERSION;

    n = p_query(base, NETSTATUS_INTERFACES, &q_ifaces, sizeof(q_ifaces));
    if (n <= 0)
        return;

    for (i = 0; i < n; i++)
    {
        const NetStatusInterface *nsi = &q_ifaces.e[i];

        if (!(nsi->nsi_Flags & NETSTATUS_IF_NAMED))
            continue;
        if (!same_name(nsi->nsi_Name, want))
            continue;

        out->found   = TRUE;
        out->index   = nsi->nsi_Index;
        /* LINKUP, not ONLINE: NETCTRL_INTERFACE_UP/DOWN reach NX_LINK_ENABLE
           and NX_LINK_DISABLE, which is what LINKUP records.  The SANA-II
           shim's own online flag is a layer below and does not follow in
           step, the same note is in src/tools/onoff.c. */
        out->linkup  = (nsi->nsi_Flags & NETSTATUS_IF_LINKUP) ? TRUE : FALSE;
        out->address = nsi->nsi_Address;
        out->netmask = nsi->nsi_NetMask;
        out->unit    = nsi->nsi_Unit;
        copy_str(out->name, nsi->nsi_Name, sizeof(out->name));
        copy_str(out->device, nsi->nsi_Device, sizeof(out->device));
        return;
    }
}

static VOID dotted(ULONG addr, char *out)
{
    static const char digits[] = "0123456789";
    LONG  shift;
    ULONG pos = 0;

    for (shift = 24; shift >= 0; shift -= 8)
    {
        ULONG v = (addr >> shift) & 0xFFUL;

        if (v >= 100) out[pos++] = digits[v / 100];
        if (v >= 10)  out[pos++] = digits[(v / 10) % 10];
        out[pos++] = digits[v % 10];

        if (shift != 0)
            out[pos++] = '.';
    }

    out[pos] = '\0';
}

/* ----------------------------------------------------------- the phases --- */

static LONG did_opens;
static LONG did_bounces;
static LONG did_roundtrips;
static LONG did_expunges;

static struct Library *nest[NEST_OPENS];
static LONG            socks[MAX_SOCKETS];

/*
 * Nested opens.  Every one of them gets its own child base, opening a child
 * again redirects to the master (library.c), and nsl_Opens is the number
 * that says whether the library agrees about how many callers it has.
 */
/*
 * One WaitSelect() with a timeout on a freshly opened base, with nothing in the
 * sets, so it does nothing but expire. The point is the side effect: the base
 * opens timer.device and takes a signal bit out of this Process the first time
 * a caller passes a timeout, and CloseLibrary() has to give that bit back.
 */
static VOID timed_wait(struct Library *base)
{
    struct timeval tv;

    tv.tv_secs  = 0;
    tv.tv_micro = 1000;

    (VOID)p_waitselect(base, 0, NULL, &tv);
}

static VOID phase_opens(struct Library *anchor, ULONG opens_before)
{
    Sample   before;
    Sample   peak;
    Sample   after;
    LONG     i;
    LONG     got = 0;

    sample(anchor, &before);

    for (i = 0; i < NEST_OPENS; i++)
    {
        nest[i] = OpenLibrary((CONST_STRPTR)LIB_NAME, 4UL);
        if (nest[i] != NULL)
        {
            got++;
            did_opens++;
            timed_wait(nest[i]);
        }
    }

    sample(anchor, &peak);

    for (i = 0; i < NEST_OPENS; i++)
    {
        if (nest[i] != NULL)
        {
            CloseLibrary(nest[i]);
            nest[i] = NULL;
        }
    }

    sample(anchor, &after);

    check(got == NEST_OPENS, "every nested OpenLibrary succeeded",
          got, NEST_OPENS);

    /* Exact, not >=: an open that did not register would be invisible to any
       looser test. */
    check(peak.opens == opens_before + (ULONG)got,
          "nsl_Opens counted the nested opens",
          (LONG)peak.opens, (LONG)(opens_before + (ULONG)got));

    check(after.opens == opens_before,
          "nsl_Opens came back to where it started",
          (LONG)after.opens, (LONG)opens_before);

    /*
     * Exact, and inside one cycle rather than only in the drift table: each of
     * those bases was made to open timer.device by the WaitSelect() above, and
     * a bit not given back at CloseLibrary() is gone from this Process for
     * good. Eight per cycle empties the 32 in four.
     */
    check(after.sigs == before.sigs,
          "the nested opens gave every signal bit back",
          (LONG)before.sigs, (LONG)after.sigs);
}

/*
 * Online/Offline, through the same control op the Online and Offline commands
 * use.  Both directions are verified from NETSTATUS_INTERFACES rather than
 * from the return code: a control that answers 0 and changes nothing is the
 * failure worth catching.
 */
static BOOL phase_bounce(struct Library *base, const char *iface)
{
    NetStatusControl ctl;
    IfInfo           info;
    LONG             rc;
    BOOL             ok = TRUE;

    if_look(base, iface, &info);
    if (!check(info.found, "the interface is there before the bounce", 0, 0))
        return FALSE;

    zero(&ctl, sizeof(ctl));
    ctl.nsc_Magic   = AMI_NETSTATUS_MAGIC;
    ctl.nsc_Version = (UWORD)AMI_NETSTATUS_VERSION;
    ctl.nsc_Index   = info.index;

    rc = p_control(base, NETCTRL_INTERFACE_DOWN, &ctl);
    if_look(base, iface, &info);
    if (!check(rc == 0 && !info.linkup, "Offline took the link down",
               rc, (LONG)info.linkup))
        ok = FALSE;

    zero(&ctl, sizeof(ctl));
    ctl.nsc_Magic   = AMI_NETSTATUS_MAGIC;
    ctl.nsc_Version = (UWORD)AMI_NETSTATUS_VERSION;
    ctl.nsc_Index   = info.index;

    rc = p_control(base, NETCTRL_INTERFACE_UP, &ctl);
    if_look(base, iface, &info);
    if (!check(rc == 0 && info.linkup, "Online brought it back up",
               rc, (LONG)info.linkup))
        ok = FALSE;

    if (ok)
        did_bounces++;

    return ok;
}

/*
 * Descriptors held across the bounce, and asked afterwards whether they are
 * still there.  getsockname() rather than close(): a descriptor that only
 * proves itself by being closeable proves nothing about the cycle it just
 * lived through.
 */
static VOID phase_sockets_across_bounce(struct Library *base,
                                        const char *iface, LONG count)
{
    ProbeAddr sa;
    LONG      len;
    Sample    before;
    Sample    after;
    LONG      i;
    LONG      opened = 0;
    LONG      alive  = 0;

    sample(base, &before);

    for (i = 0; i < count; i++)
    {
        socks[i] = p_socket(base, P_AF_INET,
                            (i & 1) ? P_SOCK_STREAM : P_SOCK_DGRAM, 0);
        if (socks[i] >= 0)
            opened++;
    }

    check(opened == count, "the sockets opened", opened, count);

    (VOID)phase_bounce(base, iface);

    for (i = 0; i < count; i++)
    {
        if (socks[i] < 0)
            continue;

        zero(&sa, sizeof(sa));
        len = (LONG)sizeof(sa);
        if (p_getsockname(base, socks[i], &sa, &len) == 0)
            alive++;
    }

    check(alive == opened, "every socket survived the bounce", alive, opened);

    for (i = 0; i < count; i++)
    {
        if (socks[i] >= 0)
            (VOID)p_close(base, socks[i]);
        socks[i] = -1;
    }

    sample(base, &after);

    check(after.sockets == before.sockets,
          "the socket count came back to where it started",
          (LONG)after.sockets, (LONG)before.sockets);
}

/*
 * Remove and re-add the interface this run is riding on, with sockets open
 * over the top of it.
 *
 * AddInterfaceTagList() has no address tag, so what comes back is BARE and the
 * ConfigureInterfaceTagList() below is what addresses it.  The address and the
 * mask put back are the ones read off the interface before it was removed, so
 * this restores rather than invents.  tests/tools/run-ifreadd.sh is the
 * regression test for that pair; here it is only the means of getting the
 * interface back for the next cycle.
 */
static VOID phase_addremove(struct Library *base, const char *iface,
                            LONG count)
{
    struct TagItem tags[3];
    IfInfo         before;
    IfInfo         after;
    char           addr_text[16];
    char           mask_text[16];
    LONG           rc;
    LONG           i;
    LONG           opened = 0;

    if_look(base, iface, &before);
    if (!check(before.found, "the interface is there before the round trip",
               0, 0))
        return;

    for (i = 0; i < count; i++)
    {
        socks[i] = p_socket(base, P_AF_INET, P_SOCK_DGRAM, 0);
        if (socks[i] >= 0)
            opened++;
    }
    check(opened == count, "sockets open over the round trip", opened, count);

    rc = p_remove_interface(base, iface, 0);
    if_look(base, iface, &after);
    check(rc != 0 && !after.found, "RemoveInterface took it away",
          rc, (LONG)after.found);

    tags[0].ti_Tag  = IFA_NumReadRequests;
    tags[0].ti_Data = 16;
    tags[1].ti_Tag  = TAG_DONE;
    tags[1].ti_Data = 0;

    rc = p_add_interface(base, iface, before.device, (LONG)before.unit, tags);
    check(rc == 0, "AddInterfaceTagList put it back", rc, p_errno(base));

    if_look(base, iface, &after);
    check(after.found && after.address == 0,
          "and it came back bare, as the published API says",
          (LONG)after.found, (LONG)after.address);

    dotted(before.address, addr_text);
    dotted(before.netmask, mask_text);

    tags[0].ti_Tag  = IFC_Address;
    tags[0].ti_Data = (ULONG)addr_text;
    tags[1].ti_Tag  = IFC_NetMask;
    tags[1].ti_Data = (ULONG)mask_text;
    tags[2].ti_Tag  = TAG_DONE;
    tags[2].ti_Data = 0;

    rc = p_configure_interface(base, iface, tags);
    if_look(base, iface, &after);
    check(rc == 0 && after.address == before.address &&
          after.netmask == before.netmask,
          "the configure restored the address and the mask",
          (LONG)after.address, (LONG)after.netmask);

    for (i = 0; i < count; i++)
    {
        if (socks[i] >= 0)
            (VOID)p_close(base, socks[i]);
        socks[i] = -1;
    }

    if (after.found)
        did_roundtrips++;
}

/*
 * RemLibrary() while this program still holds the library open.  Three things
 * have to be true afterwards, and only the first is obvious: the library is
 * still in the list, LIBF_DELEXP is set so the last close will retry, and the
 * base we are holding still answers a vector, which is the one that says the
 * segment was not handed back under a live caller.
 */
static VOID phase_guarded_expunge(struct Library *anchor)
{
    struct Library *lib;
    struct Library *tmp;
    Sample          s;
    ULONG           held = 0;
    ULONG           count_before;
    BOOL            gone;

    lib = lib_find();
    if (!check(lib != NULL, "the library is in the system list", 0, 0))
        return;

    count_before = lib->lib_OpenCnt;
    check(count_before > 0, "and this program is holding it open",
          (LONG)count_before, 0);

    gone = lib_expunge(&held);
    lib  = lib_find();

    check(!gone && lib != NULL,
          "RemLibrary declined to expunge a library with an opener",
          (LONG)gone, (LONG)held);

    check(lib != NULL && (lib->lib_Flags & LIBF_DELEXP) != 0,
          "and set LIBF_DELEXP so the last close retries",
          (LONG)(lib ? lib->lib_Flags : 0), 0);

    check(lib != NULL && lib->lib_OpenCnt == count_before,
          "the open count is untouched",
          (LONG)(lib ? lib->lib_OpenCnt : 0), (LONG)count_before);

    /* The proof that the segment is still ours to call. */
    (VOID)p_errno(anchor);
    sample(anchor, &s);
    check(s.ok, "and the base we hold still answers NetStackQuery", 0, 0);

    /*
     * Clear LIBF_DELEXP again by opening once: bsd_lib_open() clears it, and
     * closing with another opener left does not set it.  Without this the flag
     * survives into whatever runs after this command, and the next last close
     * would attempt an expunge nobody asked for.
     */
    tmp = OpenLibrary((CONST_STRPTR)LIB_NAME, 4UL);
    if (tmp != NULL)
        CloseLibrary(tmp);

    lib = lib_find();
    check(lib != NULL && (lib->lib_Flags & LIBF_DELEXP) == 0,
          "an open cleared LIBF_DELEXP again",
          (LONG)(lib ? lib->lib_Flags : 0), 0);
}

/*
 * One full expunge and reopen.  Nothing else may hold the library: this runs
 * before AddNetInterface has ever been called on the machine, which is the
 * only state in which lib_OpenCnt can reach zero.
 */
static VOID phase_expunge_cycle(LONG n, const char *iface, Sample *at_open,
                                Sample *at_gone)
{
    struct Library *base;
    IfInfo          info;
    ULONG           held = 0;
    LONG            die;
    BOOL            gone;

    base = OpenLibrary((CONST_STRPTR)LIB_NAME, 4UL);
    if (!check(base != NULL, "the library opened", n, 0))
        return;

    did_opens++;

    /* The stack comes up on the first open, interfaces and all, so this is
       also the assertion that a reopened library is a working one and not a
       shell that answers vectors. */
    if_look(base, iface, &info);
    check(info.found, "the stack came up with the interface", n,
          (LONG)info.linkup);

    timed_wait(base);

    sample(base, at_open);

    CloseLibrary(base);

    /*
     * Last close: netstack_shutdown() has run and the SANA-II readers have
     * been stopped.  This is where a driver that ignores AbortIO() orphans a
     * reader thread, the serial log carries the message, and the free memory
     * sampled below carries the cost.
     */

    die  = tcp_die();
    gone = lib_expunge(&held);

    /*
     * A build without AMINETXDUO_TCPDEVICE has no TCP: to take down, and
     * tcp_die() says so with -1.  That is not a weaker drill: the expunge has
     * to succeed with no ACTION_DIE at all, and the handler process that
     * bsd_lib_expunge() used to decline over is the thing that is gone.
     */
    if (die < 0)
        check(!tcp_present(), "no TCP: on this build, so none to take down",
              die, 0);
    else
        check(die >= 0, "TCP: answered ACTION_DIE", die, 0);
    check(gone, "the library expunged after its last close", (LONG)held, die);

    if (gone)
        did_expunges++;

    sample(NULL, at_gone);
}

/*
 * TCP: comes back for the NEXT opener, after an ACTION_DIE that did not
 * expunge.
 *
 * tcp_started was set on the first bring-up and never cleared, so once TCP:
 * had been taken down every OpenLibrary() afterwards got a library that
 * answered every vector and had no TCP: device, until the last close let the
 * segment go and the static reset itself.
 *
 * Which is why this phase holds a base open across the ACTION_DIE.  The
 * expunge cycle above cannot see the defect: it takes TCP: down with nothing
 * else holding the library, so the reload does the clearing that the code
 * failed to do, and a broken build passes.
 */
static VOID phase_tcp_restart(VOID)
{
    struct Library *holder;
    struct Library *next;
    LONG            died;

    holder = OpenLibrary((CONST_STRPTR)LIB_NAME, 4UL);
    if (!check(holder != NULL, "the library opened for the TCP: restart", 0, 0))
        return;

    /*
     * The whole phase is about a static that TCP: sets, so a build without
     * AMINETXDUO_TCPDEVICE has nothing here to get wrong.  Skipped rather than
     * failed, and said out loud so a transcript cannot be mistaken for one
     * that ran it.
     */
    if (!tcp_present())
    {
        say("  --: no TCP: device; the restart phase does not apply\n",
            0, 0, 0, 0);
        CloseLibrary(holder);
        return;
    }

    check(tcp_present(), "TCP: is there before ACTION_DIE", 0, 0);

    died = tcp_die();
    check(died > 0, "TCP: answered the restart ACTION_DIE", died, 0);
    check(!tcp_present(), "TCP: went away when told to", 0, 0);

    /* The opener the defect was about.  holder is still open, so nothing has
       been unloaded and nothing has been reinitialised. */
    next = OpenLibrary((CONST_STRPTR)LIB_NAME, 4UL);
    if (check(next != NULL, "the library opened again with TCP: down", 0, 0))
    {
        check(tcp_present(),
              "TCP: is back for an opener that arrived after ACTION_DIE", 0, 0);
        CloseLibrary(next);
    }

    CloseLibrary(holder);
}

/* ------------------------------------------------------------------ main -- */

static Sample baseline;
static Sample cyc[MAX_CYCLES];
static Sample exp_open[MAX_CYCLES];
static Sample exp_gone[MAX_CYCLES];
static Sample cold_open[MAX_CYCLES];
static Sample cold_shut[MAX_CYCLES];

int main(VOID)
{
    struct RDArgs  *rda;
    LONG            args[ARG_COUNT];
    struct Library *anchor;
    struct Library *lib;
    const char     *iface   = DEF_IFACE;
    LONG            cycles  = DEF_CYCLES;
    LONG            expunge = DEF_EXPUNGE;
    LONG            nsocks  = DEF_SOCKETS;
    LONG            i;

    zero(args, sizeof(args));
    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        PrintFault(IoErr(), (CONST_STRPTR)"CycleDrill");
        return RETURN_FAIL;
    }

    if (args[ARG_CYCLES])  cycles  = *(LONG *)args[ARG_CYCLES];
    if (args[ARG_EXPUNGE]) expunge = *(LONG *)args[ARG_EXPUNGE];
    if (args[ARG_SOCKETS]) nsocks  = *(LONG *)args[ARG_SOCKETS];
    if (args[ARG_IFACE])   iface   = (const char *)args[ARG_IFACE];

    if (cycles  < 1)           cycles  = 1;
    if (expunge < 0)           expunge = 0;
    if (nsocks  < 1)           nsocks  = 1;
    if (cycles  > MAX_CYCLES)  cycles  = MAX_CYCLES;
    if (expunge > MAX_CYCLES)  expunge = MAX_CYCLES;
    if (nsocks  > MAX_SOCKETS) nsocks  = MAX_SOCKETS;

    for (i = 0; i < MAX_SOCKETS; i++)
        socks[i] = -1;

    say("CycleDrill: %ld cycles, %ld expunges, %ld sockets, iface %s\n",
        cycles, expunge, nsocks, (LONG)iface);

    /* ---- phase E: expunge and reopen, cold -------------------------------
     *
     * First, because this is the only point in the machine's life at which
     * lib_OpenCnt can reach zero.
     */
    say("\n-- expunge/reopen --\n", 0, 0, 0, 0);

    lib = lib_find();
    check(lib == NULL || lib->lib_OpenCnt == 0,
          "nothing else holds the library yet",
          (LONG)(lib ? lib->lib_OpenCnt : 0), 0);

    for (i = 0; i < expunge; i++)
    {
        phase_expunge_cycle(i + 1, iface, &exp_open[i], &exp_gone[i]);
        show("open ", i + 1, &exp_open[i]);
        show("gone ", i + 1, &exp_gone[i]);
    }

    /* After the expunge phase, so it starts from a fully unloaded library and
       brings its own up. */
    phase_tcp_restart();

    /* ---- phase L: cold open/close, no expunge -----------------------------
     *
     * The split phase E cannot make.  Every iteration takes the stack all the
     * way down (last close, sb_StackRefs 0, netstack_shutdown()) and back up
     * again, but the library is never expunged and its segment is never
     * unloaded.  Phase C's nested pairs run under the anchor, so they never
     * reach a last close and say nothing about the teardown; this does.
     */
    say("\n-- cold open/close --\n", 0, 0, 0, 0);

    for (i = 0; i < expunge; i++)
    {
        struct Library *cold = OpenLibrary((CONST_STRPTR)LIB_NAME, 4UL);

        if (!check(cold != NULL, "the library opened cold", i + 1, 0))
            break;

        did_opens++;
        timed_wait(cold);
        sample(cold, &cold_open[i]);
        CloseLibrary(cold);
        sample(NULL, &cold_shut[i]);

        show("cold ", i + 1, &cold_open[i]);
        show("shut ", i + 1, &cold_shut[i]);
    }

    /* ---- phase C: cycling under one held reference ------------------------
     *
     * The anchor keeps the stack up so the library's own counters accumulate;
     * after an expunge they are all zero again and drift cannot be seen.
     */
    say("\n-- cycling --\n", 0, 0, 0, 0);

    anchor = OpenLibrary((CONST_STRPTR)LIB_NAME, 4UL);
    if (!check(anchor != NULL, "the library opened for the cycling phase",
               0, 0))
    {
        FreeArgs(rda);
        say("\nCycleDrill: %ld check(s), %ld failed\n", checks, failures, 0, 0);
        return RETURN_FAIL;
    }
    did_opens++;

    {
        char   addr_text[16];
        IfInfo info;

        sample(anchor, &baseline);
        if_look(anchor, iface, &info);
        dotted(info.address, addr_text);

        check(info.found, "the interface is up before cycling starts", 0, 0);
        say("interface %s: index %ld, %s, address %s\n",
            (LONG)iface, (LONG)info.index,
            (LONG)(info.linkup ? "up" : "DOWN"), (LONG)addr_text);

        show("cycle", 0, &baseline);
    }

    for (i = 0; i < cycles; i++)
    {
        phase_opens(anchor, baseline.opens);
        (VOID)phase_bounce(anchor, iface);
        phase_sockets_across_bounce(anchor, iface, nsocks);
        phase_addremove(anchor, iface, nsocks);

        sample(anchor, &cyc[i]);
        show("cycle", i + 1, &cyc[i]);
    }

    /* ---- phase G: expunge refused while we hold it ------------------------ */
    say("\n-- expunge with an opener --\n", 0, 0, 0, 0);
    phase_guarded_expunge(anchor);

    /* ---- the verdict ------------------------------------------------------ */
    say("\n-- drift --\n", 0, 0, 0, 0);

    if (cycles >= 2)
    {
        const Sample *a = &cyc[0];
        const Sample *b = &cyc[cycles - 1];

        say("alloc_live %lu to %lu, sockets %lu to %lu\n",
            (LONG)a->alloc_live, (LONG)b->alloc_live,
            (LONG)a->sockets, (LONG)b->sockets);
        say("pool_free %lu to %lu, free %lu to %lu\n",
            (LONG)a->pool_free, (LONG)b->pool_free,
            (LONG)a->free_mem, (LONG)b->free_mem);
        say("sigs %lu to %lu, tasks %lu to %lu\n",
            (LONG)a->sigs, (LONG)b->sigs,
            (LONG)a->tasks, (LONG)b->tasks);

        check(b->alloc_live <= a->alloc_live,
              "allocations outstanding did not grow over the cycles",
              (LONG)a->alloc_live, (LONG)b->alloc_live);
        check(b->sockets <= a->sockets,
              "sockets alive did not grow over the cycles",
              (LONG)a->sockets, (LONG)b->sockets);
        check(b->pool_free >= a->pool_free,
              "packet buffers free did not shrink over the cycles",
              (LONG)a->pool_free, (LONG)b->pool_free);
        check(b->sigs <= a->sigs,
              "no signal bits were leaked onto this Process",
              (LONG)a->sigs, (LONG)b->sigs);
        check(b->tasks <= a->tasks,
              "no Task or Process was left behind over the cycles",
              (LONG)a->tasks, (LONG)b->tasks);
    }
    else
    {
        say("(one cycle: nothing to compare against)\n", 0, 0, 0, 0);
    }

    if (expunge >= 2)
    {
        /*
         * The same instant of the first and the last expunge cycle, divided by
         * the number of intervals between them.  Two choices in that sentence
         * are the difference between a figure and a coin toss:
         *
         *   the FIRST and the LAST, not the last adjacent pair, because one
         *   pair is one sample, measured spread over five cycles was 12,568,
         *   12,616, 12,592 and 21,104 bytes, so the last pair alone answers
         *   anything from 12 KB to 21 KB;
         *
         *   the sample taken while the library is OPEN, not the one taken
         *   after it has gone.  The post-expunge instant is a few
         *   milliseconds after ACTION_DIE and a Process may still be on its
         *   way out; the open one is taken with the stack fully up and is
         *   quiet.  The 21,104 above is that, and only that.
         *
         * Anything paid once, the packet pool, whatever RemLibrary() does
         * with the seglist, is in both samples and cancels.
         */
        LONG last  = expunge - 1;
        LONG total = (LONG)exp_open[0].free_mem -
                     (LONG)exp_open[last].free_mem;
        LONG per   = total / last;

        say("free at expunge 1: %lu, at %ld: %lu over %ld cycles\n",
            (LONG)exp_open[0].free_mem, last + 1,
            (LONG)exp_open[last].free_mem, last);
        /* The line run-cycledrill.sh reads.  Named, and on its own, so the
           budget lives in the shell where it can be raised for a soak rather
           than being compiled in here. */
        say("expunge leak: %ld bytes per cycle\n", per, 0, 0, 0);

        check(exp_open[last].tasks <= exp_open[0].tasks,
              "no Process was left behind by an expunge",
              (LONG)exp_open[0].tasks, (LONG)exp_open[last].tasks);

        /*
         * The same figure for phase L.  Reported next to the expunge one
         * because the pair is what attributes a loss: equal means the cost is
         * in the teardown a last close already does, and a loss here with none
         * there means it is the unload and reload.  The 12,612 bytes an
         * expunge used to lose read zero on this line, which is what said the
         * orphan was the library's statics and not the stack.
         */
        say("free at cold 1: %lu, at %ld: %lu\n",
            (LONG)cold_shut[0].free_mem, last + 1,
            (LONG)cold_shut[last].free_mem, 0);
        say("cold leak: %ld bytes per cycle\n",
            ((LONG)cold_shut[0].free_mem -
             (LONG)cold_shut[last].free_mem) / last, 0, 0, 0);

        check(cold_shut[last].sigs <= cold_shut[0].sigs,
              "no signal bit was leaked by a cold open/close",
              (LONG)cold_shut[0].sigs, (LONG)cold_shut[last].sigs);
    }

    /* ---- what actually happened ------------------------------------------- */
    say("\ndid: %ld opens, %ld bounces, %ld round trips, %ld expunges\n",
        did_opens, did_bounces, did_roundtrips, did_expunges);

    /*
     * The anchor is deliberately left open, the same way AddNetInterface leaks
     * one: it keeps this stack, the one every number above is about, up
     * for the `netstat -h` and `ShowNetStatus MEMORY` that follow in the
     * command list.  Closing it here would tear the stack down and leave those
     * two reporting on a fresh one.
     */
    say("the stack is left running on a held reference\n", 0, 0, 0, 0);

    FreeArgs(rda);

    say("\nCycleDrill: %ld check(s), %ld failed\n", checks, failures, 0, 0);

    return (failures == 0) ? RETURN_OK : RETURN_FAIL;
}
