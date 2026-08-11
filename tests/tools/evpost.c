/*
 * EvPost, does a socket event signal a Task that has exited?
 *
 * src/bsdsocket/select.c's bsd_event_post() ends in Signal(base->sb_Task,
 * signals) and guards nothing but sb_Task == NULL.  It runs from NetX Duo's
 * receive, transmit and out-of-band callbacks, which is to say on every
 * packet.  sb_Task is the Task that opened the base, and nothing in AmigaOS
 * or in this library takes a base down when its owner exits: a program that
 * opens bsdsocket.library, makes a socket and exits without CloseLibrary()
 * leaves the base on the master's child list for good, pointing at a struct
 * Task Exec has since freed.
 *
 * This asks whether that write actually happens, and it answers by owning the
 * memory it is asking about.  Arguing from the source is not enough: if the
 * block were still allocated to something, or the socket were torn down with
 * its owner, the read would prove nothing either way.
 *
 * THE CONSTRUCTION
 *
 *   1. A child Process opens the library, asks for SBTC_SIGIOMASK with a
 *      distinctive mask, binds a UDP socket, and returns without closing
 *      either the socket or the library.
 *   2. This program waits until that Task is on none of Exec's lists --
 *      ThisTask, TaskReady, TaskWait -- which is the whole set for a live
 *      task, so it is gone.
 *   3. AllocAbs() the range the Task occupied.  AllocAbs() succeeds only on
 *      memory that is FREE, so success is itself the proof that the block was
 *      handed back, and from then on the range belongs to this program and
 *      nothing else can be blamed for what appears in it.
 *   4. Zero it.  Wait, and read it again with no packet sent: the quiet
 *      control.  A non-zero reading there would mean the measurement is noise.
 *   5. Send one UDP datagram to the port the dead child bound, and read the
 *      same longword back.
 *
 * If tc_SigRecvd comes back holding the mask the dead child asked for, then
 * Signal() performed a read-modify-write on freed memory, and on a real
 * machine that memory belongs to whoever got it next.
 *
 * THE LIVE CONTROL, WHICH IS THE HALF THAT MAKES A NEGATIVE READABLE
 *
 *   A second child does exactly what the first does but STAYS ALIVE and
 *   closes everything properly.  It binds the next port up and reports
 *   whether its own signal arrived.  Both children are stimulated by the same
 *   datagram burst through the same code path, so:
 *
 *     control got its signal, dead block written    -> reachable
 *     control got its signal, dead block untouched  -> the pointer was
 *                                                      latched; a live
 *                                                      client is unaffected
 *     control got nothing                           -> the stimulus never
 *                                                      arrived, and the run
 *                                                      says nothing at all
 *
 *   Without it, a build that had merely stopped delivering datagrams would
 *   read as a clean bill of health.
 *
 *   That child blocks in WaitSelect(), which is the shape that would break:
 *   WaitSelect() Waits on sb_EventSigMask and the only thing that sets it is
 *   the Signal() in bsd_event_post().  It is then poked once a round for
 *   SOAK_ROUNDS rounds -- a minute of guest time, so rather more than sixty
 *   sweeps -- and every round has to produce a wakeup.  A sweeper that clears
 *   sb_Task on a task that is ALIVE would turn those into timeouts, silently,
 *   for the rest of the session.  That is a worse failure than the defect and
 *   this is the check for it.
 *
 * THE WINDOW A ONCE-A-SECOND SWEEP CANNOT CLOSE
 *
 *   A third child repeats the orphan exactly, but is stimulated as fast as the
 *   probe can manage after its Task goes, with none of the grace the first was
 *   given.  On a swept build `fast_uaf=yes` is the expected reading and is not
 *   a failure: it is the size of the hole that remains, and it is also what
 *   proves a datagram still reaches an orphan's socket on that build -- so the
 *   headline `uaf=no` cannot be explained away as a packet that never arrived.
 *
 * THE TRIGGER NEEDS NO PEER.  The datagrams go to 127.0.0.1 first; the stack
 * has a loopback interface.  If that does not arrive the probe falls back to
 * a broadcast, which this build loops back
 * (NX_ENABLE_IP_BROADCAST_LOOPBACK).
 *
 * WHAT IT LEAVES BEHIND, ON PURPOSE
 *
 *   The claimed range is never freed and the orphaned base is never closed.
 *   Freeing the range would hand the dead base's Signal() target to the next
 *   allocation, which is the very thing being demonstrated.  This probe is
 *   therefore not a leak test and must not be run inside one.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/tasks.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dostags.h>

#include <proto/exec.h>
#include <proto/dos.h>

static const char version_tag[] __attribute__((used)) =
    "$VER: EvPost 1.0 (10.8.2026)";

/* bsdsocket.library LVOs, from src/bsdsocket/bsdsocket_vectors.c. */
#define LVO_socket              (-30)       /* -0x01e */
#define LVO_bind                (-36)       /* -0x024 */
#define LVO_sendto              (-60)       /* -0x03c */
#define LVO_setsockopt          (-90)       /* -0x05a */
#define LVO_CloseSocket        (-120)       /* -0x078 */
#define LVO_SocketBaseTagList  (-294)       /* -0x126 */

