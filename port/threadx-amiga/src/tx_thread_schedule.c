/***************************************************************************
 * Eclipse ThreadX, AmigaOS/m68k port.
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
/*    tx_kernel_enter() and is the only place a ThreadX thread is started. */
/*                                                                        */
/*    A thread runs if and only if it is _tx_thread_current_ptr.  The      */
/*    dispatcher therefore refuses to give the baton out while             */
/*    _tx_thread_current_ptr is non-NULL, which makes the model safe       */
/*    against a third party (an adopting Task, the tick Task) waking it at */
/*    an arbitrary moment.  The Linux port relies on the yielding thread   */
/*    having NULLed the pointer before it posts; making the check explicit */
/*    costs one compare and removes a class of double-dispatch race.       */
/*                                                                        */
/*    "Runs forever" has one exception: tx_amiga_kernel_stop() sets        */
/*    _tx_amiga_kernel_stopping and pokes the scheduler signal, and this   */
/*    function then returns, through _tx_initialize_kernel_enter() and   */
/*    back into _tx_amiga_kernel_task_entry(), which destroys the master   */
/*    Task.  By then stop has already reaped every thread, so the loop     */
/*    below is sitting in its idle wait when the flag arrives.             */
/*                                                                        */
/**************************************************************************/

#define TX_SOURCE_CODE

#include "tx_amiga_internal.h"

#include <devices/timer.h>


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


/* ----------------------------------------------------------- teardown --- */

/*
 * Remove the Exec Task backing a thread that ThreadX has finished with.
 *
 * The task is normally parked in Wait() inside _tx_amiga_thread_park().
 * Setting ctrl_die and poking its run signal makes it fall out of that Wait
 * and destroy itself.  It signals back first, under Forbid(), and only then
 * calls RemTask(NULL), Exec discards the forbid nesting of a task it is
 * removing, so the "signal the reaper then die" pair is atomic and the reaper
 * cannot observe a half-removed task.
 *
 * The wait is bounded.  A ThreadX thread may be blocked inside Exec rather than
 * on its run signal, a SANA-II reader parked in WaitIO() on a device that
 * ignores AbortIO() is the case that matters.  Such a task cannot be woken by
 * the port and cannot be removed by it either: RemTask() on another task would
 * leave that device holding a queued IORequest that it will later write into
 * freed memory.  There is therefore no interleaving in which an unbounded
 * Wait() terminates, and the caller, likely holding the ThreadX baton,
 * would become permanently unrunnable, taking the whole stack with it.
 *
 * The defined outcome when the task cannot be woken is a zombie:
 *
 *   - the task is detached from its TX_THREAD in both directions, so neither
 *     side can ever dereference the other again;
 *   - if it still held the baton, the baton is taken back and the scheduler
 *     restarted, so the rest of ThreadX survives;
 *   - the task keeps its own Task/MemList blocks and destroys itself at its
 *     next port entry point, whenever that finally happens;
 *   - _tx_amiga_zombies is bumped.  tx_amiga_zombie_tasks() reports it, and a
 *     caller that sees it move must not free that thread's stack: the zombie
 *     is still running on it.
 *
 * Adopted threads are never reaped: their Exec Task belongs to the application.
 */

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
        ((thread_ptr -> tx_thread_amiga_flags & TX_AMIGA_THREAD_ADOPTED) != 0U))
    {

        /* Nothing of ours to remove.  */
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

        /* No spare signal: ask the task to die without a handshake, then fall
           through to the explicit zombie bookkeeping below.  The caller still
           cannot free the stack while a live zombie remains.  */
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

                /* No timer could be opened.  One wait is all we can offer; the
                   handshake either completed or the task is unreachable, and
                   either way going round again would just block forever.  */
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

    /* Decide the outcome under Forbid.  While reaped is still zero the dying
       task has not reached its teardown, so its control block is necessarily
       still allocated and safe to touch.  */
    wake =  TX_FALSE;

    Forbid();
    if ((sigmask == 0UL) || (reaped == 0UL))
    {

        /* With no handshake signal the task may already have destroyed
           itself, including its control block.  Re-read the owner pointer
           under Forbid() before dereferencing it.  */
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

            /* Mark it, so that _tx_amiga_task_destroy() can take it back off
               the live-zombie count when it eventually unblocks.  Without the
               mark nothing would ever clear that count, and
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


/*
 * _tx_amiga_dispatch_inline() for callers outside this directory.
 *
 * The inline itself lives in tx_amiga_internal.h, which is PRIVATE to the port
 * target, so the netstack's release/acquire bracket could not reach it and
 * poked the scheduler Task unconditionally instead -- the three-context-switch
 * handoff every path in here stopped doing (docs/RESEARCH.md 89).  Those two
 * sites are the last ones outside the port, and they are the hottest: a wire
 * transfer reacquires the baton about once per receive wake.
 *
 * The core lock is held and the baton is already released.  TX_TRUE means the
 * dispatch did not happen AND the scheduler Task is the only thing that can
 * make it happen, so the caller must call _tx_amiga_wake_scheduler() once it
 * drops the lock.  The other two ways to fail need no poke, see
 * _tx_amiga_wake_needed().
 */
UINT _tx_amiga_dispatch_or_wake(VOID)
{

    return(_tx_amiga_wake_needed(_tx_amiga_dispatch_inline()));
}
