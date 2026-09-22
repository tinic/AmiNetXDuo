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
#ifdef AMINETXDUO_NXCENSUS
    ULONG t0;
#endif

    if (base == NULL)
        return -1;

    if (base->sb_NxNest > 0)
    {
        base->sb_NxNest++;
#ifdef AMINETXDUO_NXCENSUS
        base->sb_NxNested++;
#endif
        return 0;
    }

#ifdef AMINETXDUO_NXCENSUS
    t0 = bsd_nx_eclock();
#endif

#ifdef AMINETXDUO_RXPROBE
    {
        ULONG bt0 = ami_budget_clock();

#if AMINETXDUO_NXCACHE
        if (ami_netstack_enter_cached(&base->sb_NxCaller) != AMI_NET_OK)
            return -1;
#else
        if (ami_netstack_enter(&base->sb_NxCaller) != AMI_NET_OK)
            return -1;
#endif
        ami_budget_baton(ami_budget_clock() - bt0);
    }
#else
#if AMINETXDUO_NXCACHE
    if (ami_netstack_enter_cached(&base->sb_NxCaller) != AMI_NET_OK)
        return -1;
#else
    if (ami_netstack_enter(&base->sb_NxCaller) != AMI_NET_OK)
        return -1;
#endif
#endif

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

    base->sb_NxNest     = 0;
    base->sb_NxTask     = NULL;
    base->sb_ErrPending = 0;    /* dropped, not delivered: no bracket to leave */

    ami_netstack_release(&base->sb_NxCaller);
}