#define AF_INET_L               2
#define SOCK_DGRAM_L            2
#define SOL_SOCKET_L            0xFFFF
#define SO_BROADCAST_L          0x0020

/*
 * SBTM_SETVAL(SBTC_SIGIOMASK), spelled out so this file needs no amitcp
 * headers: TAG_USER | (code << SBTB_CODE) | SBTF_SET, with SBTC_SIGIOMASK 2,
 * SBTB_CODE 1 and SBTF_SET 1 (amitcp/socketbasetags.h).
 */
#define SBTM_SETVAL_SIGIOMASK   0x80000005UL

/*
 * The masks the two children ask to be signalled with, and they have to be
 * disjoint from the ones the library allocates for itself or a hit proves
 * nothing.  AllocSignal(-1) fills from bit 31 DOWNWARDS and a child base
 * allocates exactly one, so sb_EventSigMask is at the top of the word: an
 * observed reading of 8F000000 is bit 31 (the base's own) plus the four
 * asked for here.  Eight allocations in one task would be needed to reach
 * bit 27, and these children make one each.
 *
 * Neither mask overlaps SIGF_DOS, SIGF_SINGLE or SIGBREAKF_CTRL_C, so Delay()
 * in the control child cannot manufacture one.
 */
#define MARK_MASK               0x0F000000UL    /* the orphan's  */
#define CTRL_MASK               0x30000000UL    /* the live control's */

/* The first orphan binds this, the live control the next one up, and the
   second orphan -- the one used to measure the window a once-a-second sweep
   cannot close -- the one after that. */
#define EVPOST_PORT             2308
#define CONTROL_PORT            2309
#define FAST_PORT               2310

/* Ceilings, in Delay() ticks of 1/50 s.  Reaching one is reported, not
   waited out: a probe that hangs proves nothing. */
#define DEATH_TICKS             500         /* 10 s */
#define SETTLE_TICKS            25          /* 0.5 s */

/*
 * The soak.  120 rounds half a second apart is a minute of guest time, so the
 * live client is poked across rather more than sixty sweeps and every one of
 * them has to have decided correctly that it is alive.  A shorter run would
 * not distinguish "the sweeper is right" from "the sweeper has not run yet".
 */
#define SOAK_ROUNDS             120
#define SOAK_ROUND_TICKS        25          /* 0.5 s between rounds */
#define SOAK_WAIT_TICKS         50          /* 1 s to answer one round */

struct sockaddr_in_local
{
    UBYTE   sin_len;
    UBYTE   sin_family;
    UWORD   sin_port;
    ULONG   sin_addr;
    UBYTE   sin_zero[8];
};

/* ------------------------------------------------------------- reporting -- */

static LONG checks;
static LONG failures;

static VOID say(const char *fmt, LONG a, LONG b)
{
    LONG args[2];

    args[0] = a;
    args[1] = b;

    VPrintf((CONST_STRPTR)fmt, (APTR)args);
    Flush(Output());
}

static BOOL check(BOOL ok, const char *what, LONG a)
{
    checks++;
    if (!ok)
        failures++;

    say(ok ? "  ok: %s (%ld)\n" : "FAIL: %s (%ld)\n", (LONG)what, a);

    return ok;
}

/* ----------------------------------------------------------- the vectors -- */
/*
 * The base is a parameter, not a global: the two children and this program
 * hold DIFFERENT bases, and the whole point of the probe is which one a
 * signal is charged to.
 */

static LONG call_socket(struct Library *base, LONG domain, LONG type, LONG proto)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = domain;
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

static LONG call_bind(struct Library *base, LONG s,
                      struct sockaddr_in_local *sa, LONG len)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = sa;
    register LONG            d1  __asm("d1") = len;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-36:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1)
                      : "a1", "d2", "cc", "memory");
    return res;
}

