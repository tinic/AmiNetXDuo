/* AmiNetXDuo, private glue shared between the ThreadX Exec port sources.
   Include "tx_api.h" first: ThreadX's VOID/ULONG typedefs must beat those of
   <exec/types.h>.  SPDX-License-Identifier: MIT  */

#ifndef TX_AMIGA_INTERNAL_H
#define TX_AMIGA_INTERNAL_H

#include "tx_api.h"
#include "tx_thread.h"
#include "tx_timer.h"
#include "tx_amiga.h"
#include "aminetxduo/nxstatus.h"

#include <exec/types.h>
#include <exec/nodes.h>
#include <exec/lists.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <exec/execbase.h>
#include <proto/exec.h>

/* The baton-holder side of the receive step budget (a no-op outside
   AMINETXDUO_RXPROBE builds).  */
#include "aminetxduo/budget.h"


/* Open-coded NewList(): amiga.lib is not linkable into a shared library.
   Same pattern as src/common/compat.c.  */
static __inline VOID _tx_amiga_newlist(struct List *list)
{
    list -> lh_Head     = (struct Node *) &list -> lh_Tail;
    list -> lh_Tail     = (struct Node *) 0;
    list -> lh_TailPred = (struct Node *) &list -> lh_Head;
}


/* Per-Task control block, registered in tc_MemEntry so it outlives the TX_THREAD:
   a task the reaper gave up on must be able to destroy itself from memory it
   owns.  ctrl_task is at offset 0, so a struct Task * IS the control block.  */

#define TX_AMIGA_CTRL_MAGIC     0x54584143UL        /* 'TXAC'  */

struct _tx_amiga_ctrl
{
    struct Task      ctrl_task;                     /* MUST be first          */
    ULONG            ctrl_magic;
    volatile UINT    ctrl_die;                      /* teardown requested     */
    TX_THREAD       *ctrl_thread;                   /* TX_NULL once detached  */
    struct Task     *ctrl_reaper;
    ULONG            ctrl_reaper_signal;
    volatile ULONG  *ctrl_reaped;                   /* flag on reaper's stack */
    volatile UINT    ctrl_zombie;                   /* reaper gave up on it   */
};


/* THE LIVE BATON HOLDER, as an Exec Task address, or 0 when the baton is free.
   Written wherever _tx_thread_current_ptr is, which is why every one of those
   sites goes through ami_baton_note() rather than assigning directly.

   It exists so a harness OUTSIDE this program can see the precondition it has
   to arm on -- "the victim holds the baton right now" -- and remove the victim
   inside the same Forbid() that observed it.  Before this, the concurrent
   reclaim test removed its victim after a loop count and missed the bracket
   about one run in four.  tests/concurrent reaches it through the health mark's
   hm_Holder.

   A thread that is current is PUBLISHED by construction: publication happens
   inside the create's own Forbid(), before the thread can be dispatched or take
   the baton on the fast path.  So this one word carries both conditions.

   It costs one load and one store per baton handover, on the scheduler path and
   not on the packet path.  */
extern VOID *_tx_amiga_baton_holder_task;


/* An identity for a Task that survives its address being recycled, never 0.
   tx_thread_interrupt_control.c owns the Task layout it reads.  */
ULONG _tx_amiga_task_stamp(struct Task *task);


/* Is `task` on one of Exec's scheduler lists, or is it us?  Caller holds
   Disable(): an interrupt moves a task between TaskWait and TaskReady even
   while scheduling is forbidden.  NOTHING inside the struct is read, so this is
   the one question that may be asked about a Task that may already be freed.
   tx_thread_interrupt_control.c.  */
UINT tx_amiga_task_alive_locked(struct Task *task);


/* ---------------------------------------------- the adoption slot pool ---
   tx_amiga_pool.c.  The port owns the TX_THREAD of every adopted Exec Task;
   that file's head says why, and the public handle calls are in tx_amiga.h.  */

struct _tx_amiga_adopt_slot
{
    TX_THREAD       as_thread;          /* MUST BE FIRST: a TX_THREAD * IS a slot */
    ULONG           as_generation;      /* 0 while free, never reused while busy  */
    UINT            as_busy;
    /* Who took the slot, and whether the TX_THREAD in it exists yet.  A claim
       and the _tx_thread_create() that fills it are not one atom: the claimer
       has to leave Forbid() in between.  A Task removed in that window would
       leak a busy slot for ever, so the claimer is recorded here and the tick's
       sweep gives the slot back.  Cleared the moment the slot is published.
       as_claim_stamp is _tx_amiga_task_stamp() of the claimer, taken from its
       own context at the claim: the address alone is not the claimer once
       Exec has recycled it.  */
    struct Task    *as_claimer;
    ULONG           as_claim_stamp;
    UINT            as_published;
};

