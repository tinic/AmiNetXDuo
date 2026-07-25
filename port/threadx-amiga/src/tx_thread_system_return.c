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
 * Park the calling Exec Task until it is the ThreadX baton holder.
 *
 * Never returns if the thread has been marked for teardown and it is a Task
 * that ThreadX created -- in that case the task destroys itself here.
 * Returns TX_FALSE if the thread was torn down but the Task is the
 * application's (an adopted thread), so the caller must unwind.
 */
UINT _tx_amiga_thread_park(TX_THREAD *thread_ptr)
{

ULONG        run_signal;
UINT         flags;
struct Task *reaper;
ULONG        reaper_signal;


    run_signal =  thread_ptr -> tx_thread_amiga_run_signal;

    for (;;)
    {

        Wait(run_signal);

        Forbid();

        flags =  thread_ptr -> tx_thread_amiga_flags;

        if ((flags & TX_AMIGA_THREAD_DIE) != 0U)
        {

            if ((flags & TX_AMIGA_THREAD_ADOPTED) != 0U)
            {

                /* The application owns this Task.  Tell the caller it is no
                   longer a ThreadX thread and let it unwind normally.  */
                thread_ptr -> tx_thread_amiga_flags |=  TX_AMIGA_THREAD_ORPHANED;
                Permit();
                return(TX_FALSE);
            }

            /* A Task we created.  Signal the reaper and remove ourselves.
               Both happen inside the Forbid(), and Exec discards the forbid
               nesting of a task it removes, so the reaper cannot run until we
               are genuinely gone.  */
            thread_ptr -> tx_thread_amiga_task =  (VOID *) 0;

            reaper        =  (struct Task *) thread_ptr -> tx_thread_amiga_reaper;
            reaper_signal =  thread_ptr -> tx_thread_amiga_reaper_signal;

            thread_ptr -> tx_thread_amiga_reaper        =  (VOID *) 0;
            thread_ptr -> tx_thread_amiga_reaper_signal =  0UL;

            if (reaper != (struct Task *) 0)
            {
                Signal(reaper, reaper_signal);
            }

            RemTask((struct Task *) 0);              /* never returns */
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
