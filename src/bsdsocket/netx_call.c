/*
 * bsdsocket.library -- putting the calling task into ThreadX context.
 *
 * WHY
 *
 * NetX Duo checks who is calling. Roughly forty of its entry points are
 * wrapped in NX_THREADS_ONLY_CALLER_CHECKING, which returns NX_CALLER_ERROR
 * unless tx_thread_identify() is non-NULL -- that is bind, listen, unlisten,
 * accept, relisten, unaccept, the client bind/unbind/connect, disconnect,
 * send, receive, bytes_available, peer_info_get, port_get and both
 * socket_delete flavours. An Exec Task that ThreadX has never adopted fails
 * every one of them, and bsd_status_map[] turns NX_CALLER_ERROR into EINVAL,
 * so the failure surfaces as "listen(): Invalid argument" rather than as
 * anything that points at the real cause.
 *
 * Only nx_tcp_socket_create/nx_udp_socket_create (INIT_AND_THREADS) and the
 * nx_packet_* helpers (no check at all) tolerate a plain Task -- which is
 * exactly why socket() used to be the one thing that worked.
 *
 * The adopt/orphan itself is ami_netstack_enter()/ami_netstack_leave() from
 * include/aminetxduo/netstack.h -- the same bracket src/netstack/ and the
 * tools use. This file adds only the two things that are specific to a
 * library base: where the TX_THREAD control block lives, and the nesting
 * counter.
 *
 * GRANULARITY: Per call for the baton, Per task for the thread
 *
 * The port's adoption model (port/threadx-amiga/src/tx_amiga_adopt.c) makes
 * an adopted Task the holder of the ThreadX baton: while it is adopted, no
 * other ThreadX thread runs, including the NetX Duo IP thread and the
 * periodic timer. Holding the baton from OpenLibrary() to CloseLibrary()
 * would therefore park it inside application code for the lifetime of the
 * base -- one Wait() on an Intuition port and the entire stack stops. So the
 * BATON is taken and given back on every call, and that is not negotiable.
 *
 * The TX_THREAD is a different thing and does not have to be rebuilt each
 * time. It used to be: this file called ami_netstack_enter(), which adopts on
 * the way in and orphans on the way out, so every recv(), every send() and
 * every poll pass inside WaitSelect() paid an AllocSignal(), a
 * _tx_thread_create(), a _tx_thread_terminate(), a _tx_thread_delete() and a
 * scheduler poke. tests/perf/bracket_test.c prices that pair at ~790 us on a
 * 14 MHz 68020, of which the AllocSignal()/FreeSignal() is 17 us. It is a
 * per-CALL constant, so it scales with how a client reads rather than with
 * how much it reads -- which is exactly the shape of docs/RESEARCH.md §29.3,
 * where this stack beat Roadshow on connect and on time-to-first-byte and
 * lost the body, on a client that reads small and selects between reads.
 *
 * A base belongs to one task (library.c records it in sb_Task), and that task
 * gets the same TX_THREAD every time, so ami_netstack_enter_cached() builds
 * it once and keeps it dormant -- TX_SUSPENDED, on no ready list, dispatchable
 * by nobody -- between brackets. The same measurement prices that pair at
 * ~270 us. bsd_nx_release() gives it back, and library.c calls it from
 * bsd_child_destroy(), which is the one place that runs on the owning task
 * with every socket already closed.
 *
 * The one call this shape does not fit is WaitSelect(), which blocks in Exec
 * Wait() for as long as the caller asked for. It brackets each poll pass and
 * drops out of ThreadX context before parking -- see select.c.
 *
 * Vectors call other vectors' internals (CloseSocket -> bsd_socket_release,
 * accept -> relisten), and a base's task is inside at most one vector at a
 * time, so a plain depth counter in the base is enough. Depth > 0 means the
 * task already holds the baton.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include <proto/exec.h>

/*
 * The control arm.  -DAMINETXDUO_NXCACHE=OFF puts the per-call adopt/orphan
 * back, so the two libraries differ in this one decision and nothing else --
 * which is how the before and after in docs/RESEARCH.md were taken, and the
 * only way to keep taking them once the tree has moved on.
 */
#ifndef AMINETXDUO_NXCACHE
#  define AMINETXDUO_NXCACHE 1
#endif

/*
 * How many brackets a real client's fetch costs, and what fraction of its wall
 * clock they are. Not compiled in by default -- it adds two ReadEClock() calls
 * per bracket, which is precisely the thing being measured -- but the question
 * "is the bracket the cost?" has no answer without it, and answering it by
 * arithmetic on a throughput delta is how a prediction gets confirmed rather
 * than tested.
 *
 *   cmake -B build/census -DAMINETXDUO_NXCENSUS=ON ...
 *
 * The totals land in the serial log when the base is closed.
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

#if AMINETXDUO_NXCACHE
    if (ami_netstack_enter_cached(&base->sb_NxCaller) != AMI_NET_OK)
        return -1;
#else
    if (ami_netstack_enter(&base->sb_NxCaller) != AMI_NET_OK)
        return -1;
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
 * Give the cached TX_THREAD back.
 *
 * Called from bsd_child_destroy() (library.c) after the last socket is shut,
 * so it runs on the base's own task with no bracket open. A base that is torn
 * down by anyone else still gets the registration removed -- see
 * ami_netstack_release().
 */
VOID bsd_nx_release(struct AmiSocketBase *base)
{
    if (base == NULL)
        return;

#ifdef AMINETXDUO_NXCENSUS
    /* 709379 E-Clock ticks a second on PAL; report milliseconds so the number
       can be put next to a stopwatch without arithmetic. */
    AMI_INFO("bsdsocket: %ld brackets (%ld nested): enter %ld ms, leave %ld ms,"
             " %ld over 1 ms, worst %ld ms",
             (long)base->sb_NxCount, (long)base->sb_NxNested,
             (long)(base->sb_NxEnterTicks / 709UL),
             (long)(base->sb_NxLeaveTicks / 709UL),
             (long)base->sb_NxSlow,
             (long)(base->sb_NxWorst / 709UL));
#endif

    base->sb_NxNest = 0;

    ami_netstack_release(&base->sb_NxCaller);
}
