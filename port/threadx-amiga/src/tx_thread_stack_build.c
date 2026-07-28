/***************************************************************************
 * Eclipse ThreadX -- AmigaOS/m68k port.
 *
 * Derived in structure from ports/linux/gnu/src/tx_thread_stack_build.c
 *   Copyright (c) 2024 Microsoft Corporation
 *   Copyright (c) 2026-present Eclipse ThreadX contributors
 *
 * SPDX-License-Identifier: MIT
 **************************************************************************/

/**************************************************************************/
/*                                                                        */
/*    _tx_thread_stack_build                           AmigaOS/m68k       */
/*                                                                        */
/*  DESCRIPTION                                                           */
/*                                                                        */
/*    There is no stack frame to build in a hosted port.  Instead this     */
/*    creates the Exec Task that will execute the thread, using the stack  */
/*    tx_thread_create() was given, so a ThreadX thread costs one struct   */
/*    Task plus one MemList over and above its ThreadX stack.              */
/*                                                                        */
/*    For an adopted thread there is nothing to create: the Exec Task      */
/*    already exists and already owns a stack.  tx_amiga_adopt_thread()    */
/*    leaves the task and its run signal in _tx_amiga_adopt_task /         */
/*    _tx_amiga_adopt_signal and we bind to those.  The handshake is a     */
/*    pair of globals rather than an argument because _tx_thread_create()  */
/*    zeroes the whole control block, extension fields included, before it */
/*    calls us -- anything staged in the TX_THREAD would be wiped.         */
/*                                                                        */
/**************************************************************************/

#define TX_SOURCE_CODE

#include "tx_amiga_internal.h"


static VOID _tx_amiga_thread_entry(VOID);


VOID _tx_thread_stack_build(TX_THREAD *thread_ptr, VOID (*function_ptr)(VOID))
{

struct Task *task;
CHAR        *name;


    (VOID) function_ptr;

    thread_ptr -> tx_thread_amiga_suspension_type =  ((UINT) 0);
    thread_ptr -> tx_thread_amiga_flags           =  ((UINT) 0);
    thread_ptr -> tx_thread_amiga_task            =  (VOID *) 0;
    thread_ptr -> tx_thread_amiga_run_signal      =  0UL;

    /* Give the generic code a plausible stack pointer.  Nothing in this port
       dereferences it (stack checking is unavailable, see tx_port.h).  */
    thread_ptr -> tx_thread_stack_ptr =
        (VOID *) (((CHAR *) thread_ptr -> tx_thread_stack_end) - 8);

    name =  thread_ptr -> tx_thread_name;
    if (name == (CHAR *) 0)
    {
        name =  "ThreadX thread";
    }

    /* ---- adoption ------------------------------------------------------- */

    if (_tx_amiga_adopt_task != (VOID *) 0)
    {

        thread_ptr -> tx_thread_amiga_task       =  _tx_amiga_adopt_task;
        thread_ptr -> tx_thread_amiga_run_signal =  _tx_amiga_adopt_signal;
        thread_ptr -> tx_thread_amiga_flags      =  TX_AMIGA_THREAD_ADOPTED;

        _tx_amiga_adopt_task   =  (VOID *) 0;
        _tx_amiga_adopt_signal =  0UL;
        return;
    }

    /* ---- ThreadX-created thread ----------------------------------------- */

    /* SIGF_SINGLE is the run signal for tasks we create.  It is permanently
       allocated by Exec and is private to the task, so there is no window
       between AddTask() and the task's first AllocSignal() during which the
       scheduler could poke a bit that does not exist.  Exec signals latch, so
       a dispatch that beats the task to its first Wait() is not lost.  */
    thread_ptr -> tx_thread_amiga_run_signal =  SIGF_SINGLE;

    task =  _tx_amiga_task_create(name,
                                  (BYTE) TX_AMIGA_TASK_PRIORITY,
                                  _tx_amiga_thread_entry,
                                  thread_ptr -> tx_thread_stack_start,
                                  thread_ptr -> tx_thread_stack_size,
                                  (APTR) thread_ptr);

    thread_ptr -> tx_thread_amiga_task =  (VOID *) task;
}


/*
 * Entry point of every Exec Task that backs a ThreadX thread.  The TX_THREAD
 * comes out of the task's own control block, which tc_UserData identifies and
 * which is set up before AddTask().
 */
static VOID _tx_amiga_thread_entry(VOID)
{

TX_THREAD               *thread_ptr;
struct _tx_amiga_ctrl   *ctrl;


    ctrl =  _tx_amiga_ctrl_of(FindTask((STRPTR) 0));
    if (ctrl == (struct _tx_amiga_ctrl *) 0)
    {
        Wait(0UL);                                   /* cannot happen */
    }
    thread_ptr =  ctrl -> ctrl_thread;

    TXTRACE("TXT entry task=%08lx thr=%08lx", (LONG) FindTask((STRPTR) 0), (LONG) thread_ptr);

    /* Do not touch a single ThreadX structure until the scheduler says so.  */
    (VOID) _tx_amiga_thread_park(thread_ptr);

    TXTRACE("TXT dispatched task=%08lx", (LONG) FindTask((STRPTR) 0));

    _tx_thread_shell_entry();

    /* _tx_thread_shell_entry() is not supposed to return: a completed thread
       suspends itself and _tx_thread_system_return() parks it here forever.
       It does return if _tx_thread_system_suspend() declines to switch (the
       preempt-disable / system-state guard), and falling off the end of an
       Exec task entry point lands in Exec's default finaliser, which removes
       the task without telling the port -- leaving tx_thread_amiga_task
       pointing at freed memory.  Go through the port's own teardown instead.  */

    TXTRACE("TXT shell_entry returned task=%08lx", (LONG) FindTask((STRPTR) 0));

    _tx_amiga_task_destroy(ctrl);                    /* never returns */
}
