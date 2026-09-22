/***************************************************************************
 * Eclipse ThreadX, AmigaOS/m68k port.
 *
 * Derived in structure from ports/linux/gnu/src/tx_thread_interrupt_control.c
 *   Copyright (c) 2024 Microsoft Corporation
 *   Copyright (c) 2026-present Eclipse ThreadX contributors
 *
 * SPDX-License-Identifier: MIT
 **************************************************************************/

/* _tx_thread_interrupt_control, AmigaOS/m68k: ThreadX's interrupt lockout is
   Forbid()/Permit(), not Disable()/Enable() -- nothing here runs at Exec interrupt
   level, and a long Disable() drops serial characters and floppy transfers.  */

#define TX_SOURCE_CODE

#include "tx_amiga_internal.h"

#include <exec/ports.h>


/* TX_DISABLE / TX_RESTORE.  These are always strictly paired in the ThreadX
   core, so the saved posture is informational and the nesting is carried by
   Exec's TDNestCnt.  */

/* The nest counter is touched directly rather than through the library vectors;
   the bodies are inline in tx_port.h.  The offsets that header reaches Exec by are
   asserted below, so a moved field fails the build instead of corrupting it.  */

_Static_assert(__builtin_offsetof(struct ExecBase, TDNestCnt) == TX_AMIGA_OFF_TDNESTCNT,
               "ExecBase.TDNestCnt moved, fix TX_AMIGA_OFF_TDNESTCNT");
_Static_assert(__builtin_offsetof(struct ExecBase, AttnResched) == TX_AMIGA_OFF_ATTNRESCHED,
               "ExecBase.AttnResched moved, fix TX_AMIGA_OFF_ATTNRESCHED");
_Static_assert(sizeof(SysBase -> TDNestCnt) == 1, "TDNestCnt is not a byte");
_Static_assert(sizeof(SysBase -> AttnResched) == 2, "AttnResched is not a word");

/* exec/tasks.i: V36+ keeps tc_ETask where V33 kept tc_TrapAlloc/tc_TrapAble,
   valid when TF_ETASK is set, and et_UniqueID follows the ETask's struct Message
   and et_Parent.  The C header spells the old pair and has no struct ETask, so
   both are reached by offset.  Commodore's own exec never sets TF_ETASK -- 37.132
   and 40.10 were both probed, every task had tc_Flags 0 and 0x80000000 at offset
   34 -- so on those ROMs this contributes nothing and the stamp below rests on the
   fields that do exist.  */
#define TX_AMIGA_OFF_ETASK          34
#define TX_AMIGA_OFF_ET_UNIQUEID    (sizeof(struct Message) + sizeof(APTR))

_Static_assert(__builtin_offsetof(struct Task, tc_TrapAlloc) == TX_AMIGA_OFF_ETASK,
               "Task.tc_TrapAlloc moved, fix TX_AMIGA_OFF_ETASK");
_Static_assert(sizeof(struct Message) == 20, "Message is not 20 bytes");
_Static_assert(TX_AMIGA_OFF_ET_UNIQUEID == 24, "et_UniqueID is not at ETask+24");


/* The tail of Permit(), reached from TX_RESTORE only when AttnResched is set.  The
   decrement has already happened; when all three of Exec's conditions hold the
   nesting is put back and the real Permit() runs, so the reschedule is Exec's.  */
VOID _tx_amiga_permit_finish(void)
{

    if ((SysBase -> TDNestCnt >= 0) || (SysBase -> IDNestCnt >= 0) ||
        (SysBase -> AttnResched == 0))
    {
        return;
    }

    __asm volatile ("addq.b #1,%0" : "+m" (SysBase -> TDNestCnt) : : "cc");
    TX_AMIGA_COUNT(TX_AMIGA_SC_PERMIT_SLOW);
    Permit();
}


/* The out-of-line spellings.  Nothing on the data path reaches them -- TX_DISABLE
   and TX_RESTORE are the inline pair -- but they are part of the port's surface. */

