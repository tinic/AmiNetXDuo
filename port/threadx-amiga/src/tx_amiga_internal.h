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
 * Create an Exec Task on a caller-supplied stack.  The struct Task and its
 * MemList live in one AllocMem() block that is registered in tc_MemEntry, so
 * Exec frees it when the task finally calls RemTask(NULL).  The stack itself
 * is NOT owned by the task -- for ThreadX threads it belongs to whoever called
 * tx_thread_create().
 *
 * Returns the struct Task * or NULL.
 */
struct Task *_tx_amiga_task_create(CHAR *name, BYTE priority, VOID (*entry)(VOID),
                                   APTR stack, ULONG stack_size, APTR user_data);

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


/* Port globals defined in tx_initialize_low_level.c.  */
extern volatile UINT    _tx_amiga_kernel_up;
extern volatile UINT    _tx_amiga_timer_stop;

#endif /* TX_AMIGA_INTERNAL_H */