static LONG call_sendto(struct Library *base, LONG s, APTR buf, LONG len,
                        LONG flags, struct sockaddr_in_local *to, LONG tolen)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = buf;
    register LONG            d1  __asm("d1") = len;
    register LONG            d2  __asm("d2") = flags;
    register APTR            a1  __asm("a1") = to;
    register LONG            d3  __asm("d3") = tolen;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-60:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1), "r" (d2),
                        "r" (a1), "r" (d3)
                      : "cc", "memory");
    return res;
}

static LONG call_setsockopt(struct Library *base, LONG s, LONG level,
                            LONG optname, APTR optval, LONG optlen)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register LONG            d1  __asm("d1") = level;
    register LONG            d2  __asm("d2") = optname;
    register APTR            a0  __asm("a0") = optval;
    register LONG            d3  __asm("d3") = optlen;
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

static LONG call_closesocket(struct Library *base, LONG s)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");

    __asm __volatile ("jsr a6@(-120:W)"
                      : "=r" (res), "=r" (_clob_d1)
                      : "r" (a6), "r" (d0)
                      : "a0", "a1", "d2", "cc", "memory");
    return res;
}

static LONG call_recvfrom(struct Library *base, LONG s, APTR buf, LONG len,
                          LONG flags, APTR addr, APTR addrlen)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = buf;
    register LONG            d1  __asm("d1") = len;
    register LONG            d2  __asm("d2") = flags;
    register APTR            a1  __asm("a1") = addr;
    register APTR            a2  __asm("a2") = addrlen;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-72:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1), "r" (d2),
                        "r" (a1), "r" (a2)
                      : "cc", "memory");
    return res;
}

struct TimeVal_local
{
    ULONG   tv_secs;
    ULONG   tv_micro;
};

static LONG call_waitselect(struct Library *base, LONG nfds, ULONG *readfds,
                            struct TimeVal_local *tv, ULONG *sigs)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = nfds;
    register APTR            a0  __asm("a0") = readfds;
    register APTR            a1  __asm("a1") = NULL;
    register APTR            a2  __asm("a2") = NULL;
    register APTR            a3  __asm("a3") = tv;
    register APTR            d1  __asm("d1") = sigs;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-126:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (a1), "r" (a2),
                        "r" (a3), "r" (d1)
                      : "d2", "d3", "cc", "memory");
    return res;
}

static LONG call_sbtaglist(struct Library *base, struct TagItem *tags)
{
    register struct Library *a6  __asm("a6") = base;
    register APTR            a0  __asm("a0") = tags;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-294:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0)
                      : "a1", "d2", "d3", "cc", "memory");
    return res;
}

/* ------------------------------------------------------------- liveness -- */

/*
 * Is this pointer still one of Exec's tasks?  The running one plus the two
 * scheduler lists is the whole set, which is the same test
 * src/bsdsocket/library.c's bsd_task_alive() makes.
 *
 * Disable() and not Forbid(): an interrupt moves a task between TaskWait and
 * TaskReady, so Forbid() does not make the walk safe.  Nothing here
 * dereferences the candidate -- the answer comes from the lists, not from the
 * block, which matters because the block may already be somebody else's.
 */
static BOOL on_list(struct List *list, struct Task *task)
{
    struct Node *node;

    for (node = list->lh_Head; node->ln_Succ != NULL; node = node->ln_Succ)
    {
        if ((struct Task *)node == task)
            return TRUE;
    }

    return FALSE;
}

static BOOL task_alive(struct Task *task)
{
    BOOL alive;

    if (task == NULL)
        return FALSE;

    Disable();
    alive = (SysBase->ThisTask == task ||
             on_list(&SysBase->TaskReady, task) ||
             on_list(&SysBase->TaskWait, task));
    Enable();

    return alive;
}

/* ------------------------------------------------------------ the cost -- */

/*
 * How many nodes a sweep has to walk, and how long one walk takes on this
 * machine.  Both are read here rather than asserted: the sweeper does exactly
 * this scan, once a second, once per child base, so these two numbers are the
 * whole price of the fix and they belong in the transcript of the machine
 * that paid it rather than in an estimate.
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

/* Monotonic within the day, in 1/50 s ticks. */
static ULONG ticks_now(VOID)
{
    struct DateStamp ds;

    DateStamp(&ds);

    return (ULONG)ds.ds_Minute * 3000UL + (ULONG)ds.ds_Tick;
}

