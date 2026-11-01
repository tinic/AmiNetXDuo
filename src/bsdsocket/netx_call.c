/*
 * bsdsocket.library, putting the calling task into ThreadX context.
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"
#include "aminetxduo/budget.h"
#include "../thread_priorities.h"

#include <proto/exec.h>

#ifndef AMINETXDUO_NXCACHE
#  define AMINETXDUO_NXCACHE 1
#endif

#ifdef AMINETXDUO_NXCENSUS
#  include <proto/timer.h>
#  include <devices/timer.h>

extern struct Device *TimerBase;

static ULONG bsd_nx_eclock(VOID)
{
    struct EClockVal ev;

    if (TimerBase == NULL)
        return 0;

    (VOID)ReadEClock(&ev);

    return ev.ev_lo;
}
#endif

LONG bsd_nx_enter(struct AmiSocketBase *base)
{
    LONG entered;
#ifdef AMINETXDUO_NXCENSUS
    ULONG t0;
#endif

    if (base == NULL)
        return -1;

    /*
     * The depth is the base's, the bracket is its holder's.  Another Task
     * reaching this base while the holder is inside -- suspended in NetX, or
     * only time-sliced out by Exec -- used to nest on it and run NetX with no
     * TX_THREAD and no baton, racing the holder, and its leave could end the
     * holder's bracket from the wrong Task (F-042).  It is refused, as
     * ami_netstack_enter_cached() refuses a second Task at depth 0.
     */
    if (base->sb_NxNest > 0)
    {
        if (base->sb_NxTask != FindTask(NULL))
            return -1;

        base->sb_NxNest++;
#ifdef AMINETXDUO_NXCENSUS
        base->sb_NxNested++;
#endif
        return 0;
    }

#ifdef AMINETXDUO_NXCENSUS
    t0 = bsd_nx_eclock();
#endif

    /* A full pool parks the enter until a slot comes back; the break ends
       that wait, as it ends the call's own (#383). */
    base->sb_NxCaller.nc_BreakMask = base->sb_BreakMask;

#ifdef AMINETXDUO_RXPROBE
    {
        ULONG bt0 = ami_budget_clock();

#if AMINETXDUO_NXCACHE
        entered = ami_netstack_enter_cached(&base->sb_NxCaller);
#else
        entered = ami_netstack_enter(&base->sb_NxCaller);
#endif
        ami_budget_baton(ami_budget_clock() - bt0);
    }
#else
#if AMINETXDUO_NXCACHE
    entered = ami_netstack_enter_cached(&base->sb_NxCaller);
#else
    entered = ami_netstack_enter(&base->sb_NxCaller);
#endif
#endif

    base->sb_NxBroken = (entered == AMI_NET_ERR_ABORTED) ? TRUE : FALSE;
    if (entered != AMI_NET_OK)
        return -1;

#ifdef AMINETXDUO_NXCENSUS
    t0 = bsd_nx_eclock() - t0;
    base->sb_NxEnterTicks += t0;
    base->sb_NxCount++;
    if (t0 > 709UL)                     /* over a millisecond */
        base->sb_NxSlow++;
    if (t0 > base->sb_NxWorst)
        base->sb_NxWorst = t0;
#endif

    base->sb_NxNest = 1;
    base->sb_NxTask = FindTask(NULL);

    return 0;
}

VOID bsd_nx_leave(struct AmiSocketBase *base)
{
#ifdef AMINETXDUO_NXCENSUS
    ULONG t0;
#endif

    if (base == NULL || base->sb_NxNest <= 0)
        return;

    if (--base->sb_NxNest > 0)
        return;

#ifdef AMINETXDUO_NXCENSUS
    t0 = bsd_nx_eclock();
#endif

#if AMINETXDUO_NXCACHE
    ami_netstack_leave_cached(&base->sb_NxCaller);
#else
    ami_netstack_leave(&base->sb_NxCaller);
#endif

#ifdef AMINETXDUO_NXCENSUS
    base->sb_NxLeaveTicks += bsd_nx_eclock() - t0;
#endif

    /* The errno hook calls the bracket held back, now that a blocking hook
       stalls nothing but this task. */
    if (base->sb_ErrPending)
        bsd_error_hook_flush(base);

    base->sb_NxTask = NULL;
}

