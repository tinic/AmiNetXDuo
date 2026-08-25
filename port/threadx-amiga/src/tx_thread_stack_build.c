/***************************************************************************
 * Eclipse ThreadX, AmigaOS/m68k port.
 *
 * Derived in structure from ports/linux/gnu/src/tx_thread_stack_build.c
 *   Copyright (c) 2024 Microsoft Corporation
 *   Copyright (c) 2026-present Eclipse ThreadX contributors
 *
 * SPDX-License-Identifier: MIT
 **************************************************************************/

/* No stack frame to build in a hosted port: this creates the Exec Task that will
   run the thread.  Adoption binds to the globals _tx_amiga_adopt_task/_signal,
   because _tx_thread_create() zeroes the whole control block before calling us. */

#define TX_SOURCE_CODE

#include "tx_amiga_internal.h"


#ifndef AMINETXDUO_GREEN_REALM
static VOID _tx_amiga_thread_entry(VOID);
#endif


VOID _tx_thread_stack_build(TX_THREAD *thread_ptr, VOID (*function_ptr)(VOID))
{

#ifndef AMINETXDUO_GREEN_REALM
struct Task *task;
#endif
CHAR        *name;


    (VOID) function_ptr;

    thread_ptr -> tx_thread_amiga_suspension_type =  ((UINT) 0);
    thread_ptr -> tx_thread_amiga_flags           =  ((UINT) 0);
    thread_ptr -> tx_thread_amiga_task            =  (VOID *) 0;
    thread_ptr -> tx_thread_amiga_signal_owner    =  (VOID *) 0;
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

        thread_ptr -> tx_thread_amiga_task         =  _tx_amiga_adopt_task;
        /* The same Task, recorded twice on purpose, see tx_port.h.  Teardown
           clears the first one and must not clear this one.  */
        thread_ptr -> tx_thread_amiga_signal_owner =  _tx_amiga_adopt_task;
        thread_ptr -> tx_thread_amiga_run_signal   =  _tx_amiga_adopt_signal;
        thread_ptr -> tx_thread_amiga_flags        =  TX_AMIGA_THREAD_ADOPTED;

        _tx_amiga_adopt_task   =  (VOID *) 0;
        _tx_amiga_adopt_signal =  0UL;
        return;
    }

    /* ---- ThreadX-created thread ----------------------------------------- */

#ifdef AMINETXDUO_GREEN_REALM

    /* A request-gate proxy: green identity, but NO initial frame.  A frame laid at
       stack_end here would scribble on the top of the owning Task's live stack,
       which is what the "stack" of a gate proxy is.  */

    if (_tx_amiga_gate_bind_pending != 0U)
    {
        thread_ptr -> tx_thread_amiga_task       =  _tx_amiga_scheduler_task;
        thread_ptr -> tx_thread_amiga_run_signal =  _tx_amiga_scheduler_signal;
        thread_ptr -> tx_thread_amiga_flags      =  TX_AMIGA_THREAD_GREEN;
        (VOID) name;
        return;
    }

    /* Green realm: no Exec Task.  tx_thread_amiga_task points at the realm Task so
       every "is the caller the baton holder" test answers correctly while this
       context runs; the run signal is the scheduler's, so a stray poke wakes it. */

    thread_ptr -> tx_thread_amiga_task       =  _tx_amiga_scheduler_task;
    thread_ptr -> tx_thread_amiga_run_signal =  _tx_amiga_scheduler_signal;
    thread_ptr -> tx_thread_amiga_flags      =  TX_AMIGA_THREAD_GREEN;

    _tx_green_stack_build(thread_ptr);

    (VOID) name;
    return;

#else /* !AMINETXDUO_GREEN_REALM */

    /* SIGF_SINGLE is the run signal for tasks we create: permanently allocated by
       Exec and private to the task, so there is no window between AddTask() and the
       task's first AllocSignal() in which the scheduler could poke a missing bit. */
    thread_ptr -> tx_thread_amiga_run_signal =  SIGF_SINGLE;

    task =  _tx_amiga_task_create(name,
                                  (BYTE) TX_AMIGA_TASK_PRIORITY,
                                  _tx_amiga_thread_entry,
                                  thread_ptr -> tx_thread_stack_start,
                                  thread_ptr -> tx_thread_stack_size,
                                  (APTR) thread_ptr);

    thread_ptr -> tx_thread_amiga_task =  (VOID *) task;

#endif /* AMINETXDUO_GREEN_REALM */
}


#ifndef AMINETXDUO_GREEN_REALM
/* Entry point of every Exec Task that backs a ThreadX thread.  The TX_THREAD comes
   out of the task's own control block, which tc_UserData identifies and which is
   set up before AddTask().  */
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

    /* _tx_thread_shell_entry() does return if _tx_thread_system_suspend() declines
       to switch, and falling off an Exec entry point lands in Exec's finaliser,
       which removes the task without telling the port.  Use the port's teardown. */

    TXTRACE("TXT shell_entry returned task=%08lx", (LONG) FindTask((STRPTR) 0));

    _tx_amiga_task_destroy(ctrl);                    /* never returns */
}
#endif /* !AMINETXDUO_GREEN_REALM */