UINT _tx_thread_interrupt_disable(void)
{

    return(_tx_amiga_int_disable());
}


VOID _tx_thread_interrupt_restore(UINT previous_posture)
{

    _tx_amiga_int_restore(previous_posture);
}


/* Keep Exec's private scheduling fields inside the port which implements
   their semantics.  In particular, a ThreadX timer callback is an Exec Task
   but still cannot call an application hook: the timer task runs its wheel
   under Forbid(). */
UINT tx_amiga_exec_task_context(VOID)
{
struct Task *task;


    if (SysBase == (struct ExecBase *) 0)
    {
        return(TX_FALSE);
    }

    task =  FindTask((STRPTR) 0);
    if ((SysBase -> TDNestCnt >= 0) || (SysBase -> IDNestCnt >= 0) ||
        ((VOID *) task == _tx_amiga_timer_task))
    {
        return(TX_FALSE);
    }

    return(TX_TRUE);
}


/* Is task on one of Exec's scheduler lists?  Caller holds Disable(), because
   an interrupt can move a task between TaskWait and TaskReady even while
   scheduling is forbidden. */
static UINT tx_amiga_task_on_list(struct List *list, struct Task *task)
{
struct Node *node;

    for (node = list -> lh_Head; node -> ln_Succ != (struct Node *) 0;
         node = node -> ln_Succ)
    {
        if ((struct Task *) node == task)
        {
            return(TX_TRUE);
        }
    }

    return(TX_FALSE);
}


UINT tx_amiga_task_alive_locked(struct Task *task)
{
    if ((task != (struct Task *) 0) &&
        ((FindTask((STRPTR) 0) == task) ||
         tx_amiga_task_on_list(&SysBase -> TaskReady, task) ||
         tx_amiga_task_on_list(&SysBase -> TaskWait, task)))
    {
        return(TX_TRUE);
    }

    return(TX_FALSE);
}


/* An identity for a Task that a recycled address does not inherit.  Exec has no
   task ID on the shipped ROMs (see above), so this is the stack bounds and the
   name pointer, which a live task cannot change from inside a library vector, plus
   et_UniqueID where an exec provides one.  Never 0, so 0 in a TX_THREAD means
   "not stamped".  The bounds move on StackSwap() between calls, which is why the
   port re-stamps at every entry from the task's own context.  */
ULONG _tx_amiga_task_stamp(struct Task *task)
{
UBYTE   *etask;
ULONG    stamp;

    if (task == (struct Task *) 0)
    {
        return(0UL);
    }

    stamp =  ((ULONG) task -> tc_SPLower) ^
             (((ULONG) task -> tc_SPUpper) << 1) ^
             (((ULONG) task -> tc_Node.ln_Name) << 2);

    if ((task -> tc_Flags & TF_ETASK) != 0)
    {
        etask =  *(UBYTE **) (((UBYTE *) task) + TX_AMIGA_OFF_ETASK);
        if (etask != (UBYTE *) 0)
        {
            stamp ^=  *(ULONG *) (etask + TX_AMIGA_OFF_ET_UNIQUEID);
        }
    }

    return((stamp != 0UL) ? stamp : 1UL);
}


/* Is the Exec Task behind an adopted thread gone?  Caller holds Forbid().
   Only a plain adopted thread qualifies: a thread the port created, or one
   already dying or orphaned, has its own teardown.

   EVERY TEST HERE IS A POSITIVE TEST FOR DEATH, and the first of them is the
   only question that may be asked about a Task that may already have been
   freed, so it is asked first and nothing inside the struct is read until it
   has answered "still there":

     (a) the Task is on neither of Exec's scheduler lists and is not us.  A Task
         Exec has removed is on neither; a Task that exists is always on one.
     (b) the Task no longer has the thread's run-signal bit allocated.  An
         adopted Task allocates that bit itself before it adopts and frees it
         only at orphan or discard, so a Task at this address without it is not
         the Task that adopted.

   THE SHAPE STAMP IS A NEGATIVE TEST ONLY.  A mismatch says the Task at this
   address no longer looks like the one that adopted, which is a reason to trust
   nothing else read out of it -- so a mismatch DECLINES the reclaim.  It never
   proves death: a live holder that StackSwap()ed inside a call has a new shape
   and is not dead.

   What that costs is MISSED reclaims, never wrong ones:

     - a removed Task whose address has been recycled by a Task that allocated
       the same run bit and happens to carry the same stack bounds and name
       pointer reads as alive.  That is the behaviour every Task had before this
       existed, and it is not permanent: when the recycled Task exits, (a) fires.
     - a removed Task whose address has been recycled by a Task with a different
       shape is declined by the stamp for as long as that Task lives, and then
       reclaimed by (a).

   A wrong answer can therefore only ever be "alive".  */
