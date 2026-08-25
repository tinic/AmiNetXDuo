/***************************************************************************
 * Eclipse ThreadX, AmigaOS/m68k port.
 *
 * Derived in structure from ports/linux/gnu/src/tx_thread_system_return.c
 *   Copyright (c) 2024 Microsoft Corporation
 *   Copyright (c) 2026-present Eclipse ThreadX contributors
 *
 * SPDX-License-Identifier: MIT
 **************************************************************************/

/* _tx_thread_system_return, AmigaOS/m68k: returns the baton and parks the calling
   Exec Task on its run signal.  Exec saves TDNestCnt when a task blocks and puts it
   back on redispatch, which is what makes Forbid() a legal TX_DISABLE here.  */

#define TX_SOURCE_CODE

#include "tx_amiga_internal.h"


/* Destroy the calling Exec Task, which the port created.  Touches nothing but the
   task's own control block, so it is correct even when the reaper gave up on this
   task and its TX_THREAD has since been deleted or reused.  Never returns.  */
VOID _tx_amiga_task_destroy(struct _tx_amiga_ctrl *ctrl)
{

struct Task     *reaper;
ULONG            reaper_signal;
volatile ULONG  *reaped;
TX_THREAD       *owner;


    Forbid();

    owner =  ctrl -> ctrl_thread;
    if (owner != TX_NULL)
    {
        owner -> tx_thread_amiga_task =  (VOID *) 0;
    }

    reaper        =  ctrl -> ctrl_reaper;
    reaper_signal =  ctrl -> ctrl_reaper_signal;
    reaped        =  ctrl -> ctrl_reaped;

    ctrl -> ctrl_thread        =  TX_NULL;
    ctrl -> ctrl_reaper        =  (struct Task *) 0;
    ctrl -> ctrl_reaper_signal =  0UL;
    ctrl -> ctrl_reaped        =  (volatile ULONG *) 0;
    ctrl -> ctrl_magic         =  0UL;

    /* A zombie stops being one here: the moment it can no longer touch the
       application's code or data.  tx_amiga_kernel_stop() waits for zero.  */
    if (ctrl -> ctrl_zombie != 0U)
    {
        ctrl -> ctrl_zombie =  0U;
        if (_tx_amiga_zombies_live != 0UL)
        {
            _tx_amiga_zombies_live--;
        }
    }

    /* Set the flag before the Signal, so a reaper waking on its timeout in the same
       instant still sees a completed handshake.  Both happen inside the Forbid()
       whose nesting RemTask() discards, so the pair is atomic.  */
    if (reaped != (volatile ULONG *) 0)
    {
        *reaped =  1UL;
    }
    if (reaper != (struct Task *) 0)
    {
        Signal(reaper, reaper_signal);
    }

    TXTRACE("TXT destroy task=%08lx", (LONG) FindTask((STRPTR) 0));

    RemTask((struct Task *) 0);                      /* never returns */

    /* Unreachable; keeps a "noreturn" analysis honest if RemTask ever does.  */
    for (;;)
    {
        Wait(0UL);
    }
}


/* TX_THREAD_COMPLETED_EXTENSION, before _tx_thread_system_suspend() unlinks the
   thread: for an abandoned thread that unlinking splices a dead node's stale
   neighbours into the live list.  It also undoes shell_entry's preempt_disable++. */
VOID _tx_amiga_thread_completed(VOID)
{

struct _tx_amiga_ctrl   *ctrl;


    ctrl =  _tx_amiga_ctrl_of(FindTask((STRPTR) 0));

    if ((ctrl != (struct _tx_amiga_ctrl *) 0) && (ctrl -> ctrl_die != 0U))
    {

        Forbid();
        _tx_thread_preempt_disable--;
        Permit();

        TXTRACE("TXT completed-as-zombie task=%08lx", (LONG) FindTask((STRPTR) 0));

        _tx_amiga_task_destroy(ctrl);                /* never returns */
    }
}


/* Park the calling Exec Task until it is the ThreadX baton holder.  Never returns
   if a Task ThreadX created has been marked for teardown -- it destroys itself
   here.  TX_FALSE means an adopted Task was torn down and the caller must unwind. */
