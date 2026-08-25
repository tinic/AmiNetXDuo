/***************************************************************************
 * Eclipse ThreadX, AmigaOS/m68k port.
 *
 * Derived in structure from ports/linux/gnu/src/tx_thread_schedule.c
 *   Copyright (c) 2024 Microsoft Corporation
 *   Copyright (c) 2026-present Eclipse ThreadX contributors
 *
 * SPDX-License-Identifier: MIT
 **************************************************************************/

/* _tx_thread_schedule, AmigaOS/m68k: the baton dispatcher, running forever on the
   Exec Task that called tx_kernel_enter().  It refuses to hand the baton out while
   _tx_thread_current_ptr is non-NULL; tx_amiga_kernel_stop() makes it return.  */

#define TX_SOURCE_CODE

#include "tx_amiga_internal.h"

#include <devices/timer.h>


#ifdef AMINETXDUO_GREEN_REALM

/* The green realm scheduler: a GREEN thread is entered by stack switch, an ADOPTED
   one still gets the baton by Signal().  PROTOCOL for every _tx_green_switch():
   one Forbid() by the side that switches away, one Permit() by the resumer.  */

VOID _tx_thread_schedule(VOID)
{

TX_THREAD   *thread_ptr;
ULONG        pending;
ULONG        mask;


    if (_tx_amiga_scheduler_signal == 0UL)
    {
        Wait(0UL);
    }

    pending =  0UL;

    Forbid();

    for (;;)
    {

        /* The tick merge: once the VERTB server targets the realm (tr_realm),
           tick servicing happens here, in passing, at every scheduler pass.  The
           service is E-Clock-based, so coming by often costs a ReadEClock.  */
        if (_tx_amiga_tick_run.tr_realm != ((UINT) TX_FALSE))
        {
            _tx_amiga_tick_deliver((UINT) TX_TRUE);
        }

        /* Deliver Exec signals to green waiters.  Consume ONLY registered
           waiters' bits -- an unregistered thread's signal must stay latched for
           its own next wait.  */
        mask =  _tx_green_pending_union();
        if (mask != 0UL)
        {
            pending |=  SetSignal(0UL, mask) & mask;
        }
        if (pending != 0UL)
        {
            _tx_green_deliver(pending);
            pending =  0UL;
        }

        if (_tx_amiga_kernel_stopping != TX_FALSE)
        {
            Permit();
            break;
        }

        thread_ptr =  _tx_thread_execute_ptr;

        if ((thread_ptr != TX_NULL) &&
            (_tx_thread_current_ptr == TX_NULL) &&
            (_tx_thread_system_state == ((ULONG) 0)))
        {

            /* Dispatch.  */
            TX_AMIGA_COUNT(TX_AMIGA_SC_SCHED_DISPATCH);
            _tx_thread_current_ptr =  thread_ptr;
            thread_ptr -> tx_thread_run_count++;
            _tx_timer_time_slice =  thread_ptr -> tx_thread_time_slice;
            thread_ptr -> tx_thread_amiga_suspension_type =  ((UINT) 0);

            ami_budget_hold_start();

            if (_tx_amiga_thread_green(thread_ptr) != TX_FALSE)
            {

                /* Enter the green context.  Returns when a green thread yields
                   back; the Forbid() travels with the switch.  */
                _tx_green_counters.gc_switches++;
                _tx_green_switch(&_tx_green_scheduler_sp,
                                 thread_ptr -> tx_thread_stack_ptr);
                continue;
            }

            /* An adopted thread: the baton goes out by Signal, and comes
               back by a poke on the scheduler signal.  */
            _tx_green_counters.gc_external++;
            _tx_amiga_signal(thread_ptr -> tx_thread_amiga_task,
                             thread_ptr -> tx_thread_amiga_run_signal);
        }

        /* Nothing dispatchable.  Sleep on the scheduler signal AND the green
           waiters' signals: a device reply must wake the realm even while an
           adopted caller holds the baton.  */
        mask =  _tx_amiga_scheduler_signal | _tx_green_pending_union();
        Permit();
        TX_AMIGA_COUNT(TX_AMIGA_SC_SCHED_WAIT);
        _tx_green_counters.gc_idle_waits++;
        pending =  Wait(mask) & ~_tx_amiga_scheduler_signal;
        Forbid();
    }
}

