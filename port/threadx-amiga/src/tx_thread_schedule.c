/***************************************************************************
 * Eclipse ThreadX -- AmigaOS/m68k port.
 *
 * Derived in structure from ports/linux/gnu/src/tx_thread_schedule.c
 *   Copyright (c) 2024 Microsoft Corporation
 *   Copyright (c) 2026-present Eclipse ThreadX contributors
 *
 * SPDX-License-Identifier: MIT
 **************************************************************************/

/**************************************************************************/
/*                                                                        */
/*    _tx_thread_schedule                              AmigaOS/m68k       */
/*                                                                        */
/*  DESCRIPTION                                                           */
/*                                                                        */
/*    The baton dispatcher.  Runs forever on the Exec Task that called     */
/*    tx_kernel_enter() and is the ONLY place a ThreadX thread is started. */
/*                                                                        */
/*    Invariant: a thread runs if and only if it is                        */
/*    _tx_thread_current_ptr.  The dispatcher therefore refuses to hand    */
/*    the baton out while _tx_thread_current_ptr is non-NULL, which is     */
/*    what makes the model safe against a third party (an adopting Task,   */
/*    the tick Task) waking it at an arbitrary moment.  The Linux port     */
/*    relies on the yielding thread having NULLed the pointer before it    */
/*    posts; making the check explicit costs one compare and removes a     */
/*    whole class of double-dispatch race.                                 */
/*                                                                        */
/**************************************************************************/

#define TX_SOURCE_CODE

#include "tx_amiga_internal.h"


VOID _tx_thread_schedule(VOID)
{

TX_THREAD   *thread_ptr;


    if (_tx_amiga_scheduler_signal == 0UL)
    {

        /* _tx_initialize_low_level() could not allocate a signal, so there is
           no way to hand the baton back.  Park rather than spin.  */
        Wait(0UL);
    }

    for (;;)
    {

        /* Wait for a thread to execute, with the baton free and no
           "interrupt" (tick) in progress.  */
        Forbid();
        while ((_tx_thread_execute_ptr == TX_NULL) ||
               (_tx_thread_current_ptr != TX_NULL) ||
               (_tx_thread_system_state != ((ULONG) 0)))
        {

            Permit();
            Wait(_tx_amiga_scheduler_signal);
            Forbid();
        }

        thread_ptr =  _tx_thread_execute_ptr;

        /* Hand over the baton.  */
        _tx_thread_current_ptr =  thread_ptr;
        thread_ptr -> tx_thread_run_count++;
        _tx_timer_time_slice =  thread_ptr -> tx_thread_time_slice;

        thread_ptr -> tx_thread_amiga_suspension_type =  ((UINT) 0);

        _tx_amiga_signal(thread_ptr -> tx_thread_amiga_task,
                         thread_ptr -> tx_thread_amiga_run_signal);

        Permit();

        /* Sleep until the thread (or the tick) says something changed.  */
        Wait(_tx_amiga_scheduler_signal);
    }
}


/* ----------------------------------------------------------- teardown --- */

/*
 * Remove the Exec Task backing a thread that ThreadX has finished with.
 *
 * The task is parked in Wait() inside _tx_thread_system_return().  Setting
 * TX_AMIGA_THREAD_DIE and poking its run signal makes it fall out of that Wait
 * and destroy itself.  It signals back first, under Forbid(), and only then
 * calls RemTask(NULL) -- Exec discards the forbid nesting of a task it is
 * removing, so the "signal the reaper then die" pair really is atomic and the
 * reaper cannot observe a half-removed task.
 *
 * Adopted threads are never reaped: their Exec Task belongs to the application.
 */
static VOID _tx_amiga_reap(TX_THREAD *thread_ptr)
{

struct Task     *task;
struct Task     *me;
BYTE             sig;
ULONG            sigmask;


    Forbid();
    task =  (struct Task *) thread_ptr -> tx_thread_amiga_task;
    if ((task == (struct Task *) 0) ||
        ((thread_ptr -> tx_thread_amiga_flags & TX_AMIGA_THREAD_ADOPTED) != 0U))
    {

        /* Nothing of ours to remove.  */
        thread_ptr -> tx_thread_amiga_task =  (VOID *) 0;
        Permit();
        return;
    }
    Permit();

    me  =  FindTask((STRPTR) 0);
    sig =  AllocSignal(-1);

    Forbid();

    /* Re-check: the task may have gone while we were unforbidden.  */
    task =  (struct Task *) thread_ptr -> tx_thread_amiga_task;
    if (task == (struct Task *) 0)
    {
        Permit();
        if (sig >= 0)
        {
            FreeSignal(sig);
        }
        return;
    }

    thread_ptr -> tx_thread_amiga_flags |=  TX_AMIGA_THREAD_DIE;

    if (sig >= 0)
    {

        sigmask =  1UL << ((ULONG) sig);
        thread_ptr -> tx_thread_amiga_reaper         =  (VOID *) me;
        thread_ptr -> tx_thread_amiga_reaper_signal  =  sigmask;
    }
    else
    {

        /* No spare signal: fire and forget.  The task still tears itself down,
           but we cannot prove it finished before the caller frees the stack.  */
        sigmask =  0UL;
        thread_ptr -> tx_thread_amiga_reaper         =  (VOID *) 0;
        thread_ptr -> tx_thread_amiga_reaper_signal  =  0UL;
    }

    Signal(task, thread_ptr -> tx_thread_amiga_run_signal);
    Permit();

    if (sigmask != 0UL)
    {
        Wait(sigmask);
        FreeSignal(sig);
    }
}


void _tx_thread_delete_port_completion(TX_THREAD *thread_ptr, UINT tx_saved_posture)
{

    TX_RESTORE
    _tx_amiga_reap(thread_ptr);
    TX_DISABLE
}


void _tx_thread_reset_port_completion(TX_THREAD *thread_ptr, UINT tx_saved_posture)
{

    TX_RESTORE
    _tx_amiga_reap(thread_ptr);
    TX_DISABLE
}
