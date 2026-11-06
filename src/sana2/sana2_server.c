/*
 * AmiNetXDuo, the receive server: how much of the machine a busy SANA-II
 * reader takes at the band priority.
 *
 * WHAT IT IS FOR.  The readers run at TX_AMIGA_TASK_PRIORITY, one above the
 * programs, and with NX_TCP_PACKET_RECEIVE_DIRECT a reader also does the TCP
 * input of what it takes.  Under a sustained receive that a 68k cannot keep
 * up with, the reader is always ready, and Exec never runs a priority-0
 * program while a priority-1 Task is ready: Workbench, a file manager and an
 * audio decoder stop for as long as the transfer lasts.
 *
 * SO: a deferrable server.  While programs wait, the readers together hold
 * the band priority for about AMI_SANA2_RX_SERVER_SHARE /
 * AMI_SANA2_RX_SERVER_COST of what they drain, and run at
 * AMI_EXEC_PROGRAM_PRIORITY for the rest, sharing the CPU with the programs
 * round robin.  An idle machine loses nothing: a reader alone at priority 0
 * still has the CPU to itself, and a consumer that blocks gives it back.
 *
 * THE CHARGE IS SAMPLED, NOT TIMED.  Every ThreadX tick the tick task, which
 * preempts the band, looks at the bound readers: one that is ready (not
 * waiting) while a program that does not use the stack is ready at 0 too
 * wanted the CPU through that tick against it, and costs
 * AMI_SANA2_RX_SERVER_COST credits; each tick refills
 * AMI_SANA2_RX_SERVER_SHARE, up to AMI_SANA2_RX_SERVER_CAP.  A transfer with
 * nobody else waiting is never lowered: the sender or receiver at 0 is what
 * the reader works for.  A reader preempted by a driver above the band
 * counts as wanting the CPU, so a machine whose USB or audio drivers run high
 * lowers it sooner.  Reading a clock in the reader instead cost the A1200
 * 7-9% of its receive and send rate: an E-Clock read is CIA bus cycles, once
 * per pass.
 *
 * BELOW ZERO, A SLICE PER TICK.  A whole tick at the band priority is 20 ms,
 * and a decoder on the A1200 that waits that long behind a reader drops
 * audio (#9).  Each tick gives the readers SHARE/COST of the frames they
 * drained in the tick before, at the band priority, and they drop to 0 once
 * it is used: frames counted per pass, no clock.  The credit stays below zero
 * while a program keeps waiting, floored at -COST, and climbs back within a
 * few ticks of it leaving.
 *
 * THE LEVEL CHANGES AT SAFE POINTS.  The tick raises a lowered reader; a
 * reader lowers itself, at its baton release before it waits, or after a
 * pass with frames still queued, through a release so the band's other
 * threads can run (ami_sana2_server_settle()).  Only a reader at
 * TX_AMIGA_TASK_PRIORITY or AMI_EXEC_PROGRAM_PRIORITY is moved, so a raise
 * for the baton handoff and a priority a user set are kept.  Green builds
 * share one Exec Task between threads and bind nothing.
 *
 * SPDX-License-Identifier: MIT
 */

#include "sana2_internal.h"

#include "aminetxduo/nxstatus.h"
#include "tx_amiga.h"

#include <exec/execbase.h>
#include <exec/lists.h>
#include <exec/tasks.h>
#include <proto/exec.h>

#if AMI_SANA2_RX_SERVER_SHARE >= AMI_SANA2_RX_SERVER_COST
#error "AMI_SANA2_RX_SERVER_SHARE must leave the programs part of the CPU"
#endif

static struct
{
    struct MinList  readers;        /* AmiSana2Served, under Forbid()      */
    UWORD           bound;
    LONG            credit;
    volatile BOOL   low;            /* readers settle at priority 0        */
    BOOL            throttled;      /* credit spent: a slice per tick      */
    ULONG           frames;         /* drained, every reader, any priority */
    ULONG           frames_tick;    /* `frames` at the last tick           */
    ULONG           slice;          /* frames at the band priority a tick  */
    ULONG           used;           /* of the slice, since the last tick   */
    TX_TIMER        refill;
} ami_rx_server;

/* Is this Task's priority one the server set, or may set? */
static BOOL ami_sana2_server_ours(const struct Task *task)
{
    return (BOOL)(task->tc_Node.ln_Pri == (BYTE)TX_AMIGA_TASK_PRIORITY ||
                  task->tc_Node.ln_Pri == (BYTE)AMI_EXEC_PROGRAM_PRIORITY);
}