#else /* !AMINETXDUO_GREEN_REALM */

VOID _tx_thread_schedule(VOID)
{

TX_THREAD   *thread_ptr;


    if (_tx_amiga_scheduler_signal == 0UL)
    {

        /* _tx_initialize_low_level() could not allocate a signal, so there is
           no way to release the baton.  Park rather than spin.  */
        Wait(0UL);
    }

    for (;;)
    {

        /* Wait for a thread to execute, with the baton free and no
           "interrupt" (tick) in progress.  */
        Forbid();
        while ((_tx_amiga_kernel_stopping == TX_FALSE) &&
               ((_tx_thread_execute_ptr == TX_NULL) ||
                (_tx_thread_current_ptr != TX_NULL) ||
                (_tx_thread_system_state != ((ULONG) 0))))
        {

            Permit();
            TX_AMIGA_COUNT(TX_AMIGA_SC_SCHED_WAIT);
            Wait(_tx_amiga_scheduler_signal);
            Forbid();
        }

        if (_tx_amiga_kernel_stopping != TX_FALSE)
        {

            /* Leave the loop with the baton free and nothing dispatched.  The
               caller (_tx_amiga_kernel_task_entry) removes this Task.  */
            Permit();
            break;
        }

        thread_ptr =  _tx_thread_execute_ptr;

        /* Pass the baton on.  */
        TX_AMIGA_COUNT(TX_AMIGA_SC_SCHED_DISPATCH);
        _tx_thread_current_ptr =  thread_ptr;
        thread_ptr -> tx_thread_run_count++;
        _tx_timer_time_slice =  thread_ptr -> tx_thread_time_slice;

        thread_ptr -> tx_thread_amiga_suspension_type =  ((UINT) 0);

        ami_budget_hold_start();

        _tx_amiga_signal(thread_ptr -> tx_thread_amiga_task,
                         thread_ptr -> tx_thread_amiga_run_signal);

        Permit();

        /* Sleep until the thread (or the tick) says something changed.  */
        TX_AMIGA_COUNT(TX_AMIGA_SC_SCHED_WAIT);
        Wait(_tx_amiga_scheduler_signal);
    }
}

#endif /* AMINETXDUO_GREEN_REALM */


/* ----------------------------------------------------------- teardown --- */

/* Remove the Exec Task backing a thread ThreadX has finished with.  The wait is
   bounded: a task blocked inside Exec can be neither woken nor safely removed, so
   the outcome is a detached zombie whose stack must not be freed.  */

#ifndef TX_AMIGA_REAP_TIMEOUT_SECS
#define TX_AMIGA_REAP_TIMEOUT_SECS      2UL
#endif


static VOID _tx_amiga_reap_cleanup(struct timerequest *tr, struct MsgPort *port, BYTE sig)
{

    if (tr != (struct timerequest *) 0)
    {
        CloseDevice((struct IORequest *) tr);
        DeleteIORequest((APTR) tr);
    }
    if (port != (struct MsgPort *) 0)
    {
        DeleteMsgPort(port);
    }
    if (sig >= 0)
    {
        FreeSignal(sig);
    }
}


