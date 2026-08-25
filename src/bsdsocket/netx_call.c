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

#ifdef AMINETXDUO_GREEN_REALM

/*
 * The request gate, the green realm's client boundary.
 */
static LONG bsd_gate_enter(struct AmiSocketBase *base)
{
    TX_AMIGA_GATE *gate = &base->sb_NxGate;

    if (base->sb_NxGateDead)
        return -1;

    if (tx_amiga_kernel_running() != (UINT)TX_TRUE)
        return -1;

    /* A stack-side context (a green thread, the realm) arriving here is
       already a ThreadX thread; the plain path answers it with a no-op. */
    if (tx_amiga_caller_is_thread() != (UINT)TX_FALSE)
        return -1;

    if (gate->ag_Live == 0U)
    {
        if (tx_amiga_gate_bind(gate, (CHAR *)"aminetxduo proxy",
                               AMI_CALLER_PRIORITY) != TX_SUCCESS)
        {
            base->sb_NxGateDead = TRUE;
            return -1;
        }
    }

    if (tx_amiga_gate_call(gate, base->sb_BreakMask) != TX_SUCCESS)
        return -1;

    /* On the realm, as the proxy, from here to bsd_nx_leave(). */
    return 0;
}

ULONG bsd_break_signals(struct AmiSocketBase *base)
{
    if (base->sb_NxGated)
        return tx_amiga_gate_breaks(&base->sb_NxGate);

    return SetSignal(0UL, 0UL);
}

BOOL bsd_nx_orphan(struct AmiSocketBase *base)
{
    return (tx_amiga_gate_orphan(&base->sb_NxGate) != (UINT)TX_FALSE)
           ? TRUE : FALSE;
}

#endif /* AMINETXDUO_GREEN_REALM (baton builds inline both helpers in
          bsdsocket_internal.h, so the host tier's per-file test builds link
          without this file) */

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

#ifdef AMINETXDUO_GREEN_REALM
    {
#ifdef AMINETXDUO_RXPROBE
        ULONG gt0 = ami_budget_clock();
#endif
        LONG fast = ami_netstack_try_enter_cached(&base->sb_NxCaller);

        if (fast == AMI_NET_OK)
        {
#ifdef AMINETXDUO_RXPROBE
            ami_budget_baton(ami_budget_clock() - gt0);
#endif
            if (base->sb_NxCaller.nc_Adopted)
                tx_amiga_gate_fast_note();
            base->sb_NxGated = FALSE;
            base->sb_NxNest  = 1;
            return 0;
        }

        if (bsd_gate_enter(base) == 0)
        {
#ifdef AMINETXDUO_RXPROBE
            ami_budget_baton(ami_budget_clock() - gt0);
#endif
            base->sb_NxGated = TRUE;
            base->sb_NxNest  = 1;
            return 0;
        }
    }
    if (tx_amiga_kernel_running() == (UINT)TX_TRUE &&
        tx_amiga_caller_is_thread() == (UINT)TX_FALSE)
        tx_amiga_gate_fallback_note();
#endif

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

#ifdef AMINETXDUO_GREEN_REALM
    if (base->sb_NxGated)
    {
        /* Cleared first: gated must imply "the body is on the realm", and
           past this point it is not.  gate_return() completes the request
           -- proxy suspended, one Signal to the owner -- and RETURNS ON THE
           OWNER, which then runs the vector epilogue as itself. */
        base->sb_NxGated = FALSE;
        tx_amiga_gate_return(&base->sb_NxGate);
        return;
    }
#endif

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

    base->sb_NxNest = 0;

#ifdef AMINETXDUO_GREEN_REALM
    /* The gate goes first: it holds a TX_THREAD of its own, and this runs on
       the owning task with no bracket open, which is the one context where
       the completion signal bit can be recovered. */
    tx_amiga_gate_release(&base->sb_NxGate);
    base->sb_NxGated = FALSE;
#endif

    ami_netstack_release(&base->sb_NxCaller);
}
