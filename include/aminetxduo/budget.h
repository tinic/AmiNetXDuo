/*
 * The receive step budget's shared half.  Everything here exists only under
 * AMINETXDUO_RXPROBE; a shipped build carries neither the code nor the calls.
 * SPDX-License-Identifier: MIT
 */
#ifndef AMINETXDUO_BUDGET_H
#define AMINETXDUO_BUDGET_H

#include <exec/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Where a baton hold ended.  Outside the probe guard so call sites compile in
 * every build; NETSTATUS_HOLDSITE_* in netstatus.h must stay in step.
 */
#define AMI_HOLD_SITE_YIELD    1   /* _tx_thread_system_return: blocked in TX */
#define AMI_HOLD_SITE_SUSPEND  2   /* tx_amiga_adopt_suspend: call returning  */
#define AMI_HOLD_SITE_DISCARD  3   /* tx_amiga_discard_thread: teardown       */
#define AMI_HOLD_SITE_ORPHAN   4   /* tx_amiga_orphan_thread: teardown        */
#define AMI_HOLD_SITE_BRACKET  5   /* baton release: about to Wait() in Exec  */
#define AMI_HOLD_SITE_REAP     6   /* scheduler took it back from a zombie    */

#ifdef AMINETXDUO_RXPROBE

/* Matches AMI_RXPROBE_BUCKETS; spelled out so this header stands alone. */
#define AMI_BUDGET_BUCKETS  20

/*
 * The holder's side of the baton leg.  One global stamp, because the baton
 * model admits exactly one holder at a time and every stamp happens under the
 * Forbid() the port already holds at its dispatch and release sites.
 */
#define AMI_BUDGET_HOLD_RING   16
#define AMI_BUDGET_HOLD_NAME   16

typedef struct AmiBudgetHold
{
    ULONG   seq;                    /* running count; 0 = empty slot       */
    ULONG   ticks;                  /* E-Clock ticks the baton was held    */
    ULONG   thread;                 /* the TX_THREAD's address             */
    UWORD   site;                   /* AMI_HOLD_SITE_*                     */
    UWORD   state;                  /* tx_thread_state at release          */
    char    name[AMI_BUDGET_HOLD_NAME];  /* copy of the thread name        */
} AmiBudgetHold;

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
    ULONG           pickup_at;      /* armed by pickup, taken by socket    */
    ULONG           socket_at;      /* armed by socket, taken by notify    */
    ULONG           xmit_at;        /* armed by socket, taken by tx_send   */
    AmiBudgetLeg    drain;          /* reader: reply dequeued -> delivered */
    AmiBudgetLeg    baton;          /* bsd_nx_enter(): asking to having     */
    AmiBudgetLeg    settle;         /* deliver -> receive notify           */
    /* Three chained sub-legs between the same deliver and notify, so their
       sum is settle whenever the chain stays on one frame. */
    AmiBudgetLeg    defer;          /* deliver -> the IP thread picks it up */
    AmiBudgetLeg    demux;          /* pickup -> the segment's own socket  */
    AmiBudgetLeg    state;          /* socket entry -> receive notify      */
    AmiBudgetLeg    fetch;          /* receive notify -> recv() returns    */
    AmiBudgetLeg    xmit;           /* socket entry -> the ACK's tx_send   */
    AmiBudgetLeg    ack;            /* CMD_WRITE BeginIO -> reply reaped   */
    AmiBudgetLeg    reap;           /* tx_send: the TX completion reap walk */
    AmiBudgetLeg    stuff;          /* tx_send: claim + framing + slot fill */
    AmiBudgetLeg    post;           /* tx_send: BeginIO enter -> return    */
    /*
     * THE RE-POST, WHICH IS THE BIGGEST THING IN `drain` NOBODY TIMES.
     *
     * `drain` reads 930 us across 2,241 samples against 1,285 frames -- about
     * 1,620 us a frame -- and `settle`, the whole IP-to-notify chain inside
     * it, is 462.  ELEVEN HUNDRED MICROSECONDS A FRAME OF THE READER'S LOOP
     * IS UNACCOUNTED, and the largest candidate in it is the CMD_READ this
     * function hands straight back to the device: ami_sana2_rx_post_slot()
     * allocates a packet, arms the slot and calls BeginIO, and the wire
     * profile puts it at 1.8-2.0%.
     *
     * The transmit side already has `post` for exactly this question.  This is
     * the receive side of it, and one subtraction then splits the eleven
     * hundred the way `hand` split the device's quarter-millisecond.
     */
    AmiBudgetLeg    repost;         /* rx re-arm: allocate, arm, BeginIO   */

    /* Which side of the direct-completion fork a receive took.  Plain
       counters, not legs: they answer coverage, not duration. */
    ULONG           rx_direct;
    ULONG           rx_fallback;

    /* The baton holder instrument.  hold_at is the acquisition stamp of the
       current holder; the rest accumulate.  hold_threshold is the ~50 ms
       gate in E-Clock ticks, derived from the measured rate on first use. */
    ULONG           hold_at;
    ULONG           hold_total;
    /*
     * HOW LONG THE BATON WAS HELD, WHICH IS WHAT SAYS WHETHER WAITING FOR IT
     * IS WASTE OR OVERLAP.
     *
     * `baton, asking to holding` in the receive budget measured 221 us across
     * 1,972 acquisitions -- 436 ms of a 3.56 s run, 12.2%, the largest ours
     * block left after the device closed.  A WAIT is not a cost if the holder
     * was doing the work the waiter is waiting for, and nothing here could
     * tell the two apart: hold_total is a COUNT, hold_max is one sample, and
     * there was no sum.
     *
     * With this, baton-total against hold-sum settles it.  If the holds fill
     * the run, the waits are overlap and the 12.2% is not available.
     */
    ULONG           hold_ticks;     /* summed duration of every hold         */
    ULONG           hold_slow;
    ULONG           hold_max;
    ULONG           hold_threshold;
    AmiBudgetHold   hold_ring[AMI_BUDGET_HOLD_RING];
} AmiBudget;

