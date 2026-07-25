/***************************************************************************
 * Eclipse ThreadX -- AmigaOS/m68k port.
 *
 * Derived in structure from ports/linux/gnu/src/tx_thread_system_return.c
 *   Copyright (c) 2024 Microsoft Corporation
 *   Copyright (c) 2026-present Eclipse ThreadX contributors
 *
 * SPDX-License-Identifier: MIT
 **************************************************************************/

/**************************************************************************/
/*                                                                        */
/*    _tx_thread_system_return                         AmigaOS/m68k       */
/*                                                                        */
/*  DESCRIPTION                                                           */
/*                                                                        */
/*    Hands the baton back to the scheduler and parks the calling Exec     */
/*    Task on its run signal until the scheduler picks it again.           */
/*                                                                        */
/*    The core always calls this with the critical section already         */
/*    restored (every call site is preceded by TX_RESTORE), so the Wait()  */
/*    below happens at Forbid() nesting zero.  Even if it did not: Exec    */
/*    saves SysBase->TDNestCnt into tc_TDNestCnt when a task blocks and    */
/*    restores it on redispatch, so a Forbid() held across Wait() neither  */
/*    stops the machine nor is lost.  That property is what makes          */
/*    Forbid()/Permit() a legal TX_DISABLE/TX_RESTORE for a port whose     */
/*    threads block inside critical sections.                              */
/*                                                                        */
/**************************************************************************/

#define TX_SOURCE_CODE

#include "tx_amiga_internal.h"


/*
 * Destroy the calling Exec Task, which the port created.
 *
 * Touches nothing but the task's own control block, so it is correct even when
 * the reaper gave up on this task long ago and the TX_THREAD it used to back
 * has since been deleted, reused or freed.  Never returns.
 */
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

    /* A zombie stops being one here: this is the moment it can no longer touch
       the application's code or data.  tx_amiga_kernel_stop() waits for this
       count to reach zero before it will call an exit safe.  */
    if (ctrl -> ctrl_zombie != 0U)
    {
        ctrl -> ctrl_zombie =  0U;
        if (_tx_amiga_zombies_live != 0UL)
        {
            _tx_amiga_zombies_live--;
        }
    }

    /* The flag lives on the reaper's stack and it is blocked in Wait(), so it
       is alive.  Set it BEFORE the Signal, so a reaper that wakes on its
       timeout in the same instant still sees a completed handshake rather than
       declaring a zombie.  Signalling and dying both happen inside the
       Forbid(), and Exec discards the forbid nesting of a task it removes, so
       the pair is atomic: the reaper cannot observe a half-removed task.  */
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


/*
 * TX_THREAD_COMPLETED_EXTENSION.
 *
 * _tx_thread_shell_entry() calls this the moment a thread's entry function
 * returns, and crucially BEFORE _tx_thread_system_suspend() unlinks the thread
 * from the ready lists.  For a thread the reaper had to abandon (see
 * _tx_amiga_reap()) that unlinking would be done with a TX_THREAD that has
 * already been deleted -- tx_thread_ready_next/previous still point at live
 * threads, so it splices a dead node's stale neighbours into the live list and
 * the next dispatch jumps through whatever that leaves behind.  Dying here
 * instead is the difference between a tidy exit and a wild jump.
 *
 * shell_entry has already done _tx_thread_preempt_disable++ on the assumption
 * that _tx_thread_system_suspend() will undo it, so undo it here instead.
 */
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


/*
 * Park the calling Exec Task until it is the ThreadX baton holder.
 *
 * Never returns if the thread has been marked for teardown and it is a Task
 * that ThreadX created -- in that case the task destroys itself here.
 * Returns TX_FALSE if the thread was torn down but the Task is the
 * application's (an adopted thread), so the caller must unwind.
 */
UINT _tx_amiga_thread_park(TX_THREAD *thread_ptr)
{

ULONG                    run_signal;
UINT                     adopted;
struct _tx_amiga_ctrl   *ctrl;


    run_signal =  thread_ptr -> tx_thread_amiga_run_signal;
    adopted    =  thread_ptr -> tx_thread_amiga_flags & TX_AMIGA_THREAD_ADOPTED;

    /* Cache the control block ONCE, while the TX_THREAD is certainly still
       ours.  Everything the teardown below touches lives in it, so a task that
       the reaper gave up on can still destroy itself safely long after the
       TX_THREAD has been deleted and reused.  An adopted Task has no control
       block -- its teardown is a return, not a RemTask().  */
    ctrl =  (adopted != 0U) ? ((struct _tx_amiga_ctrl *) 0)
                            : _tx_amiga_ctrl_of(FindTask((STRPTR) 0));

    for (;;)
    {

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
            return(TX_TRUE);
        }

        /* Spurious wake-up -- the scheduler changed its mind, or a stale
           signal latched.  Go back to sleep.  */
        Permit();
    }
}


VOID _tx_thread_system_return(VOID)
{

TX_THREAD   *thread_ptr;
struct Task *me;


    me =  FindTask((STRPTR) 0);

    Forbid();

    thread_ptr =  _tx_thread_current_ptr;

    if ((thread_ptr == TX_NULL) ||
        (thread_ptr -> tx_thread_amiga_task != (VOID *) me))
    {

        /* We are not the baton holder, so there is nothing to give back.
           The port keeps _tx_thread_system_state non-zero over every window
           in which a non-ThreadX Exec Task touches ThreadX state
           (tx_amiga_adopt.c, the tick task), so this should be unreachable;
           returning quietly is still better than corrupting the ready list.  */
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

    /* Release the baton.  */
    _tx_thread_current_ptr =  TX_NULL;

    Permit();

    _tx_amiga_wake_scheduler();

    /* Park.  A completed or terminated thread simply stays here until
       tx_thread_delete() reaps it; that keeps teardown on one path.  */
    (VOID) _tx_amiga_thread_park(thread_ptr);
}
