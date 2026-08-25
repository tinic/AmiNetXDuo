/*
 * bsdsocket.library, putting the calling task into ThreadX context.
 *
 * NetX Duo checks who is calling. Roughly forty of its entry points are wrapped
 * in NX_THREADS_ONLY_CALLER_CHECKING and return NX_CALLER_ERROR unless the
 * caller is a ThreadX thread. The set is bind, listen, unlisten, accept,
 * relisten, unaccept, the client bind/unbind/connect, disconnect, send,
 * receive, bytes_available, peer_info_get, port_get and both socket_delete
 * flavours. An Exec Task that ThreadX has never adopted fails all of them.
 * bsd_status_map[] turns NX_CALLER_ERROR into EINVAL, so the failure surfaces
 * as "listen(): Invalid argument".
 *
 * Only nx_tcp_socket_create/nx_udp_socket_create (INIT_AND_THREADS) and the
 * nx_packet_* helpers (no check at all) tolerate a plain Task, which is why
 * socket() used to be the one call that worked.
 *
 * The adopt/orphan itself is ami_netstack_enter()/ami_netstack_leave() from
 * include/aminetxduo/netstack.h, the same bracket src/netstack/ and the tools
 * use. This file adds only what is specific to a library base: where the
 * TX_THREAD control block lives, and the nesting counter.
 *
 * The ThreadX scheduler lock is per call, the thread is per task. The port's
 * adoption model (port/threadx-amiga/src/tx_amiga_adopt.c) gives an adopted
 * Task that lock. While it is adopted, no other ThreadX thread runs, including
 * the NetX Duo IP thread and the periodic timer. Held from OpenLibrary() to
 * CloseLibrary(), it would sit inside application code for the lifetime of the
 * base, where one Wait() on an Intuition port stops the entire stack. So it is
 * taken and released on every call.
 *
 * The TX_THREAD does not have to be rebuilt each time. It used to be: this file
 * called ami_netstack_enter(), which adopts on the way in and orphans on the
 * way out, so every recv(), every send() and every poll pass inside
 * WaitSelect() paid an AllocSignal(), a _tx_thread_create(), a
 * _tx_thread_terminate(), a _tx_thread_delete() and a scheduler poke.
 * tests/perf/bracket_test.c prices that pair at ~790 us on a 14 MHz 68020, of
 * which AllocSignal()/FreeSignal() is 17 us. It is a per-call constant, so it
 * scales with how a client reads rather than with how much it reads. It costs
 * a client that reads small, and it does not decide a bulk transfer: a 1.2 MB
 * fetch by a third-party curl takes 108 brackets, 11 kB a call, and the cached
 * and uncached arms both measured 825 ticks.
 *
 * A base belongs to one task (library.c records it in sb_Task) and that task
 * gets the same TX_THREAD every time. ami_netstack_enter_cached() builds it
 * once and keeps it dormant between brackets, TX_SUSPENDED and on no ready
 * list. The same measurement prices that pair at ~270 us. bsd_nx_release()
 * gives it back, from bsd_child_destroy() in library.c, which runs on the
 * owning task with every socket already closed.
 *
 * WaitSelect() does not fit this shape. It blocks in Exec Wait() for as long as
 * the caller asked for, so it brackets each poll pass and drops out of ThreadX
 * context before parking. See select.c.
 *
 * Vectors call other vectors' internals (CloseSocket -> bsd_socket_release,
 * accept -> relisten) and a base's task is inside at most one vector at a time,
 * so a plain depth counter in the base is enough. Depth > 0 means the task is
 * already inside the bracket.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"
#include "aminetxduo/budget.h"
#include "../thread_priorities.h"

#include <proto/exec.h>

/*
 * The control arm. -DAMINETXDUO_NXCACHE=OFF puts the per-call adopt/orphan
 * back, so two builds differ in this one decision and nothing else. That is
 * how the before/after numbers in docs/RESEARCH.md were taken.
 */
#ifndef AMINETXDUO_NXCACHE
#  define AMINETXDUO_NXCACHE 1
#endif

/*
 * Counts the brackets a client's fetch costs and their share of its wall clock.
 * Off by default, because it adds two ReadEClock() calls per bracket, which is
 * the thing under measurement.
 *
 *   cmake -B build/census -DAMINETXDUO_NXCENSUS=ON ...
 *
 * The totals go to the serial log when the base is closed.
 */
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
 *
 * In a green build the bracket does not adopt the calling Task; it MIGRATES
 * the rest of the vector into the realm.  tx_amiga_gate_call() captures the
 * caller's continuation (what a context switch saves IS the request), hands
 * it to a cached green proxy thread, and parks the Task in one Wait() on the
 * completion signal.  From that point to bsd_nx_leave() the vector body runs
 * inside the realm: a NetX suspension is a stack switch among green threads
 * instead of an Exec Signal/Wait round trip per wake, and the realm's idle
 * Wait() stays the only place the stack sleeps.  bsd_nx_leave() completes
 * the request -- one boundary Signal back -- and the vector's epilogue runs
 * on the owner again.
 *
 * The region is migratable because the bracket discipline already demanded
 * it: "Inside one, nothing must block on anything except ThreadX"
 * (bsdsocket_internal.h).  The one piece of owner state a body consults is
 * the break mask, and that goes through bsd_break_signals() below.
 *
 * Everything that cannot gate -- a foreign task, a bind that failed, the
 * stack going down, a nested stack context -- falls back to the adopted
 * baton bracket, which remains fully wired.  The fall-backs are counted
 * (netstat "green:" gate line) so a run whose gate is quietly dead shows.
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
            /* Latched: a machine that cannot gate keeps the baton bracket
               and does not retry an AllocMem per socket call. */
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
        /*
         * The free-baton fast path first (cycle 3's verdict: mid-transfer
         * the baton is takeable far more often than not, and the gate's
         * unconditional submission was ~1 ms per bracket of pure cost
         * there).  The try is take-or-back-out under one Forbid() in the
         * port, so there is no window between "the realm is idle" and
         * holding the baton; a decline touched nothing and the gate
         * carries the call exactly as cycle 2 built it.  Either way the
         * realm's one Wait() stays the only idle point: a fast take means
         * the realm had nothing ready and it still wakes on its waiters'
         * signals while we hold the baton; a gated call pokes it
         * unconditionally from the parker.
         */
        LONG fast = ami_netstack_try_enter_cached(&base->sb_NxCaller);

        if (fast == AMI_NET_OK)
        {
            /* The baton leg is the fast take, clocked like the old adopt
               entry so control and green numbers stay comparable. */
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
            /* The baton leg is the gate handoff: submission to the
               proxy's first instruction, clocked across the migration. */
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
    /* The budget's baton leg: what a caller waits between asking for the
       scheduler bracket and holding it.  This is the suspect the fetch leg's
       priority experiment left standing, so it is measured at the one place
       every recv() passes through rather than inferred. */
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
 * bracket open. A base taken down by anyone else still gets the registration
 * removed, see ami_netstack_release().
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