/*
 * Microseconds for ONE liveness scan of a task that is on neither list, which
 * is the worst case and the only case that matters: a live owner is usually
 * found early, a dead one is never found at all and costs the full walk.
 *
 * Timed in batches of 100 so the DateStamp() calls are about one per cent of
 * the measurement rather than all of it.
 */
#define COST_BATCH      100
#define COST_TICKS      100         /* 2 s */

static ULONG scan_micros(VOID)
{
    struct Task *bogus = (struct Task *)0x00000002UL;  /* on no list, ever */
    ULONG        start;
    ULONG        n = 0;
    LONG         i;

    /* Start on a tick edge, so the first partial tick is not counted. */
    start = ticks_now();
    while (ticks_now() == start)
        ;

    start = ticks_now();
    while ((ticks_now() - start) < COST_TICKS)
    {
        for (i = 0; i < COST_BATCH; i++)
            (VOID)task_alive(bogus);
        n += COST_BATCH;
    }

    if (n == 0)
        return 0;

    /* COST_TICKS ticks at 1/50 s is COST_TICKS * 20000 microseconds. */
    return (COST_TICKS * 20000UL) / n;
}

/* -------------------------------------------------- a socket on a port -- */

static LONG bind_udp(struct Library *base, LONG port)
{
    struct sockaddr_in_local sa;
    LONG                     s;
    LONG                     i;

    s = call_socket(base, AF_INET_L, SOCK_DGRAM_L, 0);
    if (s < 0)
        return -1;

    sa.sin_len    = sizeof(sa);
    sa.sin_family = AF_INET_L;
    sa.sin_port   = (UWORD)port;
    sa.sin_addr   = 0;                      /* INADDR_ANY */
    for (i = 0; i < 8; i++)
        sa.sin_zero[i] = 0;

    if (call_bind(base, s, &sa, (LONG)sizeof(sa)) != 0)
    {
        (VOID)call_closesocket(base, s);
        return -1;
    }

    return s;
}

static VOID set_io_mask(struct Library *base, ULONG mask)
{
    struct TagItem tags[2];

    tags[0].ti_Tag  = SBTM_SETVAL_SIGIOMASK;
    tags[0].ti_Data = mask;
    tags[1].ti_Tag  = TAG_END;
    tags[1].ti_Data = 0;

    (VOID)call_sbtaglist(base, tags);
}

/* --------------------------------------------------------- the children -- */

/*
 * Written by a child, read by the parent.  volatile, and the ready flag is
 * written LAST, so a parent that sees it set sees everything before it.
 */
static struct Task * volatile orphan_task;
static struct Library * volatile orphan_base;
static volatile LONG            orphan_socket = -1;
static volatile LONG            orphan_step;
static volatile BOOL            orphan_ready;

static volatile LONG            orphan_port = EVPOST_PORT;

static volatile LONG            control_step;
static volatile BOOL            control_ready;
static volatile BOOL            control_stop;
static volatile BOOL            control_done;
static volatile ULONG           control_wakes;
static volatile ULONG           control_bytes;

/*
 * Opens the library, asks to be signalled, binds a socket, and RETURNS
 * WITHOUT CLOSING ANYTHING.  That is the program under study, not a mistake
 * in the probe: it is what a user's program does when it forgets, or is
 * broken by Ctrl-C, and nothing reclaims any of it.
 */
static VOID orphan_entry(VOID)
{
    struct Library *base;
    LONG            s;

    orphan_task = FindTask(NULL);
    orphan_step = 1;

    base = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    if (base == NULL)
    {
        orphan_ready = TRUE;
        return;
    }
    orphan_base = base;
    orphan_step = 2;

    set_io_mask(base, MARK_MASK);
    orphan_step = 3;

    s = bind_udp(base, orphan_port);
    if (s < 0)
    {
        orphan_ready = TRUE;
        return;
    }
    orphan_socket = s;
    orphan_step   = 4;

    /* No CloseSocket().  No CloseLibrary().  Return, and die. */
    orphan_step  = 5;
    orphan_ready = TRUE;
}

/*
 * The same program, written correctly: it blocks in WaitSelect(), reads what
 * arrives, and closes both the socket and the library on the way out.
 *
 * This is the client the fix must not break, and WaitSelect() is the shape
 * that would break.  It Waits on sb_EventSigMask, and the only thing that
 * sets that bit is the Signal() in bsd_event_post() -- the very call the
 * sweeper decides whether to make.  A sweeper that clears sb_Task on a task
 * that is alive turns every one of these wakeups into a timeout, silently and
 * for the rest of the session, and that is a worse failure than the defect
 * being fixed.  So the parent pokes this child once per round for many rounds,
 * and every round is a fresh sweep that has to have got the answer right.
 */
