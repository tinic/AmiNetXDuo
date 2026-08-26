/*
 * The receive step budget: where one frame's time goes between the reader and
 * the application, as E-Clock aggregates per hop.
 *
 * Compiled under AMINETXDUO_RXPROBE only, the same switch as the rest of the
 * receive instrumentation, and holds no strings, in keeping with
 * tools/check-no-diag-strings.sh.
 *
 * SPDX-License-Identifier: MIT
 */

#include "aminetxduo/budget.h"
#include "aminetxduo/compat.h"

#ifdef AMINETXDUO_RXPROBE

/* One E-Clock second; a hop longer than this is a lost pairing, not a hop. */
#define AMI_BUDGET_CEILING  710000UL

AmiBudget ami_budget;

static VOID ami_budget_leg(AmiBudgetLeg *leg, ULONG dt)
{
    UWORD b = 0;
    ULONG v = dt;

    if (dt > AMI_BUDGET_CEILING)
        return;

    leg->count++;
    leg->sum += dt;
    if (dt > leg->max)
        leg->max = dt;

    while (v != 0UL && b < (UWORD)(AMI_BUDGET_BUCKETS - 1))
    {
        v >>= 1;
        b++;
    }
    leg->hist[b]++;
}

VOID ami_budget_drain(ULONG dt)
{
    ami_budget_leg(&ami_budget.drain, dt);
}

VOID ami_budget_baton(ULONG dt)
{
    ami_budget_leg(&ami_budget.baton, dt);
}

VOID ami_budget_deliver(ULONG now)
{
    ami_budget.deliver_at = now;

    /* A fresh chain: a pickup or socket stamp still armed belongs to a frame
       that never notified (out-of-order queueing, a drop, a foreign socket).
       Clearing them here keeps a stale boundary from pairing with this
       frame's; the sample it would have made is discarded, per the module
       rule above. */
    ami_budget.pickup_at = 0UL;
    ami_budget.socket_at = 0UL;
    ami_budget.xmit_at   = 0UL;
}

/*
 * The IP thread holds the packet: close the defer sub-leg, open demux.  Only
 * a chain deliver armed is continued -- the filter this is called from sees
 * every inbound IP packet, and a bare ACK or a foreign protocol must not
 * inherit a data segment's stamp.  deliver_at is read, not consumed: the
 * whole settle pair still closes at the notify.
 */
VOID ami_budget_pickup(ULONG now)
{
    ULONG opened = ami_budget.deliver_at;

    if (opened == 0UL)
        return;

    ami_budget_leg(&ami_budget.defer, now - opened);
    ami_budget.pickup_at = now;
}

/*
 * The segment reached its socket: close demux, open state.  Called from the
 * one probe hook inside the NetX Duo fork (nx_tcp_socket_packet_process.c),
 * which is why this one reads the clock itself rather than taking it -- the
 * fork's call site stays a single argumentless line.
 */
VOID ami_budget_socket_enter(VOID)
{
    ULONG opened = ami_budget.pickup_at;
    ULONG now;

    if (opened == 0UL)
        return;

    now = ami_budget_clock();
    if (now == 0UL)
        return;

    ami_budget.pickup_at = 0UL;
    ami_budget_leg(&ami_budget.demux, now - opened);
    ami_budget.socket_at = now;
    ami_budget.xmit_at   = now;
}

/*
 * The transmit half of a received segment: socket entry to the driver call the
 * ACK it provoked arrives in.  One stamp, consumed once, so a segment that
 * emitted nothing costs the next transmit no sample rather than a wrong one.
 */
VOID ami_budget_xmit(ULONG now)
{
    ULONG opened = ami_budget.xmit_at;

    if (opened == 0UL)
        return;

    ami_budget.xmit_at = 0UL;
    ami_budget_leg(&ami_budget.xmit, now - opened);
}

VOID ami_budget_notify(ULONG now)
{
    ULONG opened = ami_budget.deliver_at;

    if (opened != 0UL)
    {
        ami_budget.deliver_at = 0UL;
        ami_budget_leg(&ami_budget.settle, now - opened);
    }

    /* The state sub-leg ends where settle ends. */
    opened = ami_budget.socket_at;
    if (opened != 0UL)
    {
        ami_budget.socket_at = 0UL;
        ami_budget_leg(&ami_budget.state, now - opened);
    }

    ami_budget.notify_at = now;
}

VOID ami_budget_ack(ULONG dt)
{
    ami_budget_leg(&ami_budget.ack, dt);
}

VOID ami_budget_reap(ULONG dt)
{
    ami_budget_leg(&ami_budget.reap, dt);
}

VOID ami_budget_stuff(ULONG dt)
{
    ami_budget_leg(&ami_budget.stuff, dt);
}

VOID ami_budget_post(ULONG dt)
{
    ami_budget_leg(&ami_budget.post, dt);
}

VOID ami_budget_rx_direct(VOID)
{
    ami_budget.rx_direct++;
}

VOID ami_budget_rx_fallback(VOID)
{
    ami_budget.rx_fallback++;
}

VOID ami_budget_fetch(ULONG now)
{
    ULONG opened = ami_budget.notify_at;

    if (opened != 0UL)
    {
        ami_budget.notify_at = 0UL;
        ami_budget_leg(&ami_budget.fetch, now - opened);
    }
}

/*
 * The baton holder's side.  aminetxduo/budget.h says why one stamp suffices;
 * both entry points run under the caller's Forbid(), so the stamp, the
 * counters and the ring never race.  A stamp of zero means no clock yet, and
 * doubles as "not stamped", the same convention as the hop stamps above.
 */
VOID ami_budget_hold_start(VOID)
{
    ami_budget.hold_at = ami_budget_clock();
}

VOID ami_budget_hold_end(APTR thread, const char *name, ULONG state, UWORD site)
{
    ULONG          at = ami_budget.hold_at;
    ULONG          now;
    ULONG          dt;
    ULONG          thr;
    AmiBudgetHold *slot;
    UWORD          i;

    ami_budget.hold_at = 0UL;
    if (at == 0UL)
        return;

    now = ami_budget_clock();
    if (now == 0UL)
        return;

    dt = now - at;
    if (dt > AMI_BUDGET_CEILING)
        return;

    ami_budget.hold_total++;

    /* ~50 ms in E-Clock ticks, from the measured rate rather than the PAL
       constant an NTSC machine would be 1% wrong by.  Cached: the rate never
       changes once timer.device has answered. */
    thr = ami_budget.hold_threshold;
    if (thr == 0UL)
    {
        thr = ami_eclock_rate() / 20UL;
        if (thr == 0UL)
            return;
        ami_budget.hold_threshold = thr;
    }

    if (dt <= thr)
        return;

    ami_budget.hold_slow++;
    if (dt > ami_budget.hold_max)
        ami_budget.hold_max = dt;

    slot = &ami_budget.hold_ring[(ami_budget.hold_slow - 1UL) %
                                 (ULONG)AMI_BUDGET_HOLD_RING];
    slot->seq    = ami_budget.hold_slow;
    slot->ticks  = dt;
    slot->thread = (ULONG)thread;
    slot->site   = site;
    slot->state  = (UWORD)state;

    /* The name is copied at record time, while the holder is provably alive
       under this Forbid(); by the time a reader asks, the TX_THREAD behind an
       adopted caller may be long gone. */
    for (i = 0; i < (UWORD)(AMI_BUDGET_HOLD_NAME - 1); i++)
    {
        if (name == NULL || name[i] == '\0')
            break;
        slot->name[i] = name[i];
    }
    slot->name[i] = '\0';
}

#endif /* AMINETXDUO_RXPROBE */