/* Claim: Forbid() held, 0 when the pool is full.  Park: Forbid() NOT held, it
   waits, and 0 means the waiter table was full or the kernel went away.  */
struct _tx_amiga_adopt_slot *_tx_amiga_slot_claim_locked(UINT reserved);
struct _tx_amiga_adopt_slot *_tx_amiga_slot_claim_or_park(struct Task *me, UINT reserved);

/* The slot behind a handle that still names this adoption, or 0.  Forbid()
   held.  _tx_amiga_slot_release_locked() and the waiter wakes are declared in
   tx_port.h, with the port's other VOID helpers.  */
struct _tx_amiga_adopt_slot *_tx_amiga_slot_held(TX_THREAD *thread_ptr, ULONG generation);
BYTE _tx_amiga_sigbit(ULONG sigmask);


/* Create an Exec Task on a caller-supplied stack.  The MemList is a SEPARATE
   allocation: RemTask() hands it to FreeEntry(), which frees both the entries it
   describes and the MemList itself.  The stack is not owned by the task.  */
struct Task *_tx_amiga_task_create(CHAR *name, BYTE priority, VOID (*entry)(VOID),
                                   APTR stack, ULONG stack_size, APTR user_data);


/* The control block of a Task the port created, or NULL.  Safe on any Task:
   it reads nothing outside the struct Task until tc_UserData has identified
   the block as ours.  */
static __inline struct _tx_amiga_ctrl *_tx_amiga_ctrl_of(struct Task *task)
{
struct _tx_amiga_ctrl   *ctrl;

    if ((task != (struct Task *) 0) && (task -> tc_UserData == (APTR) task))
    {
        ctrl =  (struct _tx_amiga_ctrl *) task;
        if (ctrl -> ctrl_magic == TX_AMIGA_CTRL_MAGIC)
        {
            return(ctrl);
        }
    }
    return((struct _tx_amiga_ctrl *) 0);
}

/* The ONE writer of _tx_thread_current_ptr in this port.  Every baton handover
   goes through it so the mirror above cannot drift from the pointer it
   mirrors.  Caller holds the core lock, as it did for the bare assignment.  */
static __inline VOID ami_baton_note(TX_THREAD *thread_ptr)
{
    _tx_thread_current_ptr      =  thread_ptr;
    _tx_amiga_baton_holder_task =  (thread_ptr != TX_NULL)
                                   ? thread_ptr -> tx_thread_amiga_task
                                   : (VOID *) 0;
}


/* Signal helper that tolerates a NULL task pointer.  */
static __inline VOID _tx_amiga_signal(APTR task, ULONG sigmask)
{
    if ((task != (APTR) 0) && (sigmask != 0UL))
    {
        Signal((struct Task *) task, sigmask);
    }
}

/* TX_TRUE while the calling task is inside a Forbid().  */
static __inline UINT _tx_amiga_forbidden(VOID)
{
    return ((SysBase -> TDNestCnt >= 0) ? ((UINT) TX_INT_DISABLE) : ((UINT) TX_INT_ENABLE));
}


/* Set by tx_amiga_kernel_stop() once its preconditions are met, cleared only when
   the kernel is fully down: _tx_thread_schedule() returns instead of dispatching,
   and tx_amiga_adopt_thread() refuses.  */
extern volatile UINT    _tx_amiga_kernel_stopping;

/* Hand the baton straight to the next thread instead of poking the scheduler
   Task.  Call with the core lock held and the baton already released.  TX_TRUE if
   a thread was dispatched; TX_FALSE means fall back to _tx_amiga_wake_scheduler(). */
static __inline UINT _tx_amiga_dispatch_inline(VOID)
{
TX_THREAD   *thread_ptr;


    thread_ptr =  _tx_thread_execute_ptr;

    if ((thread_ptr == TX_NULL) ||
        (_tx_thread_current_ptr != TX_NULL) ||
        (_tx_thread_system_state != ((ULONG) 0)) ||
        (_tx_amiga_kernel_stopping != TX_FALSE))
    {
        return((UINT) TX_FALSE);
    }


    ami_baton_note(thread_ptr);
    thread_ptr -> tx_thread_run_count++;
    _tx_timer_time_slice =  thread_ptr -> tx_thread_time_slice;

    thread_ptr -> tx_thread_amiga_suspension_type =  ((UINT) 0);

    ami_budget_hold_start();

    _tx_amiga_signal(thread_ptr -> tx_thread_amiga_task,
                     thread_ptr -> tx_thread_amiga_run_signal);

    TX_AMIGA_COUNT(TX_AMIGA_SC_DIRECT);

    return((UINT) TX_TRUE);
}


