/* AmiNetXDuo, private glue shared between the ThreadX Exec port sources.
   Include "tx_api.h" first: ThreadX's VOID/ULONG typedefs must beat those of
   <exec/types.h>.  SPDX-License-Identifier: MIT  */

#ifndef TX_AMIGA_INTERNAL_H
#define TX_AMIGA_INTERNAL_H

#include "tx_api.h"
#include "tx_thread.h"
#include "tx_timer.h"
#include "tx_amiga.h"

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

#ifdef AMINETXDUO_GREEN_REALM
    /* A green thread's context can only be entered by a stack switch, and only
       the realm Task's scheduler loop may perform one -- this inline runs on
       whatever Task is yielding.  Decline, so the caller pokes the realm.  */
    if ((thread_ptr -> tx_thread_amiga_flags & TX_AMIGA_THREAD_GREEN) != 0U)
    {
        return((UINT) TX_FALSE);
    }
#endif

    _tx_thread_current_ptr =  thread_ptr;
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


#ifdef AMINETXDUO_GREEN_REALM

/* ------------------------------------------------------- the green realm --- */

/* The green realm (tx_amiga_green.c, tx_green_switch.S): every ThreadX thread the
   stack creates runs as a coroutine INSIDE the realm Task, with a real m68k
   context switch.  Adopted threads keep their Exec Task and the baton protocol. */

/* TX_TRUE if the thread's context is a green (realm-internal) one.  */
static __inline UINT _tx_amiga_thread_green(TX_THREAD *thread_ptr)
{
    return(((thread_ptr -> tx_thread_amiga_flags & TX_AMIGA_THREAD_GREEN) != 0U)
           ? ((UINT) TX_TRUE) : ((UINT) TX_FALSE));
}

/* The realm scheduler's saved context.  A green thread yields by saving its own
   SP into tx_thread_stack_ptr and loading this one.  */
extern APTR     _tx_green_scheduler_sp;

/* The switch itself (tx_green_switch.S): saves d2-d7/a2-a6 and the return address,
   stores SP through save_slot, loads new_sp.  PROTOCOL: called with exactly one
   Forbid() held; the resumed side performs the matching Permit().  */
VOID    _tx_green_switch(APTR *save_slot, APTR new_sp);

/* First activation target of a green thread (tx_amiga_green.c): the initial
   frame's return address.  Permit()s the dispatcher's Forbid, then runs
   _tx_thread_shell_entry().  */
VOID    _tx_green_thread_begin(VOID);

/* Build the initial switch frame on a green thread's stack (tx_amiga_green.c).  */
VOID    _tx_green_stack_build(TX_THREAD *thread_ptr);

/* Green Exec-signal waits.  A green thread registers its mask and suspends; the
   realm's idle Wait() covers the union of registered masks and the scheduler
   delivers latched bits before every dispatch pass.  */
ULONG   _tx_green_pending_union(VOID);          /* call under Forbid()        */
VOID    _tx_green_deliver(ULONG sigs);          /* call under Forbid()        */
VOID    _tx_green_forget(TX_THREAD *thread_ptr);/* call under Forbid()        */

/* Scheduling counters for the prototype's measurement arm (always compiled
   in green builds; a handful of increments under Forbid).  */
struct _tx_green_counters
{
    ULONG   gc_switches;        /* green contexts entered (stack switches in)  */
    ULONG   gc_external;        /* baton handoffs to non-green (adopted) ones  */
    ULONG   gc_idle_waits;      /* realm slept in Wait()                       */
    ULONG   gc_wait_fast;       /* tx_amiga_green_wait() satisfied latched     */
    ULONG   gc_wait_slow;       /* tx_amiga_green_wait() suspended             */
    ULONG   gc_stray_wait;      /* blocking Exec waits caught from green       */
    ULONG   gc_gate_calls;      /* brackets carried through the request gate   */
    ULONG   gc_gate_fallback;   /* brackets the gate declined                  */
    ULONG   gc_gate_fast;       /* brackets that took a free baton directly    */
};
extern struct _tx_green_counters    _tx_green_counters;

/* Non-zero while the TX_THREAD being created is a gate proxy: green identity
   fields but NO initial frame (the capture switch writes its context).  A global
   because _tx_thread_create() zeroes the control block before stack_build runs. */
extern volatile UINT    _tx_amiga_gate_bind_pending;

/* The C half of the side-stack parker (tx_amiga_green.c); entered through
   _tx_gate_park_entry in tx_green_switch.S with the dispatcher-side
   Forbid() still held.  Never returns.  */
VOID    _tx_gate_park(TX_AMIGA_GATE *gate);

/* The asm shim the side frame returns into (tx_green_switch.S): moves the gate out
   of the a2 slot and calls _tx_gate_park().  Only its address is taken.  */
VOID    _tx_gate_park_entry(VOID);

#endif /* AMINETXDUO_GREEN_REALM */


/* Temporary lifecycle tracing.  Define TX_AMIGA_TRACE to route to ami_log().  */
#ifdef TX_AMIGA_TRACE
extern void ami_log(int level, const char *fmt, ...);
#define TXTRACE(...)    ami_log(0, __VA_ARGS__)
#else
#define TXTRACE(...)    ((void) 0)
#endif

#endif /* TX_AMIGA_INTERNAL_H */