/*
 * A blocking call's wait, made in Exec with the slot given back.
 *
 * A Task suspended in NetX Duo holds an adoption slot for as long as it waits,
 * and the slots are a fixed pool (tx_amiga_pool.c).  Fifteen Tasks blocked in
 * recv() took every slot, and the sixteenth caller -- any program, httpd
 * included -- waited in the pool, deaf to Ctrl-C, until one of the fifteen
 * got data (#383).  Here the leave makes the adoption dormant, so a full pool
 * takes it back, and the bracket stays this Task's: sb_NxNest and sb_NxTask
 * are untouched, so another Task on this base is still refused, as it is
 * while this one is suspended in NetX.
 *
 * Only the opener waits here: the event signal goes to sb_Task.
 */
LONG bsd_nx_park(struct AmiSocketBase *base, ULONG mask, ULONG *received)
{
    if (base->sb_NxNest != 1 || base->sb_NxTask != base->sb_Task ||
        FindTask(NULL) != base->sb_Task)
        return BSD_PARK_NONE;

#if AMINETXDUO_NXCACHE
    ami_netstack_leave_cached(&base->sb_NxCaller);
#else
    ami_netstack_leave(&base->sb_NxCaller);
#endif

    *received = Wait(mask);

    /* The break stays set: it ends the re-enter's wait for a slot too, and
       the caller answers it (#383). */
    if ((*received & base->sb_BreakMask) != 0)
        SetSignal(*received & base->sb_BreakMask,
                  *received & base->sb_BreakMask);
    base->sb_NxCaller.nc_BreakMask = base->sb_BreakMask;

#if AMINETXDUO_NXCACHE
    if (ami_netstack_enter_cached(&base->sb_NxCaller) == AMI_NET_OK)
        return BSD_PARK_DONE;
#else
    if (ami_netstack_enter(&base->sb_NxCaller) == AMI_NET_OK)
        return BSD_PARK_DONE;
#endif

    base->sb_NxNest = 0;
    base->sb_NxTask = NULL;

    return BSD_PARK_LOST;
}

/*
 * Give the cached TX_THREAD back. Called from bsd_child_destroy() (library.c)
 * after the last socket is shut, so it runs on the base's own task with no
 */
VOID bsd_nx_release(struct AmiSocketBase *base)
{
    if (base == NULL)
        return;

#ifdef AMINETXDUO_NXCENSUS
    /* PAL runs 709379 E-Clock ticks a second. The report is in milliseconds. */
    AMI_INFO("bsdsocket: %ld brackets (%ld nested): enter %ld ms, leave %ld ms,"
             " %ld over 1 ms, worst %ld ms",
             (long)base->sb_NxCount, (long)base->sb_NxNested,
             (long)(base->sb_NxEnterTicks / 709UL),
             (long)(base->sb_NxLeaveTicks / 709UL),
             (long)base->sb_NxSlow,
             (long)(base->sb_NxWorst / 709UL));
#endif

#ifdef AMINETXDUO_SCHEDCOUNT
    /* The mutex counts are the whole stack's since boot, so the reports of
       two openers either side of a run bracket it. */
    AMI_INFO("bsdsocket: schedcount: %ld tcp sends, mss %ld peeked %ld locked;"
             " stack mutex get %ld put %ld",
             (long)base->sb_ScTcpSends, (long)base->sb_ScMssPeek,
             (long)base->sb_ScMssLocked,
             (long)_tx_amiga_sched_count[TX_AMIGA_SC_MUTEX_GET],
             (long)_tx_amiga_sched_count[TX_AMIGA_SC_MUTEX_PUT]);
#endif

    base->sb_NxNest     = 0;
    base->sb_NxTask     = NULL;
    base->sb_ErrPending = 0;    /* dropped, not delivered: no bracket to leave */

    ami_netstack_release(&base->sb_NxCaller);
}