UINT tx_amiga_adopted_task_dead(TX_THREAD *thread_ptr)
{
struct Task *task;
ULONG        sig;
ULONG        stamp;
UINT         dead;

    if ((thread_ptr == TX_NULL) ||
        (thread_ptr -> tx_thread_id != TX_THREAD_ID) ||
        ((thread_ptr -> tx_thread_amiga_flags &
          (TX_AMIGA_THREAD_ADOPTED | TX_AMIGA_THREAD_GREEN |
           TX_AMIGA_THREAD_DIE | TX_AMIGA_THREAD_ORPHANED)) != TX_AMIGA_THREAD_ADOPTED) ||
        (thread_ptr -> tx_thread_amiga_task == (VOID *) 0))
    {
        return(TX_FALSE);
    }

    task  =  (struct Task *) thread_ptr -> tx_thread_amiga_task;
    sig   =  thread_ptr -> tx_thread_amiga_run_signal;
    stamp =  thread_ptr -> tx_thread_amiga_task_stamp;

    Disable();

    if (tx_amiga_task_alive_locked(task) == TX_FALSE)
    {
        /* (a).  NOTHING was read out of the struct to get here.  */
        dead =  TX_TRUE;
    }
    else if ((stamp != 0UL) && (_tx_amiga_task_stamp(task) != stamp))
    {
        /* The address is occupied by something that does not look like our
           holder.  Decline; see above.  */
        dead =  TX_FALSE;
    }
    else if ((sig != 0UL) && ((task -> tc_SigAlloc & sig) == 0UL))
    {
        /* (b).  */
        dead =  TX_TRUE;
    }
    else
    {
        dead =  TX_FALSE;
    }

    Enable();

    return(dead);
}


UINT tx_amiga_exec_task_alive(VOID *task)
{
UINT alive;

    if (SysBase == (struct ExecBase *) 0)
    {
        return(TX_FALSE);
    }

    Disable();
    alive =  tx_amiga_task_alive_locked((struct Task *) task);
    Enable();

    return(alive);
}


UINT tx_amiga_exec_task_signal(VOID *task, ULONG sigmask)
{
UINT alive;

    if (SysBase == (struct ExecBase *) 0)
    {
        return(TX_FALSE);
    }

    Disable();
    alive =  tx_amiga_task_alive_locked((struct Task *) task);
    if ((alive != TX_FALSE) && (sigmask != 0UL))
    {
        Signal((struct Task *) task, sigmask);
    }
    Enable();

    return(alive);
}


/* tx_interrupt_control(), the application-visible service.  It may be called
   unbalanced, so it changes the nesting by at most one level and reports the old
   posture: TX_INT_ENABLE inside N nested Forbid()s drops one level, not all N.  */
UINT _tx_thread_interrupt_control(UINT new_posture)
{

UINT    old_posture;


    old_posture =  _tx_amiga_forbidden();

    if ((new_posture == ((UINT) TX_INT_DISABLE)) &&
        (old_posture == ((UINT) TX_INT_ENABLE)))
    {
        Forbid();
    }
    else if ((new_posture == ((UINT) TX_INT_ENABLE)) &&
             (old_posture == ((UINT) TX_INT_DISABLE)))
    {
        Permit();
    }
    else
    {
        /* Already enabled, nothing to do.  */
    }

    return(old_posture);
}