extern AmiBudget ami_budget;

/* The raw E-Clock low word, or 0 before timer.device is open; implemented
   beside the timer machinery in src/common/compat.c. */
ULONG ami_budget_clock(VOID);

VOID ami_budget_drain(ULONG dt);
VOID ami_budget_repost(ULONG dt);
VOID ami_budget_baton(ULONG dt);
VOID ami_budget_deliver(ULONG now);
VOID ami_budget_pickup(ULONG now);
VOID ami_budget_socket_enter(VOID);
VOID ami_budget_notify(ULONG now);
VOID ami_budget_fetch(ULONG now);
VOID ami_budget_xmit(ULONG now);
VOID ami_budget_ack(ULONG dt);
VOID ami_budget_reap(ULONG dt);
VOID ami_budget_stuff(ULONG dt);
VOID ami_budget_post(ULONG dt);
VOID ami_budget_rx_direct(VOID);
VOID ami_budget_rx_fallback(VOID);

/* Both must be called under Forbid(), which every dispatch and release site
   already holds; name and state are passed in so this header needs no
   TX_THREAD. */
VOID ami_budget_hold_start(VOID);
VOID ami_budget_hold_end(APTR thread, const char *name, ULONG state, UWORD site);

#else

/* Absent from the build: call sites compile to nothing through these. */
#define ami_budget_drain(dt)     ((VOID)0)
#define ami_budget_baton(dt)     ((VOID)0)
#define ami_budget_deliver(now)  ((VOID)0)
#define ami_budget_pickup(now)   ((VOID)0)
#define ami_budget_socket_enter() ((VOID)0)
#define ami_budget_notify(now)   ((VOID)0)
#define ami_budget_fetch(now)    ((VOID)0)
#define ami_budget_xmit(now)     ((VOID)0)
#define ami_budget_ack(dt)       ((VOID)0)
#define ami_budget_reap(dt)      ((VOID)0)
#define ami_budget_stuff(dt)     ((VOID)0)
#define ami_budget_post(dt)      ((VOID)0)
#define ami_budget_rx_direct()   ((VOID)0)
#define ami_budget_rx_fallback() ((VOID)0)
#define ami_budget_hold_start()                  ((VOID)0)
#define ami_budget_hold_end(th, nm, st, si)      ((VOID)0)

#endif /* AMINETXDUO_RXPROBE */

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_BUDGET_H */
