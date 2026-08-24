/*
 * The receive step budget's shared half: the settle and fetch legs, stamped
 * from three different tasks and aggregated in one place.  src/common/budget.c
 * says why the shape is a single latest-stamp, and src/sana2/sana2_rx.c holds
 * the drain leg this completes.
 *
 * Everything here exists only under AMINETXDUO_RXPROBE; a shipped build
 * carries neither the code nor the calls.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AMINETXDUO_BUDGET_H
#define AMINETXDUO_BUDGET_H

#include <exec/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef AMINETXDUO_RXPROBE

/* Matches AMI_RXPROBE_BUCKETS; spelled out so this header stands alone. */
#define AMI_BUDGET_BUCKETS  20

typedef struct AmiBudgetLeg
{
    ULONG   count;
    ULONG   sum;
    ULONG   max;
    ULONG   hist[AMI_BUDGET_BUCKETS];
} AmiBudgetLeg;

typedef struct AmiBudget
{
    ULONG           deliver_at;     /* armed by deliver, taken by notify   */
    ULONG           notify_at;      /* armed by notify, taken by fetch     */
    AmiBudgetLeg    drain;          /* reader: reply dequeued -> delivered */
    AmiBudgetLeg    baton;          /* bsd_nx_enter(): asking to having     */
    AmiBudgetLeg    settle;         /* deliver -> receive notify           */
    AmiBudgetLeg    fetch;          /* receive notify -> recv() returns    */
} AmiBudget;

extern AmiBudget ami_budget;

/* The raw E-Clock low word, or 0 before timer.device is open; implemented
   beside the timer machinery in src/common/compat.c. */
ULONG ami_budget_clock(VOID);

VOID ami_budget_drain(ULONG dt);
VOID ami_budget_baton(ULONG dt);
VOID ami_budget_deliver(ULONG now);
VOID ami_budget_notify(ULONG now);
VOID ami_budget_fetch(ULONG now);

#else

/* Absent from the build: call sites compile to nothing through these. */
#define ami_budget_drain(dt)     ((VOID)0)
#define ami_budget_baton(dt)     ((VOID)0)
#define ami_budget_deliver(now)  ((VOID)0)
#define ami_budget_notify(now)   ((VOID)0)
#define ami_budget_fetch(now)    ((VOID)0)

#endif /* AMINETXDUO_RXPROBE */

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_BUDGET_H */