static VOID control_entry(VOID)
{
    struct Library      *base;
    LONG                 s;
    ULONG                readfds;
    struct TimeVal_local tv;
    ULONG                sigs;
    LONG                 n;
    UBYTE                buf[64];

    control_step = 1;

    base = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    if (base == NULL)
    {
        control_ready = TRUE;
        control_done  = TRUE;
        return;
    }
    control_step = 2;

    set_io_mask(base, CTRL_MASK);
    control_step = 3;

    s = bind_udp(base, CONTROL_PORT);
    if (s < 0)
    {
        CloseLibrary(base);
        control_ready = TRUE;
        control_done  = TRUE;
        return;
    }
    control_step = 4;

    /* Start from a clean slate: nothing before this point is the stimulus. */
    (VOID)SetSignal(0UL, CTRL_MASK);

    control_step  = 5;
    control_ready = TRUE;

    while (!control_stop)
    {
        readfds       = 1UL << (ULONG)s;
        tv.tv_secs    = 1;
        tv.tv_micro   = 0;
        sigs          = 0;

        n = call_waitselect(base, s + 1, &readfds, &tv, &sigs);

        if (n > 0)
        {
            LONG got = call_recvfrom(base, s, buf, (LONG)sizeof(buf), 0,
                                     NULL, NULL);
            if (got > 0)
                control_bytes += (ULONG)got;

            /* Counted on the WAKEUP, not on the byte: a wakeup that delivers
               nothing is still a wakeup, and a byte that arrived without one
               is the failure this is watching for. */
            control_wakes++;
        }
    }

    (VOID)SetSignal(0UL, CTRL_MASK);
    (VOID)call_closesocket(base, s);
    CloseLibrary(base);

    control_step = 6;
    control_done = TRUE;
}

/* ------------------------------------------------------------------ main -- */

static LONG send_one(struct Library *base, LONG s, ULONG addr, LONG port)
{
    struct sockaddr_in_local to;
    UBYTE                    payload[4];
    LONG                     i;

    to.sin_len    = sizeof(to);
    to.sin_family = AF_INET_L;
    to.sin_port   = (UWORD)port;
    to.sin_addr   = addr;
    for (i = 0; i < 8; i++)
        to.sin_zero[i] = 0;

    payload[0] = 'e'; payload[1] = 'v'; payload[2] = 'p'; payload[3] = '\0';

    return call_sendto(base, s, payload, (LONG)sizeof(payload), 0,
                       &to, (LONG)sizeof(to));
}

/* One burst at both children, so a single stimulus covers both readings. */
static VOID stimulate(struct Library *base, LONG s, ULONG addr)
{
    (VOID)send_one(base, s, addr, EVPOST_PORT);
    (VOID)send_one(base, s, addr, CONTROL_PORT);
}

static VOID spawn(VOID (*entry)(VOID), const char *name)
{
    struct TagItem proc_tags[5];

    proc_tags[0].ti_Tag  = NP_Entry;
    proc_tags[0].ti_Data = (ULONG)entry;
    proc_tags[1].ti_Tag  = NP_Name;
    proc_tags[1].ti_Data = (ULONG)name;
    proc_tags[2].ti_Tag  = NP_StackSize;
    proc_tags[2].ti_Data = 8192;
    proc_tags[3].ti_Tag  = NP_Priority;
    proc_tags[3].ti_Data = 0;
    proc_tags[4].ti_Tag  = TAG_END;
    proc_tags[4].ti_Data = 0;

    (VOID)CreateNewProc(proc_tags);
}

static LONG finish(LONG rc)
{
    LONG args[2];

    args[0] = checks;
    args[1] = failures;
    VPrintf((CONST_STRPTR)"\n%ld checks, %ld failures\n", (APTR)args);
    Flush(Output());

    return rc;
}

