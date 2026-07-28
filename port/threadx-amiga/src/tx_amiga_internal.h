/*
 * AmiNetXDuo -- private glue shared between the ThreadX Exec port sources.
 * Not installed; not visible to the ThreadX or NetX Duo cores.
 *
 * Include order matters: "tx_api.h" (and therefore tx_port.h) must come first
 * so that ThreadX's VOID/ULONG typedefs beat <exec/types.h>'s macros.
 *
 * SPDX-License-Identifier: MIT
 */

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


/* Open-coded NewList(): amiga.lib is not linkable into a shared library.
   Same pattern as src/common/compat.c.  */
static __inline VOID _tx_amiga_newlist(struct List *list)
{
    list -> lh_Head     = (struct Node *) &list -> lh_Tail;
    list -> lh_Tail     = (struct Node *) 0;
    list -> lh_TailPred = (struct Node *) &list -> lh_Head;
}


/*
 * Per-Task control block for every Exec Task the port creates.
 *
 * The teardown state lives here and not in the TX_THREAD, because a dying task
 * must be able to destroy itself using only memory it owns.  If the reaper gave
 * up on it (see _tx_amiga_reap()) the TX_THREAD may already have been deleted,
 * reused or freed by the application by the time the task finally unblocks;
 * reading tx_thread_amiga_flags at that point would be a use-after-free.  This
 * block is registered in tc_MemEntry, so it stays valid for as long as the Task
 * does.
 *
 * ctrl_task is at offset 0, so the struct Task * the rest of the port passes
 * around is the control block.
 *
 * Identifying one from a bare struct Task * has to be safe on a foreign task
 * too -- _tx_amiga_thread_completed() runs before the port knows whose task it
 * is on.  The test therefore never reads past the struct Task: tc_UserData
 * points back at the Task itself, which no other task does, and only once that
 * holds is ctrl_magic (our own memory by then) consulted.
 */

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


/*
 * Create an Exec Task on a caller-supplied stack.
 *
 * Two allocations, because RemTask() hands each MemList in tc_MemEntry to
 * FreeEntry(), which frees both the entries the list describes and the MemList
 * structure itself.  A MemList that lives inside the block it describes is
 * therefore freed twice -- Guru 01000009, AN_FreeTwice -- on every task that
 * exits.  The Task (inside its control block) and the MemList are separate
 * blocks for that reason.
 *
 * The stack is not owned by the task: for ThreadX threads it belongs to whoever
 * called tx_thread_create().
 *
 * Returns the struct Task * or NULL.
 */
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


/*
 * Park the calling Exec Task until it holds the ThreadX baton (defined in
 * tx_thread_system_return.c).
 *
 * Returns TX_TRUE when the thread has been dispatched.  Returns TX_FALSE only
 * for an adopted thread that was torn down under it -- the Task must then
 * unwind out of ThreadX.  For a Task that ThreadX created, teardown is handled
 * in place and the function does not return at all.
 */
UINT _tx_amiga_thread_park(TX_THREAD *thread_ptr);


/*
 * Destroy the calling Exec Task (one the port created).  Never returns.
 * Touches only the task's own control block, so it is safe even for a task the
 * reaper had to abandon.
 */
VOID _tx_amiga_task_destroy(struct _tx_amiga_ctrl *ctrl);


/*
 * TX_THREAD_COMPLETED_EXTENSION.  Runs on the thread's own Exec Task the
 * instant its entry function returns, before _tx_thread_system_suspend() does
 * any ready-list surgery.  That is the only place a detached zombie can be
 * caught before it corrupts ThreadX with a TX_THREAD that no longer exists.
 */
VOID _tx_amiga_thread_completed(VOID);


/* Port globals defined in tx_initialize_low_level.c.  */
extern volatile UINT    _tx_amiga_kernel_up;
extern volatile UINT    _tx_amiga_timer_stop;
extern volatile ULONG   _tx_amiga_zombies;

/*
 * Set by tx_amiga_kernel_stop() once its preconditions are met, and cleared
 * only when the kernel is fully down again.
 *
 *   - _tx_thread_schedule() returns instead of dispatching, which is the only
 *     way the master Task ever leaves tx_kernel_enter();
 *   - tx_amiga_adopt_thread() refuses, so nothing new joins a kernel that is
 *     on its way out.
 */
extern volatile UINT    _tx_amiga_kernel_stopping;

/*
 * Zombies that have not yet unblocked and destroyed themselves.
 *
 * _tx_amiga_zombies counts every zombie ever declared and never goes down, so
 * it only tells the caller of tx_thread_delete() that this happened.  This one
 * goes back down when the zombie finally reaches _tx_amiga_task_destroy(), and
 * is what tx_amiga_kernel_stop() consults: a program may exit safely once its
 * zombies are gone, but not while one is still parked in Exec with its entry
 * point in a code hunk that is about to be unloaded.
 */
extern volatile ULONG   _tx_amiga_zombies_live;


/* Temporary lifecycle tracing.  Define TX_AMIGA_TRACE to route to ami_log().  */
#ifdef TX_AMIGA_TRACE
extern void ami_log(int level, const char *fmt, ...);
#define TXTRACE(...)    ami_log(0, __VA_ARGS__)
#else
#define TXTRACE(...)    ((void) 0)
#endif

#endif /* TX_AMIGA_INTERNAL_H */