/* Does a failed _tx_amiga_dispatch_inline() need the scheduler Task poking?  Only
   when the scheduler could do what the caller could not -- a held baton is handed
   on by its holder.  Call with the core lock held.  */
static __inline UINT _tx_amiga_wake_needed(UINT dispatched)
{
    if (dispatched != ((UINT) TX_FALSE))
    {
        return((UINT) TX_FALSE);
    }
    if (_tx_thread_execute_ptr == TX_NULL)
    {
        return((UINT) TX_FALSE);
    }
    if (_tx_thread_current_ptr != TX_NULL)
    {
        return((UINT) TX_FALSE);
    }
    return((UINT) TX_TRUE);
}


/* Park the calling Exec Task until it holds the ThreadX baton (defined in
   tx_thread_system_return.c).  TX_FALSE only for an adopted thread torn down
   under it, which must then unwind; for a port-created Task it never returns.  */
UINT _tx_amiga_thread_park(TX_THREAD *thread_ptr);

/* Hand the released baton directly to the next thread where possible.
   Call with Forbid() held and no current baton owner.  TX_TRUE means the
   scheduler task must be woken after Permit(). */
UINT _tx_amiga_dispatch_or_wake(VOID);


/* Destroy the calling Exec Task (one the port created).  Never returns.  Touches
   only the task's own control block, so it is safe even for a task the reaper had
   to abandon.  */
VOID _tx_amiga_task_destroy(struct _tx_amiga_ctrl *ctrl);


/* TX_THREAD_COMPLETED_EXTENSION.  Runs on the thread's own Exec Task the instant
   its entry function returns, before _tx_thread_system_suspend() does any
   ready-list surgery.  */
VOID _tx_amiga_thread_completed(VOID);


/* Add `ticks` to _tx_timer_system_clock without walking the timer wheel
   (tx_timer_interrupt.c).  The only writer of the ThreadX clock in this port.  */
VOID _tx_amiga_timer_clock_advance(ULONG ticks);

/* The shared tick service (tx_initialize_low_level.c): everything one tick wakeup
   does, E-Clock-based and idempotent, called by the tick task (every build) and by
   the realm's scheduler loop (green builds).  Runs under its own Forbid().  */
VOID _tx_amiga_tick_deliver(UINT from_realm);

/* Its state, one instance in tx_initialize_low_level.c.  The scheduler loop reads
   tr_realm under Forbid(); everything else is the service's own.  */
struct _tx_amiga_tick_run
{
    ULONG   tr_eclock_hz;
    ULONG   tr_eclock_per_ms;
    ULONG   tr_eclock_per_tick;
    ULONG   tr_eclock_rem;
    ULONG   tr_frac;
    ULONG   tr_backlog;
    ULONG   tr_last_lo;
    ULONG   tr_up_lo;
    ULONG   tr_up_rem;
    ULONG   tr_last_service;
    ULONG   tr_worst_delta;
    UINT    tr_live;            /* parameters valid; service may run        */
    UINT    tr_realm;           /* green: the realm is the wakeup target    */
};

extern struct _tx_amiga_tick_run    _tx_amiga_tick_run;


/* Port globals defined in tx_initialize_low_level.c.  */
extern volatile UINT    _tx_amiga_kernel_up;
extern volatile UINT    _tx_amiga_timer_stop;
extern volatile ULONG   _tx_amiga_zombies;

/* Zombies that have not yet unblocked and destroyed themselves.  Goes back down as
   each reaches _tx_amiga_task_destroy(); tx_amiga_kernel_stop() requires zero, and
   a program cannot be unloaded while one is still parked in Exec.  */
extern volatile ULONG   _tx_amiga_zombies_live;




/* Temporary lifecycle tracing.  Define TX_AMIGA_TRACE to route to ami_log().  */
#ifdef TX_AMIGA_TRACE
extern void ami_log(int level, const char *fmt, ...);
#define TXTRACE(...)    ami_log(0, __VA_ARGS__)
#else
#define TXTRACE(...)    ((void) 0)
#endif


#endif /* TX_AMIGA_INTERNAL_H */
