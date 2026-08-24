/*
 * The receive step budget: where one frame's time goes between the reader and
 * the application, as E-Clock aggregates per hop.
 *
 * docs/PHYSICAL_RX_A1200.md profiled the affected machine to 5.7% idle, both
 * required copies at 19.4% of busy time, and no third bulk pass -- which
 * leaves the per-frame chain itself holding the unattributed remainder.  The
 * chain is not one function, so no sampler names it; it is a sequence of
 * handoffs, and what this module records is the time between them:
 *
 *   settle   ami_budget_deliver() to ami_budget_notify(): the frame left the
 *            SANA-II reader for the IP thread, and the socket's receive
 *            notify fired for it.  IP-thread queueing, TCP processing and
 *            the socket handoff, in one number.
 *   fetch    ami_budget_notify() to ami_budget_fetch(): the notify to the
 *            application's recv() returning with data.  The wakeup chain and
 *            recv's own dequeue.
 *
 * The drain leg -- the reader's own per-frame cost -- lives in the SANA-II
 * probe (src/sana2/sana2_rx.c), which owns both of its ends.  The ack leg is
 * the transmit half the first four never see: a CMD_WRITE handed to the
 * device until its reply is reaped (src/sana2/sana2_tx.c owns both ends,
 * per slot rather than latest-stamp, because writes overlap by design).
 * During a receive every write is an ACK, which is what names it.  The ack
 * leg spans wall time and mostly measures when the reply happens to be
 * collected; the push leg is its CPU counterpart, the driver entry's send
 * case from entry to return (src/sana2/sana2_driver.c), which is what an
 * acknowledgment actually costs this machine to emit.
 *
 * WHY A SINGLE LATEST-STAMP AND NOT A MATCHED QUEUE.  The regime this
 * measures is the serial one, one frame walked through the whole chain at a
 * time, because that is the regime the physical machine is in when it is
 * slow.  A deliver that is followed by another deliver before the notify
 * fires simply overwrites the stamp: the earlier pair is not sampled.  Under
 * bursts the figure therefore under-counts on purpose -- fewer samples, none
 * of them fabricated -- and a matched queue would buy those extra samples at
 * the price of a search structure on the hot path of exactly the machine
 * that cannot afford one.
 *
 * The stamps cross task contexts (the reader, the IP thread, the caller of
 * recv()) and are single ULONG stores and reads of a monotonically read
 * clock, so the worst a race produces is one discarded or one nonsense
 * sample; a nonsense delta larger than AMI_BUDGET_CEILING is thrown away
 * rather than folded into a maximum it would own forever.
 *
 * Compiled under AMINETXDUO_RXPROBE only, the same switch as the rest of the
 * receive instrumentation, and holds no strings, in keeping with
 * tools/check-no-diag-strings.sh.
 *
 * SPDX-License-Identifier: MIT
 */

#include "aminetxduo/budget.h"

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
}

VOID ami_budget_notify(ULONG now)
{
    ULONG opened = ami_budget.deliver_at;

    if (opened != 0UL)
    {
        ami_budget.deliver_at = 0UL;
        ami_budget_leg(&ami_budget.settle, now - opened);
    }

    ami_budget.notify_at = now;
}

VOID ami_budget_ack(ULONG dt)
{
    ami_budget_leg(&ami_budget.ack, dt);
}

VOID ami_budget_push(ULONG dt)
{
    ami_budget_leg(&ami_budget.push, dt);
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

#endif /* AMINETXDUO_RXPROBE */