static VOID _tx_amiga_reap(TX_THREAD *thread_ptr)
{

struct Task             *task;
struct Task             *me;
struct _tx_amiga_ctrl   *ctrl;
struct MsgPort          *port;
struct timerequest      *tr;
BYTE                     sig;
ULONG                    sigmask;
ULONG                    portsig;
ULONG                    signals;
volatile ULONG           reaped;
UINT                     wake;


    TXTRACE("TXT reap enter thr=%08lx task=%08lx flags=%ld by=%08lx",
            (LONG) thread_ptr, (LONG) thread_ptr -> tx_thread_amiga_task,
            (LONG) thread_ptr -> tx_thread_amiga_flags, (LONG) FindTask((STRPTR) 0));

    Forbid();
    task =  (struct Task *) thread_ptr -> tx_thread_amiga_task;
    if ((task == (struct Task *) 0) ||
        ((thread_ptr -> tx_thread_amiga_flags &
          (TX_AMIGA_THREAD_ADOPTED | TX_AMIGA_THREAD_GREEN)) != 0U))
    {

            /* Nothing of ours to remove: no Task, an adopted one (the
               application's), or a green thread, whose "task" is the realm.  */
#ifdef AMINETXDUO_GREEN_REALM
        _tx_green_forget(thread_ptr);
#endif
        thread_ptr -> tx_thread_amiga_task =  (VOID *) 0;
        Permit();
        return;
    }
    Permit();

    me      =  FindTask((STRPTR) 0);
    sig     =  AllocSignal(-1);
    reaped  =  0UL;
    portsig =  0UL;
    tr      =  (struct timerequest *) 0;
    port    =  (struct MsgPort *) 0;

    /* The timeout source.  Set up before the handshake so that the window
       between "die" and "waiting" is as short as possible.  */
    if (sig >= 0)
    {
        port =  CreateMsgPort();
        if (port != (struct MsgPort *) 0)
        {
            tr =  (struct timerequest *) CreateIORequest(port, (ULONG) sizeof(struct timerequest));
            if (tr != (struct timerequest *) 0)
            {
                if (OpenDevice((CONST_STRPTR) "timer.device", (ULONG) UNIT_VBLANK,
                               (struct IORequest *) tr, 0UL) != 0)
                {
                    DeleteIORequest((APTR) tr);
                    tr =  (struct timerequest *) 0;
                }
                else
                {
                    portsig =  1UL << ((ULONG) port -> mp_SigBit);
                }
            }
            if (tr == (struct timerequest *) 0)
            {
                DeleteMsgPort(port);
                port =  (struct MsgPort *) 0;
            }
        }
    }

    Forbid();

    /* Re-check: the task may have gone while we were unforbidden.  */
    task =  (struct Task *) thread_ptr -> tx_thread_amiga_task;
    ctrl =  (task != (struct Task *) 0) ? _tx_amiga_ctrl_of(task)
                                        : ((struct _tx_amiga_ctrl *) 0);
    if (ctrl == (struct _tx_amiga_ctrl *) 0)
    {
        thread_ptr -> tx_thread_amiga_task =  (VOID *) 0;
        Permit();
        _tx_amiga_reap_cleanup(tr, port, sig);
        return;
    }

    thread_ptr -> tx_thread_amiga_flags |=  TX_AMIGA_THREAD_DIE;

    if (sig >= 0)
    {

        sigmask =  1UL << ((ULONG) sig);
        ctrl -> ctrl_reaper        =  me;
        ctrl -> ctrl_reaper_signal =  sigmask;
        ctrl -> ctrl_reaped        =  &reaped;
    }
    else
    {

            /* No spare signal: ask the task to die without a handshake and fall
               through to the zombie bookkeeping below.  */
        sigmask =  0UL;
        ctrl -> ctrl_reaper        =  (struct Task *) 0;
        ctrl -> ctrl_reaper_signal =  0UL;
        ctrl -> ctrl_reaped        =  (volatile ULONG *) 0;
    }

    ctrl -> ctrl_die =  1U;

    Signal(task, thread_ptr -> tx_thread_amiga_run_signal);
    Permit();

    if (sigmask != 0UL)
    {
        TXTRACE("TXT reap wait thr=%08lx", (LONG) thread_ptr);

        if (tr != (struct timerequest *) 0)
        {
            tr -> tr_node.io_Command =  TR_ADDREQUEST;
            tr -> tr_time.tv_secs    =  (ULONG) TX_AMIGA_REAP_TIMEOUT_SECS;
            tr -> tr_time.tv_micro   =  0UL;
            SendIO((struct IORequest *) tr);
        }

        for (;;)
        {

            signals =  Wait(sigmask | portsig);

            if (reaped != 0UL)
            {
                break;
            }
            if ((portsig != 0UL) && ((signals & portsig) != 0UL))
            {
                break;                          /* timed out */
            }
            if (portsig == 0UL)
            {

                /* No timer could be opened.  One wait is all we can offer; going
                   round again would just block forever.  */
                break;
            }
        }

        if (tr != (struct timerequest *) 0)
        {
            if (CheckIO((struct IORequest *) tr) == (struct IORequest *) 0)
            {
                AbortIO((struct IORequest *) tr);
            }
            WaitIO((struct IORequest *) tr);
        }
    }

    /* Decide the outcome under Forbid.  While reaped is zero the dying task has
       not reached its teardown, so its control block is still allocated.  */
    wake =  TX_FALSE;

    Forbid();
    if ((sigmask == 0UL) || (reaped == 0UL))
    {

            /* With no handshake signal the task may already have destroyed
               itself, control block included: re-read the owner pointer under
               Forbid() before dereferencing it.  */
        task =  (struct Task *) thread_ptr -> tx_thread_amiga_task;
        ctrl =  (task != (struct Task *) 0) ? _tx_amiga_ctrl_of(task)
                                            : ((struct _tx_amiga_ctrl *) 0);
        if (ctrl != (struct _tx_amiga_ctrl *) 0)
        {

            /* Detach in both directions.  */
            ctrl -> ctrl_thread        =  TX_NULL;
            ctrl -> ctrl_reaper        =  (struct Task *) 0;
            ctrl -> ctrl_reaper_signal =  0UL;
            ctrl -> ctrl_reaped        =  (volatile ULONG *) 0;

            /* Mark it, so _tx_amiga_task_destroy() can take it off the
               live-zombie count when it unblocks; without the mark
               tx_amiga_kernel_stop() would refuse forever after one zombie.  */
            ctrl -> ctrl_zombie        =  1U;
            _tx_amiga_zombies_live++;

            _tx_amiga_zombies++;
        }

        thread_ptr -> tx_thread_amiga_task =  (VOID *) 0;

        /* Take the baton back if the zombie was holding it, or nothing in
           ThreadX will ever run again.  */
        if (_tx_thread_current_ptr == thread_ptr)
        {
            ami_budget_hold_end((APTR) thread_ptr, thread_ptr -> tx_thread_name,
                                (ULONG) thread_ptr -> tx_thread_state,
                                AMI_HOLD_SITE_REAP);
            _tx_thread_current_ptr =  TX_NULL;
            _tx_timer_time_slice   =  ((ULONG) 0);
            wake =  TX_TRUE;
        }
    }
    Permit();

    if (wake != TX_FALSE)
    {
        _tx_amiga_wake_scheduler();
    }

    _tx_amiga_reap_cleanup(tr, port, sig);

    TXTRACE("TXT reap done thr=%08lx reaped=%ld zombies=%ld",
            (LONG) thread_ptr, (LONG) reaped, (LONG) _tx_amiga_zombies);
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


/* _tx_amiga_dispatch_inline() for callers outside this directory (the netstack's
   release/acquire bracket).  The core lock is held and the baton already released;
   TX_TRUE means the caller must _tx_amiga_wake_scheduler() once it drops the lock. */
UINT _tx_amiga_dispatch_or_wake(VOID)
{

    return(_tx_amiga_wake_needed(_tx_amiga_dispatch_inline()));
}
