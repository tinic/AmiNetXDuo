/*
 * EvPost, does a socket event signal a Task that has exited?
 *
 * Leaves the claimed range allocated and the orphaned base open on purpose:
 * this probe is not a leak test and must not be run inside one.
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

/* SBTM_SETVAL(SBTC_SIGIOMASK), spelled out so this file needs no amitcp
   headers: TAG_USER | (code << SBTB_CODE) | SBTF_SET, SBTC_SIGIOMASK 2. */
#define SBTM_SETVAL_SIGIOMASK   0x80000005UL

/* The masks the children ask to be signalled with.  They must be disjoint from
   the ones the library allocates for itself (AllocSignal(-1) fills downwards
   from bit 31) and from SIGF_DOS, SIGF_SINGLE and SIGBREAKF_CTRL_C. */
#define MARK_MASK               0x0F000000UL    /* the orphan's  */
#define CTRL_MASK               0x30000000UL    /* the live control's */

#define EVPOST_PORT             2308
#define CONTROL_PORT            2309
#define FAST_PORT               2310

/* Ceilings, in Delay() ticks of 1/50 s.  Reaching one is reported, not
   waited out: a probe that hangs proves nothing. */
#define DEATH_TICKS             500         /* 10 s */
#define SETTLE_TICKS            25          /* 0.5 s */

/* 120 rounds half a second apart is a minute of guest time, so the live client
   is poked across rather more than sixty sweeps. */
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
 * The base is a parameter, not a global: the children and this program hold
 * DIFFERENT bases, and which one a signal is charged to is the whole point.
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
 * Disable(), not Forbid(): an interrupt moves a task between TaskWait and
 * TaskReady, so Forbid() does not make the walk safe.  Nothing here
 * dereferences the candidate; the block may already be somebody else's.
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
 * Microseconds for ONE liveness scan of a task on neither list, the worst case
 * and the only one that matters.  Batched so DateStamp() is not the measure.
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
 * Opens the library, asks to be signalled, binds a socket, and RETURNS WITHOUT
 * CLOSING ANYTHING.  That is the program under study, not a bug in the probe.
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
 * The same program written correctly: it blocks in WaitSelect(), which is the
 * shape a sweeper that clears sb_Task on a LIVE task would break, silently
 * turning every wakeup into a timeout for the rest of the session.
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

    /* A second's grace: any periodic sweeper gets its chance here, which is
       the point -- measure the fix as a user meets it, do not race it. */
    Delay(SETTLE_TICKS * 4);

    /* ---- own the block --------------------------------------------------- */

    /* AllocAbs() succeeds only if the whole range is free, so success is the
       evidence that the Task was handed back. */
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

    /* Deliberately not a pass/fail on the reading itself: reachable and
       latched are both results, and the gate lives in the run script. */
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
     * A missed round is recorded rather than broken out of: how many were
     * missed, and whether they stop for good after the first, separates a
     * dropped datagram from a latched pointer.
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

            /* Poll for the wakeup rather than sampling at a fixed offset:
               sampling against a child on its own schedule measures the
               drift, not whether the datagram woke the client in time. */
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
     * On a build with the sweep `fast_uaf=yes` is expected and is NOT a
     * failure: it is the size of the remaining hole, and it proves a datagram
     * still reaches an orphan's socket on this build.
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

    /* The claimed range is NOT freed and the orphan's base is NOT closed:
       giving the range back is the defect, not the test. */
    say("evpost/retained=%ld bytes, deliberately\n", (LONG)claim_bytes, 0);

    CloseLibrary(base);

    return finish(failures == 0 ? RETURN_OK : RETURN_FAIL);
}