/* Is this Task a bound reader?  Forbid() held. */
static BOOL ami_sana2_server_bound(const struct Task *task)
{
    struct MinNode *n;

    for (n = ami_rx_server.readers.mlh_Head; n->mln_Succ != NULL;
         n = n->mln_Succ)
    {
        if (((AmiSana2Served *)n)->task == task)
            return TRUE;
    }
    return FALSE;
}

/*
 * Is a program waiting for the CPU that the band is keeping from it?  A ready
 * Task at the program priority that is neither a reader nor a caller of the
 * stack: a sender or receiver at 0 is what the reader works for, and lowering
 * the reader for it only delays its own data.  The ready list is walked under
 * Disable(), an interrupt's Signal() moves Tasks onto it, for at most
 * AMI_SANA2_RX_SERVER_WALK nodes; the candidates are compared against the
 * adoption slots after, by address only.  Forbid() held.
 */
static BOOL ami_sana2_server_victim(VOID)
{
    struct Task *cand[AMI_SANA2_RX_SERVER_WALK];
    struct Node *n;
    UWORD        k = 0, steps = 0, i;

    Disable();
    for (n = SysBase->TaskReady.lh_Head;
         n->ln_Succ != NULL && steps < (UWORD)AMI_SANA2_RX_SERVER_WALK;
         n = n->ln_Succ, steps++)
    {
        if (n->ln_Pri < (BYTE)AMI_EXEC_PROGRAM_PRIORITY)
            break;
        if (n->ln_Pri == (BYTE)AMI_EXEC_PROGRAM_PRIORITY &&
            !ami_sana2_server_bound((struct Task *)n))
            cand[k++] = (struct Task *)n;
    }
    Enable();

    for (i = 0; i < k; i++)
    {
        if (!tx_amiga_adopt_task_known_locked(cand[i]))
            return TRUE;
    }
    return FALSE;
}

/*
 * Every tick, from the tick task inside its Forbid() (TX_TIMER_PROCESS_IN_ISR):
 * charge, refill, and the next slice; a reader lowered by a spent slice goes
 * back up.
 */
static VOID ami_sana2_server_refill(ULONG unused)
{
    struct MinNode *n;
    BOOL            ready = FALSE;
    ULONG           total;

    (VOID)unused;

    Forbid();
    for (n = ami_rx_server.readers.mlh_Head; n->mln_Succ != NULL;
         n = n->mln_Succ)
    {
        if (((AmiSana2Served *)n)->task->tc_State == TS_READY)
            ready = TRUE;
    }

    if (ready && ami_sana2_server_victim())
        ami_rx_server.credit -= (LONG)AMI_SANA2_RX_SERVER_COST;
    ami_rx_server.credit += (LONG)AMI_SANA2_RX_SERVER_SHARE;
    if (ami_rx_server.credit > (LONG)AMI_SANA2_RX_SERVER_CAP)
        ami_rx_server.credit = (LONG)AMI_SANA2_RX_SERVER_CAP;
    if (ami_rx_server.credit < -(LONG)AMI_SANA2_RX_SERVER_COST)
        ami_rx_server.credit = -(LONG)AMI_SANA2_RX_SERVER_COST;

    total = ami_rx_server.frames - ami_rx_server.frames_tick;
    ami_rx_server.frames_tick = ami_rx_server.frames;
    ami_rx_server.throttled = (BOOL)(ami_rx_server.credit < 0);
    if (ami_rx_server.throttled)
    {
        ami_rx_server.slice = total / (ULONG)AMI_SANA2_RX_SERVER_COST *
                              (ULONG)AMI_SANA2_RX_SERVER_SHARE;
        if (ami_rx_server.slice < (ULONG)AMI_SANA2_RX_RUN_MAX)
            ami_rx_server.slice = (ULONG)AMI_SANA2_RX_RUN_MAX;
        ami_rx_server.used = 0;
    }

    if (ami_rx_server.low)
    {
        ami_rx_server.low = FALSE;
        for (n = ami_rx_server.readers.mlh_Head; n->mln_Succ != NULL;
             n = n->mln_Succ)
        {
            struct Task *task = ((AmiSana2Served *)n)->task;

            if (task->tc_Node.ln_Pri == (BYTE)AMI_EXEC_PROGRAM_PRIORITY)
                (VOID)SetTaskPri(task, (LONG)TX_AMIGA_TASK_PRIORITY);
        }
    }
    Permit();
}

