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