int main(void)
{
    struct Library *base;
    struct Task    *dead;
    struct Task    *claimed;
    ULONG           claim_bytes = (ULONG)sizeof(struct Task);
    ULONG           before, quiet, after;
    LONG            s;
    LONG            i;
    LONG            on = 1;
    const char     *trigger = "none";
    BOOL            broadcast = FALSE;
    BOOL            woke = FALSE;
    BOOL            died;

    ULONG           soak_wakes = 0;
    LONG            soak_missed = 0;
    LONG            soak_first = -1;

    struct Task    *fast_dead = NULL;
    struct Task    *fast_claimed = NULL;
    ULONG           fast_after = 0;
    LONG            fast_ticks = -1;

    Printf((CONST_STRPTR)"EvPost: can a socket event signal a dead Task?\n");
    Flush(Output());

    base = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    if (!check(base != NULL, "bsdsocket.library opened", 0))
        return finish(RETURN_FAIL);

    /* The stack comes up inside OpenLibrary(); let the interface settle so
       the binds and the datagrams are not racing bringup. */
    Delay(SETTLE_TICKS * 4);

    /* ---- the live control ---------------------------------------------- */

    spawn(control_entry, "EvPostControl");
    for (i = 0; i < DEATH_TICKS && !control_ready; i++)
        Delay(1);
    check(control_ready && control_step == 5,
          "the live control child bound its socket", control_step);

    /* ---- the orphan ----------------------------------------------------- */

    spawn(orphan_entry, "EvPostOrphan");
    for (i = 0; i < DEATH_TICKS && !orphan_ready; i++)
        Delay(1);

    check(orphan_ready, "orphan reached the end of its work", orphan_step);
    check(orphan_step == 5, "orphan bound a socket and never closed it",
          orphan_step);

    dead = orphan_task;
    say("evpost/orphan_task=%08lx\n", (LONG)dead, 0);
    say("evpost/orphan_base=%08lx\n", (LONG)orphan_base, 0);
    say("evpost/orphan_socket=%ld\n", (LONG)orphan_socket, 0);

    if (!check(dead != NULL, "the orphan named its Task", 0))
        return finish(RETURN_FAIL);

    /* ---- wait for it to leave Exec's lists ------------------------------ */

    died = FALSE;
    for (i = 0; i < DEATH_TICKS; i++)
    {
        if (!task_alive(dead))
        {
            died = TRUE;
            break;
        }
        Delay(1);
    }

    say("evpost/death_ticks=%ld\n", (LONG)i, 0);
    check(died, "the orphan Task is on none of Exec's lists", (LONG)i);

    /*
     * A second's grace before the claim.  Any periodic sweeper gets its
     * chance here, which is the point: the probe must measure the fix as a
     * user would meet it, not race it.
     */
    Delay(SETTLE_TICKS * 4);

    /* ---- own the block --------------------------------------------------- */

    /*
     * AllocAbs() succeeds only if the whole range is free.  Success is the
     * evidence that the Task was handed back, and it is also what makes
     * everything after this a statement about memory this program owns.
     */
    claimed = NULL;
    for (i = 0; i < 50 && claimed == NULL; i++)
    {
        claimed = (struct Task *)AllocAbs(claim_bytes, (APTR)dead);
        if (claimed == NULL)
            Delay(2);
    }

    say("evpost/claim_bytes=%ld\n", (LONG)claim_bytes, 0);
    say("evpost/claimed=%s\n", (LONG)(claimed != NULL ? "yes" : "no"), 0);

    if (claimed == NULL)
    {
        /*
         * Not a pass.  Either the block was never freed, or something took it
         * between the death and the claim.  Reading it now would be reading
         * somebody else's memory and the answer could not be attributed.
         */
        check(FALSE, "AllocAbs() reclaimed the freed Task block", 0);
        Printf((CONST_STRPTR)
               "evpost/verdict=inconclusive (the block could not be owned)\n");
        control_stop = TRUE;
        return finish(RETURN_FAIL);
    }

    check(TRUE, "AllocAbs() reclaimed the freed Task block", 0);

    /* ---- zero, and take the quiet reading ------------------------------- */

    {
        UBYTE *p = (UBYTE *)claimed;
        ULONG  n;

        for (n = 0; n < claim_bytes; n++)
            p[n] = 0;
    }

    before = claimed->tc_SigRecvd;
    say("evpost/before=%08lx\n", (LONG)before, 0);
    check(before == 0, "the claimed block starts zeroed", (LONG)before);

    Delay(SETTLE_TICKS);
    quiet = claimed->tc_SigRecvd;
    say("evpost/quiet=%08lx\n", (LONG)quiet, 0);
    check(quiet == 0,
          "nothing writes the block while no datagram is sent", (LONG)quiet);

    /* ---- one burst, aimed at both children ------------------------------ */

    s = call_socket(base, AF_INET_L, SOCK_DGRAM_L, 0);
    check(s >= 0, "this program made a socket to send from", s);

    after = claimed->tc_SigRecvd;

    if (s >= 0)
    {
        /* 127.0.0.1: the stack has a loopback interface. */
        stimulate(base, s, 0x7F000001UL);
        Delay(SETTLE_TICKS * 2);
        after = claimed->tc_SigRecvd;

        if (control_wakes != 0UL || (after & MARK_MASK) != 0)
        {
            trigger = "loopback";
        }
        else
        {
            /* This build defines NX_ENABLE_IP_BROADCAST_LOOPBACK, so a
               broadcast comes back to our own bound sockets. */
            (VOID)call_setsockopt(base, s, SOL_SOCKET_L, SO_BROADCAST_L,
                                  &on, (LONG)sizeof(on));
            broadcast = TRUE;
            stimulate(base, s, 0xFFFFFFFFUL);
            Delay(SETTLE_TICKS * 2);
            after = claimed->tc_SigRecvd;

            if (control_wakes != 0UL || (after & MARK_MASK) != 0)
                trigger = "broadcast";
        }
    }

    woke = (control_wakes != 0UL);

    say("evpost/after=%08lx\n", (LONG)after, 0);
    say("evpost/mark=%08lx\n", (LONG)MARK_MASK, 0);
    say("evpost/trigger=%s\n", (LONG)trigger, 0);
    say("evpost/control_woke=%s\n", (LONG)(woke ? "yes" : "no"), 0);
    say("evpost/uaf=%s\n",
        (LONG)(((after & MARK_MASK) != 0) ? "yes" : "no"), 0);

    /*
     * The reading, stated as the question it answers.  Deliberately NOT a
     * check(): "reachable" and "latched" are both results, and which one is
     * expected depends on which build this is.  The gate lives in the run
     * script, which knows.  The one thing that is always wrong is a stimulus
     * that did not arrive, because it makes the other reading meaningless.
     */
    check(woke,
          "the stimulus reached a LIVE client, so the reading means something",
          (LONG)control_step);

    if (!woke)
        Printf((CONST_STRPTR)
               "evpost/verdict=inconclusive (no datagram reached either "
               "socket)\n");
    else if ((after & MARK_MASK) != 0)
        Printf((CONST_STRPTR)
               "evpost/verdict=REACHABLE: Signal() wrote the mask into a "
               "freed Task\n");
    else
        Printf((CONST_STRPTR)
               "evpost/verdict=LATCHED: a live client woke, the freed Task "
               "was not touched\n");

    /* ---- the soak: does a live WaitSelect() client keep waking? ---------- */

    /*
     * The false-negative check, and the one that matters more than the fix.
     *
     * A sweeper runs once a second whether or not anything is wrong, and every
     * pass is another chance to clear sb_Task on a task that is alive.  The
     * cost of getting that wrong is not a crash: it is a client that never
     * wakes again, for the rest of the session, looking exactly like the
     * network having gone away.  So the live child is poked once per round for
     * SOAK_ROUNDS rounds spanning rather more than SOAK_ROUNDS sweeps, and
     * every single round has to produce a wakeup.
     *
     * A missed round is recorded rather than broken out of: how many were
     * missed, and whether they stop for good after the first, is the
     * difference between a dropped datagram and a latched pointer.
     */
    {
        ULONG seen  = control_wakes;
        LONG  round;
        LONG  waited;

        soak_missed = 0;
        soak_first  = -1;

        for (round = 0; round < SOAK_ROUNDS; round++)
        {
            if (s >= 0)
                (VOID)send_one(base, s,
                               broadcast ? 0xFFFFFFFFUL : 0x7F000001UL,
                               CONTROL_PORT);

            /*
             * POLL FOR THE WAKEUP, do not sample at a fixed offset.  Sampling
             * every SOAK_ROUND_TICKS against a child on its own schedule
             * reports a miss whenever the two drift past each other and a
             * double on the round after, which measures the drift and not the
             * thing being asked about.  What is being asked is "did this
             * datagram wake the client within a bounded time", so wait for
             * that answer and bound the wait.
             */
            for (waited = 0; waited < SOAK_WAIT_TICKS; waited++)
            {
                if (control_wakes > seen)
                    break;
                Delay(1);
            }

            if (control_wakes > seen)
            {
                seen = control_wakes;
            }
            else
            {
                soak_missed++;
                if (soak_first < 0)
                    soak_first = round;
            }

            Delay(SOAK_ROUND_TICKS);
        }

        soak_wakes = control_wakes;
    }

    say("evpost/soak_rounds=%ld\n", (LONG)SOAK_ROUNDS, 0);
    say("evpost/soak_seconds=%ld\n",
        (LONG)((SOAK_ROUNDS * SOAK_ROUND_TICKS) / 50), 0);
    say("evpost/soak_wakes=%ld\n", (LONG)soak_wakes, 0);
    say("evpost/soak_bytes=%ld\n", (LONG)control_bytes, 0);
    say("evpost/soak_missed=%ld\n", (LONG)soak_missed, 0);
    say("evpost/soak_first_miss=%ld\n", (LONG)soak_first, 0);

    check(soak_missed == 0,
          "a WaitSelect() client woke on every round of the soak",
          (LONG)soak_missed);

    /* ---- the residual window ------------------------------------------- */

    /*
     * What a once-a-second sweep cannot do is act inside the second.  This
     * measures that rather than leaving it as a caveat: a SECOND orphan is
     * made and stimulated as fast as the probe can manage after its Task
     * goes, with none of the grace the first one was given.
     *
     * On a build with the sweep, `fast_uaf=yes` is the expected reading and is
     * NOT a failure -- it is the size of the remaining hole, and it is also
     * the thing that proves the datagram still reaches an orphan's socket on
     * this build, so the first orphan's `uaf=no` cannot be explained away as
     * a packet that never arrived.
     */
    orphan_ready  = FALSE;
    orphan_task   = NULL;
    orphan_base   = NULL;
    orphan_step   = 0;
    orphan_socket = -1;
    orphan_port   = FAST_PORT;

    spawn(orphan_entry, "EvPostFast");
    for (i = 0; i < DEATH_TICKS && !orphan_ready; i++)
        Delay(1);

    fast_dead = orphan_task;
    if (orphan_step == 5 && fast_dead != NULL)
    {
        LONG start_ticks = (LONG)ticks_now();

        for (i = 0; i < DEATH_TICKS && task_alive(fast_dead); i++)
            Delay(1);

        /* No grace, no retries: whatever AllocAbs says first is the answer.
           Waiting here would be waiting for the sweeper. */
        fast_claimed = (struct Task *)AllocAbs((ULONG)sizeof(struct Task),
                                               (APTR)fast_dead);
        if (fast_claimed != NULL)
        {
            UBYTE *p = (UBYTE *)fast_claimed;
            ULONG  n;

            for (n = 0; n < (ULONG)sizeof(struct Task); n++)
                p[n] = 0;

            if (s >= 0)
                (VOID)send_one(base, s,
                               broadcast ? 0xFFFFFFFFUL : 0x7F000001UL,
                               FAST_PORT);
            Delay(SETTLE_TICKS);

            fast_after = fast_claimed->tc_SigRecvd;
            fast_ticks = (LONG)ticks_now() - start_ticks;
        }
    }

    say("evpost/fast_claimed=%s\n",
        (LONG)(fast_claimed != NULL ? "yes" : "no"), 0);
    say("evpost/fast_after=%08lx\n", (LONG)fast_after, 0);
    say("evpost/fast_ticks=%ld\n", (LONG)fast_ticks, 0);
    say("evpost/fast_uaf=%s\n",
        (LONG)(((fast_after & MARK_MASK) != 0) ? "yes" : "no"), 0);

    /* ---- stop the live child ------------------------------------------- */

    if (s >= 0)
        (VOID)call_closesocket(base, s);

    control_stop = TRUE;
    for (i = 0; i < DEATH_TICKS && !control_done; i++)
        Delay(1);

    say("evpost/control_closed=%s\n", (LONG)(control_done ? "yes" : "no"), 0);
    check(control_done, "the live control closed its socket and library",
          (LONG)control_step);

    /* ---- what a sweep costs on this machine ----------------------------- */

    say("evpost/tasks=%ld\n", (LONG)count_tasks(), 0);
    say("evpost/scan_us=%ld\n", (LONG)scan_micros(), 0);

    /*
     * The claimed range is NOT freed and the orphan's base is NOT closed.
     * See the header: giving the range back would hand a dead base's Signal()
     * target to the next allocation, which is the defect, not the test.
     */
    say("evpost/retained=%ld bytes, deliberately\n", (LONG)claim_bytes, 0);

    CloseLibrary(base);

    return finish(failures == 0 ? RETURN_OK : RETURN_FAIL);
}