UINT _tx_amiga_thread_park(TX_THREAD *thread_ptr)
{

ULONG                    run_signal;
UINT                     adopted;
struct _tx_amiga_ctrl   *ctrl;
struct Task             *me;
BYTE                     saved_pri;
UINT                     raised;


    me         =  FindTask((STRPTR) 0);
    run_signal =  thread_ptr -> tx_thread_amiga_run_signal;
    adopted    =  thread_ptr -> tx_thread_amiga_flags & TX_AMIGA_THREAD_ADOPTED;

    /* Cache the control block once, while the TX_THREAD is still ours: a task the
       reaper gave up on can then destroy itself long after the TX_THREAD has been
       deleted and reused.  An adopted Task has no control block.  */
    ctrl =  (adopted != 0U) ? ((struct _tx_amiga_ctrl *) 0)
                            : _tx_amiga_ctrl_of(me);

    /* Wait for the baton one Exec priority up: Exec reschedules on a Signal() only
       for a STRICTLY higher priority.  RAISE ONLY, never lower -- SetTaskPri() is
       absolute and an adopted Task's priority is the application's.  */
    Forbid();
    raised =  (_tx_thread_current_ptr != thread_ptr) ? ((UINT) TX_TRUE)
                                                     : ((UINT) TX_FALSE);
    Permit();

    saved_pri =  me -> tc_Node.ln_Pri;
    if (saved_pri >= (BYTE) TX_AMIGA_HANDOFF_PRIORITY)
    {
        raised =  (UINT) TX_FALSE;
    }
    if (raised != ((UINT) TX_FALSE))
    {
        (VOID) SetTaskPri(me, (LONG) TX_AMIGA_HANDOFF_PRIORITY);
    }

    for (;;)
    {

        TX_AMIGA_COUNT(TX_AMIGA_SC_PARK_WAIT);

        Wait(run_signal);

        Forbid();

        if (ctrl != (struct _tx_amiga_ctrl *) 0)
        {

            if (ctrl -> ctrl_die != 0U)
            {
                Permit();
                _tx_amiga_task_destroy(ctrl);        /* never returns */
            }
        }
        else if ((thread_ptr -> tx_thread_amiga_flags & TX_AMIGA_THREAD_DIE) != 0U)
        {

            /* The application owns this Task.  Tell the caller it is no longer
               a ThreadX thread and let it unwind normally.  */
            thread_ptr -> tx_thread_amiga_flags |=  TX_AMIGA_THREAD_ORPHANED;
            Permit();
            if (raised != ((UINT) TX_FALSE))
            {
                (VOID) SetTaskPri(me, (LONG) saved_pri);
            }
            return(TX_FALSE);
        }
        else
        {
            /* Nothing to do.  */
        }

        if (_tx_thread_current_ptr == thread_ptr)
        {

            /* We hold the baton.  */
            Permit();
            if (raised != ((UINT) TX_FALSE))
            {
                (VOID) SetTaskPri(me, (LONG) saved_pri);
            }
            return(TX_TRUE);
        }

        /* Spurious wake-up, the scheduler changed its mind, or a stale
           signal latched.  Go back to sleep.  */
        TX_AMIGA_COUNT(TX_AMIGA_SC_PARK_SPURIOUS);
        Permit();
    }
}


VOID _tx_thread_system_return(VOID)
{

TX_THREAD   *thread_ptr;
struct Task *me;
UINT         wake;


    me =  FindTask((STRPTR) 0);

    Forbid();

    TX_AMIGA_COUNT(TX_AMIGA_SC_SYS_RETURN);

    thread_ptr =  _tx_thread_current_ptr;

    if ((thread_ptr == TX_NULL) ||
        (thread_ptr -> tx_thread_amiga_task != (VOID *) me))
    {

        /* Not the baton holder, so there is nothing to release.  Should be
           unreachable; returning quietly beats corrupting the ready list.  */
        Permit();
        return;
    }

    /* Preserve the remaining time slice.  */
    if (_tx_timer_time_slice != ((ULONG) 0))
    {

        thread_ptr -> tx_thread_time_slice =  _tx_timer_time_slice;
        _tx_timer_time_slice =  ((ULONG) 0);
    }

    thread_ptr -> tx_thread_amiga_suspension_type =  ((UINT) 0);

    /* Tells a thread that never comes back apart from a task the port failed to
       wake: TX_TCP_IP is NetX Duo suspending its own caller, and the cleanup
       routine names the service it is parked in.  */
    if (thread_ptr -> tx_thread_state == ((UINT) TX_TCP_IP))
    {
        TXTRACE("TXT nxsusp thr=%08lx sock=%08lx cleanup=%08lx timeout=%08lx here=%08lx",
                (LONG) thread_ptr,
                (LONG) thread_ptr -> tx_thread_suspend_control_block,
                (LONG) thread_ptr -> tx_thread_suspend_cleanup,
                (LONG) thread_ptr -> tx_thread_timer.tx_timer_internal_remaining_ticks,
                (LONG) &_tx_thread_system_return);
    }

#ifdef AMINETXDUO_GREEN_REALM
    if (_tx_amiga_thread_green(thread_ptr) != TX_FALSE)
    {
        /* A green thread yields by switching straight back into the realm
           scheduler's context.  The protocol Forbid() is the one taken above; the
           scheduler loop resumes holding it, and the next dispatch returns here. */
        ami_budget_hold_end((APTR) thread_ptr, thread_ptr -> tx_thread_name,
                            (ULONG) thread_ptr -> tx_thread_state,
                            AMI_HOLD_SITE_YIELD);
        _tx_thread_current_ptr =  TX_NULL;

        _tx_green_switch(&thread_ptr -> tx_thread_stack_ptr,
                         _tx_green_scheduler_sp);

        Permit();
        return;
    }
#endif

    /* Release the baton and give it straight to whoever is next rather than waking
       the scheduler Task to do it.  */
    ami_budget_hold_end((APTR) thread_ptr, thread_ptr -> tx_thread_name,
                        (ULONG) thread_ptr -> tx_thread_state,
                        AMI_HOLD_SITE_YIELD);
    _tx_thread_current_ptr =  TX_NULL;

    wake =  _tx_amiga_wake_needed(_tx_amiga_dispatch_inline());

    Permit();

    if (wake != ((UINT) TX_FALSE))
    {
        _tx_amiga_wake_scheduler();
    }

    /* Park.  A completed or terminated thread stays here until
       tx_thread_delete() reaps it, which keeps teardown on one path.  */
    (VOID) _tx_amiga_thread_park(thread_ptr);
}