/* The reader's own context, before its loop. */
VOID ami_sana2_server_bind(AmiSana2Reader *rd)
{
    AmiSana2Served *s = &rd->served;

    s->task = NULL;
    if ((rd->thread.tx_thread_amiga_flags & TX_AMIGA_THREAD_GREEN) != 0U)
        return;

    if (ami_rx_server.bound == 0)
    {
        /* NewList() is amiga.lib's. */
        ami_rx_server.readers.mlh_Head     =
            (struct MinNode *)&ami_rx_server.readers.mlh_Tail;
        ami_rx_server.readers.mlh_Tail     = NULL;
        ami_rx_server.readers.mlh_TailPred =
            (struct MinNode *)&ami_rx_server.readers.mlh_Head;
        ami_rx_server.credit = (LONG)AMI_SANA2_RX_SERVER_CAP;
        ami_rx_server.low    = FALSE;
        ami_rx_server.throttled = FALSE;
        ami_rx_server.frames = ami_rx_server.frames_tick = 0;
        if (tx_timer_create(&ami_rx_server.refill,
                            (CHAR *)"AmiNetXDuo rx server",
                            ami_sana2_server_refill, 0, 1, 1,
                            TX_NO_ACTIVATE) != TX_SUCCESS)
            return;
    }

    s->task = rd->task;
    Forbid();
    AddTail((struct List *)&ami_rx_server.readers, (struct Node *)&s->node);
    ami_rx_server.bound++;
    Permit();

    /* Created just above, inactive: TX_SUCCESS is its one outcome. */
    if (ami_rx_server.bound == 1)
        AMI_NX_ONLY_SUCCESS(tx_timer_activate(&ami_rx_server.refill));
}

/* The reader's own context, after its loop: back at the band priority. */
VOID ami_sana2_server_unbind(AmiSana2Reader *rd)
{
    AmiSana2Served *s = &rd->served;
    BOOL            last;

    if (s->task == NULL)
        return;

    Forbid();
    Remove((struct Node *)&s->node);
    last = (BOOL)(--ami_rx_server.bound == 0);
    if (s->task->tc_Node.ln_Pri == (BYTE)AMI_EXEC_PROGRAM_PRIORITY)
        (VOID)SetTaskPri(s->task, (LONG)TX_AMIGA_TASK_PRIORITY);
    Permit();
    s->task = NULL;

    if (last)
    {
        AMI_NX_CLEANUP(tx_timer_deactivate(&ami_rx_server.refill));
        AMI_NX_CLEANUP(tx_timer_delete(&ami_rx_server.refill));
    }
}

/*
 * Baton held, work about to start.  The park that got the baton back
 * restores the priority it found on the way in, which may be the 0 of a
 * budget the tick has since refilled.
 */
VOID ami_sana2_server_run(AmiSana2Reader *rd)
{
    struct Task *task = rd->served.task;

    if (task != NULL && !ami_rx_server.low &&
        task->tc_Node.ln_Pri == (BYTE)AMI_EXEC_PROGRAM_PRIORITY)
        (VOID)SetTaskPri(task, (LONG)TX_AMIGA_TASK_PRIORITY);
}

/*
 * After a pass that drained `took` frames, baton held: TRUE when the slice
 * is spent and this reader still runs at the band priority, so the caller
 * should stand aside (ami_sana2_server_settle()) rather than start another
 * pass.  The readers count, the tick reads `frames` and resets `used`.
 */
BOOL ami_sana2_server_spent(AmiSana2Reader *rd, UWORD took)
{
    struct Task *task = rd->served.task;
    BOOL         band;

    if (task == NULL)
        return FALSE;

    band = (BOOL)(task->tc_Node.ln_Pri == (BYTE)TX_AMIGA_TASK_PRIORITY);
    ami_rx_server.frames += took;
    if (ami_rx_server.throttled && band)
    {
        ami_rx_server.used += took;
        if (ami_rx_server.used >= ami_rx_server.slice)
            ami_rx_server.low = TRUE;
    }
    return (BOOL)(band && ami_rx_server.low);
}

/*
 * Baton released: the level decides where this reader waits and runs until
 * it changes.  Lowering yields to a ready program at once.
 */
VOID ami_sana2_server_settle(AmiSana2Reader *rd)
{
    struct Task *task = rd->served.task;
    BYTE         want;

    if (task == NULL)
        return;

    Forbid();
    want = ami_rx_server.low ? (BYTE)AMI_EXEC_PROGRAM_PRIORITY
                             : (BYTE)TX_AMIGA_TASK_PRIORITY;
    if (task->tc_Node.ln_Pri != want && ami_sana2_server_ours(task))
        (VOID)SetTaskPri(task, (LONG)want);
    Permit();
}
