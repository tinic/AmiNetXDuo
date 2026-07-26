/***************************************************************************
 * AmiNetXDuo -- ThreadX thread adoption for pre-existing Exec Tasks.
 *
 * SPDX-License-Identifier: MIT
 **************************************************************************/

/**************************************************************************/
/*                                                                        */
/*    tx_amiga_adopt_thread / tx_amiga_orphan_thread   AmigaOS/m68k       */
/*                                                                        */
/*  WHY THIS EXISTS                                                       */
/*                                                                        */
/*    NetX Duo suspends THE CALLING THREAD.  nx_tcp_socket_receive(),      */
/*    nx_packet_allocate(), nx_tcp_socket_send() and ~40 other core files  */
/*    reach into the TX_THREAD control block of whoever called them and    */
/*    park it on a suspension list (docs/RESEARCH.md 5.2).  On AmigaOS the */
/*    callers are application Exec Tasks that ThreadX never created, so    */
/*    either they become ThreadX threads (this file) or every socket call  */
/*    is marshalled to a worker pool (docs/RESEARCH.md 6.3, option B).     */
/*                                                                        */
/*  HOW IT WORKS                                                          */
/*                                                                        */
/*    tx_amiga_adopt_thread() allocates one Exec signal in the calling     */
/*    Task, then drives the ordinary _tx_thread_create() path with         */
/*    _tx_amiga_adopt_task set, which makes _tx_thread_stack_build() bind  */
/*    to the existing Task instead of spawning one.  The thread is created */
/*    TX_AUTO_START, so it is READY the moment it exists, and the call     */
/*    does not return until the Task holds the baton -- i.e. until it is   */
/*    _tx_thread_current_ptr and no other ThreadX thread is running.       */
/*                                                                        */
/*    Both create and delete happen with _tx_thread_system_state raised.   */
/*    That is not decoration: _tx_thread_system_resume() ends in           */
/*    _tx_thread_system_return() whenever the new thread outranks          */
/*    _tx_thread_execute_ptr, and at that instant the calling Task is NOT  */
/*    yet a ThreadX thread and does NOT hold the baton.  Raising           */
/*    system_state turns those windows into "interrupt" context, where     */
/*    ThreadX defers every context switch to the caller.                   */
/*                                                                        */
/*  WHAT THE BATON DOES AND DOES NOT CLOSE -- read before using this       */
/*                                                                        */
/*    Closed: concurrent mutation of ThreadX ready lists, suspension       */
/*    lists and _tx_thread_current_ptr by Exec's preemptive scheduler.     */
/*    Every access is inside Forbid(), which stops all task switching,     */
/*    and only the baton holder executes ThreadX code at all.              */
/*                                                                        */
/*    Closed: an adopted Task being preempted by Exec mid-update.  Exec    */
/*    can still preempt it (Forbid is not held between ThreadX calls), but */
/*    the preempting task cannot enter ThreadX unless it too holds the     */
/*    baton, and it cannot hold the baton because we do.                   */
/*                                                                        */
/*    NOT closed: an adopted Task that blocks on something other than      */
/*    ThreadX while holding the baton.  Wait() on an Intuition port, a DOS */
/*    packet or a device IORequest leaves the baton held by a task that    */
/*    is not runnable, and the entire stack -- IP thread, timer thread,    */
/*    every other socket user -- stops behind it.  Nothing in the port can */
/*    detect this; it is a contract on the caller.  Hence: adopt on entry  */
/*    to a stack call, orphan on exit, never hold the baton across         */
/*    application code.                                                    */
/*                                                                        */
/*    NOT closed: a Task that is terminated by Exec (or crashes) while     */
/*    adopted takes the baton to the grave.  A shared library can defend   */
/*    against the tidy case (its own Close vector) but not against a       */
/*    Ctrl-C handler that RemTask()s itself.                               */
/*                                                                        */
/*    Bounded, not closed: priority.  A higher-priority ThreadX thread     */
/*    made ready by the tick does not preempt the baton holder             */
/*    asynchronously -- see tx_thread_context_restore.c.  It runs at the   */
/*    holder's next ThreadX service call.                                  */
/*                                                                        */
/*  THE FALLBACK, FOR COMPARISON (docs/RESEARCH.md 6.3, option B)         */
/*                                                                        */
/*    A worker pool would replace this file with, roughly:                */
/*                                                                        */
/*      - N ThreadX threads created by tx_thread_create(), each looping   */
/*        on a tx_queue_receive() of request blocks;                      */
/*      - a request block per call: opcode, arguments, result, plus the   */
/*        caller's struct Task * and a signal mask;                       */
/*      - a bsdsocket entry stub that fills a request, tx_queue_send()s   */
/*        it, and Wait()s on its own Exec signal, which the worker pokes  */
/*        on completion;                                                  */
/*      - a cancellation path, because WaitSelect() must abort on an Exec */
/*        break signal while a worker is parked inside                    */
/*        nx_tcp_socket_receive() -- that means tx_thread_wait_abort() on */
/*        the worker plus a protocol for what the worker does next.       */
/*                                                                        */
/*    What it buys: no application task ever holds the baton, so the      */
/*    "adopted task blocks outside ThreadX" hazard disappears entirely,   */
/*    and a crashing application cannot wedge the stack.                  */
/*                                                                        */
/*    What it costs: two extra Exec context switches AND a queue          */
/*    round-trip per socket call instead of the direct path; one worker   */
/*    tied up for the whole duration of every blocking call, so N bounds  */
/*    the number of concurrently blocked sockets in the machine; and      */
/*    WaitSelect() over M sockets becomes an M-worker problem or needs a  */
/*    second, callback-driven mechanism.  It also needs a worker stack    */
/*    per worker (4 KB each) whether or not anyone is using the stack --  */
/*    on a 4 MB machine that is a standing cost, where adoption borrows   */
/*    the caller's existing stack for free.                               */
/*                                                                        */
/*    Adoption is the better trade here provided the "never block outside */
/*    ThreadX while adopted" contract is kept inside bsdsocket.library,   */
/*    where it is one library's discipline rather than every             */
/*    application's.                                                      */
/*                                                                        */
/**************************************************************************/

#define TX_SOURCE_CODE

#include "tx_amiga_internal.h"


/*
 * Entry function recorded in the TX_THREAD of an adopted thread.  It is never
 * invoked: an adopted Task enters ThreadX through tx_amiga_adopt_thread(), not
 * through _tx_thread_shell_entry().  If it ever were called, parking is the
 * only safe thing to do.
 */
static VOID _tx_amiga_adopted_entry(ULONG id)
{

    (VOID) id;
    Wait(0UL);
}


/* Signal bit number for a single-bit mask, or -1.  */
static BYTE _tx_amiga_sigbit(ULONG sigmask)
{

BYTE    bit;


    for (bit = 0; bit < 32; bit++)
    {
        if (sigmask == (1UL << ((ULONG) bit)))
        {
            return(bit);
        }
    }
    return((BYTE) -1);
}


TX_THREAD *tx_amiga_adopted_thread(VOID)
{

TX_THREAD   *thread_ptr;
TX_THREAD   *first;
struct Task *me;
ULONG        count;


    me =  FindTask((STRPTR) 0);

    Forbid();

    thread_ptr =  TX_NULL;
    first      =  _tx_thread_created_ptr;

    if (first != TX_NULL)
    {

        thread_ptr =  first;
        for (count = 0; count < _tx_thread_created_count; count++)
        {

            if ((thread_ptr -> tx_thread_amiga_task == (VOID *) me) &&
                ((thread_ptr -> tx_thread_amiga_flags & TX_AMIGA_THREAD_ADOPTED) != 0U))
            {
                break;
            }
            thread_ptr =  thread_ptr -> tx_thread_created_next;
        }

        if (count >= _tx_thread_created_count)
        {
            thread_ptr =  TX_NULL;
        }
    }

    Permit();

    return(thread_ptr);
}


UINT tx_amiga_adopt_thread(TX_THREAD *thread_ptr, CHAR *name, UINT priority)
{

struct Task *me;
BYTE         sig;
ULONG        sigmask;
UINT         status;
ULONG        stack_size;
VOID        *stack_start;


    if (thread_ptr == TX_NULL)
    {
        return(TX_PTR_ERROR);
    }
    if (priority >= ((UINT) TX_MAX_PRIORITIES))
    {
        return(TX_PRIORITY_ERROR);
    }
    if (_tx_amiga_kernel_up == TX_FALSE)
    {
        return(TX_NOT_DONE);
    }

    me =  FindTask((STRPTR) 0);

    /* The run signal must be allocated by the Task that will Wait() on it,
       which is precisely why adoption has to happen on the caller's own
       context and cannot be arranged on its behalf.  */
    sig =  AllocSignal(-1);
    if (sig < 0)
    {
        return(TX_NO_MEMORY);
    }
    sigmask =  1UL << ((ULONG) sig);

    /* Describe the Task's real stack to ThreadX.  Nothing writes to it:
       TX_DISABLE_STACK_FILLING is set and stack checking is unavailable.  */
    stack_start =  (VOID *) me -> tc_SPLower;
    stack_size  =  (ULONG) (((UBYTE *) me -> tc_SPUpper) - ((UBYTE *) me -> tc_SPLower));

    Forbid();

    /* Interrupt context for the duration of create + auto-start resume.  */
    _tx_thread_system_state++;

    _tx_amiga_adopt_task   =  (VOID *) me;
    _tx_amiga_adopt_signal =  sigmask;

    status =  _tx_thread_create(thread_ptr, name, _tx_amiga_adopted_entry, 0UL,
                                stack_start, stack_size,
                                priority, priority,
                                TX_NO_TIME_SLICE, TX_AUTO_START);

    _tx_amiga_adopt_task   =  (VOID *) 0;
    _tx_amiga_adopt_signal =  0UL;

    _tx_thread_system_state--;

    if (status != TX_SUCCESS)
    {
        Permit();
        FreeSignal(sig);
        return(status);
    }

    /* Fast path: the baton is free and we are the chosen thread, so take it
       here instead of round-tripping through the scheduler task.  Saves two
       Exec context switches on every adoption, which matters if a library
       adopts per socket call.  */
    if ((_tx_thread_current_ptr == TX_NULL) &&
        (_tx_thread_execute_ptr == thread_ptr) &&
        (_tx_thread_system_state == ((ULONG) 0)))
    {

        _tx_thread_current_ptr =  thread_ptr;
        thread_ptr -> tx_thread_run_count++;
        _tx_timer_time_slice =  thread_ptr -> tx_thread_time_slice;
        Permit();
        return(TX_SUCCESS);
    }

    Permit();

    /* Slow path: somebody else holds the baton or outranks us.  Wait for it.  */
    _tx_amiga_wake_scheduler();
    (VOID) _tx_amiga_thread_park(thread_ptr);

    return(TX_SUCCESS);
}


/*
 * Give the baton back and go dormant, keeping the TX_THREAD.
 *
 * The order is the point.  The baton is dropped FIRST, so that by the time
 * _tx_thread_suspend() runs the thread is an ordinary ready thread rather than
 * the running one and _tx_thread_system_suspend() has nothing to switch away
 * from.  _tx_thread_system_state is raised across it for the same reason
 * tx_amiga_orphan_thread() raises it: a suspend that decides to switch would
 * do so on behalf of a Task that is no longer a ThreadX thread.
 */
UINT tx_amiga_adopt_suspend(TX_THREAD *thread_ptr)
{

struct Task *me;
ULONG        sigmask;
UINT         wake;


    if (thread_ptr == TX_NULL)
    {
        return(TX_PTR_ERROR);
    }

    me =  FindTask((STRPTR) 0);

    Forbid();

    if (((thread_ptr -> tx_thread_amiga_flags & TX_AMIGA_THREAD_ADOPTED) == 0U) ||
        (thread_ptr -> tx_thread_amiga_task != (VOID *) me) ||
        (thread_ptr -> tx_thread_id != TX_THREAD_ID))
    {
        Permit();
        return(TX_CALLER_ERROR);
    }

    sigmask =  thread_ptr -> tx_thread_amiga_run_signal;

    /* Drop the baton before we stop being runnable.  */
    if (_tx_thread_current_ptr == thread_ptr)
    {
        _tx_thread_current_ptr =  TX_NULL;
        _tx_timer_time_slice   =  ((ULONG) 0);
    }

    _tx_thread_system_state++;

    Permit();

    (VOID) _tx_thread_suspend(thread_ptr);

    Forbid();
    _tx_thread_system_state--;

    /*
     * Wake the scheduler only if there is something for it to dispatch.
     *
     * _tx_thread_execute_ptr == TX_NULL means no ThreadX thread is ready, so
     * the poke would wake the scheduler Task, have it find nothing and put it
     * back to sleep -- two Exec context switches, measured at 202 us on a
     * 14 MHz 68020, which is a quarter of what this whole bracket used to
     * cost.  Skipping it cannot lose a dispatch: every path that makes a
     * thread ready afterwards wakes the scheduler itself, from an interrupt
     * (tx_thread_context_restore.c) or from whoever holds the baton
     * (_tx_thread_system_return).  Read inside the Forbid() that lowers the
     * system state so the answer cannot be stale by the time it is used.
     */
    wake =  (_tx_thread_execute_ptr != TX_NULL) ? ((UINT) TX_TRUE) : ((UINT) TX_FALSE);

    Permit();

    if (wake == (UINT) TX_TRUE)
    {
        _tx_amiga_wake_scheduler();
    }

    /* Anything that latched on the run signal while we were dispatched is
       spent; leaving it set would turn the next park into a spin.  */
    SetSignal(0UL, sigmask);

    return(TX_SUCCESS);
}


/*
 * Come back out of dormancy and take the baton.  The tail of this is the same
 * fast-path-or-park as tx_amiga_adopt_thread(); only the registration is
 * skipped, because the thread is still registered.
 */
UINT tx_amiga_adopt_resume(TX_THREAD *thread_ptr)
{

struct Task *me;


    if (thread_ptr == TX_NULL)
    {
        return(TX_PTR_ERROR);
    }
    if (_tx_amiga_kernel_up == TX_FALSE)
    {
        return(TX_NOT_DONE);
    }

    me =  FindTask((STRPTR) 0);

    Forbid();

    if (((thread_ptr -> tx_thread_amiga_flags & TX_AMIGA_THREAD_ADOPTED) == 0U) ||
        (thread_ptr -> tx_thread_amiga_task != (VOID *) me) ||
        (thread_ptr -> tx_thread_id != TX_THREAD_ID) ||
        (thread_ptr -> tx_thread_state != TX_SUSPENDED) ||
        ((thread_ptr -> tx_thread_amiga_flags &
          (TX_AMIGA_THREAD_DIE | TX_AMIGA_THREAD_ORPHANED)) != 0U))
    {
        Permit();
        return(TX_CALLER_ERROR);
    }

    _tx_thread_system_state++;

    Permit();

    (VOID) _tx_thread_resume(thread_ptr);

    Forbid();
    _tx_thread_system_state--;

    /* The same free-baton fast path tx_amiga_adopt_thread() takes.  */
    if ((_tx_thread_current_ptr == TX_NULL) &&
        (_tx_thread_execute_ptr == thread_ptr) &&
        (_tx_thread_system_state == ((ULONG) 0)))
    {

        _tx_thread_current_ptr =  thread_ptr;
        thread_ptr -> tx_thread_run_count++;
        _tx_timer_time_slice =  thread_ptr -> tx_thread_time_slice;
        Permit();
        return(TX_SUCCESS);
    }

    Permit();

    _tx_amiga_wake_scheduler();

    if (_tx_amiga_thread_park(thread_ptr) != TX_TRUE)
    {

        /* Torn down under us while we waited.  Tell the caller to start over
           with a fresh adoption rather than pretend it holds the baton.  */
        return(TX_CALLER_ERROR);
    }

    return(TX_SUCCESS);
}


UINT tx_amiga_discard_thread(TX_THREAD *thread_ptr)
{

    if (thread_ptr == TX_NULL)
    {
        return(TX_PTR_ERROR);
    }

    Forbid();

    if ((thread_ptr -> tx_thread_id != TX_THREAD_ID) ||
        ((thread_ptr -> tx_thread_amiga_flags & TX_AMIGA_THREAD_ADOPTED) == 0U))
    {
        Permit();
        return(TX_THREAD_ERROR);
    }

    /* Somebody else's Task must not be left as the baton holder.  This should
       be unreachable -- a dormant cached thread never is -- but a stale baton
       stops the whole stack and costs nothing to rule out.  */
    if (_tx_thread_current_ptr == thread_ptr)
    {
        _tx_thread_current_ptr =  TX_NULL;
        _tx_timer_time_slice   =  ((ULONG) 0);
    }

    _tx_thread_system_state++;

    Permit();

    (VOID) _tx_thread_terminate(thread_ptr);
    (VOID) _tx_thread_delete(thread_ptr);

    Forbid();
    _tx_thread_system_state--;
    Permit();

    _tx_amiga_wake_scheduler();

    return(TX_SUCCESS);
}


UINT tx_amiga_orphan_thread(TX_THREAD *thread_ptr)
{

struct Task *me;
ULONG        sigmask;
BYTE         sig;
UINT         wake;


    if (thread_ptr == TX_NULL)
    {
        return(TX_PTR_ERROR);
    }

    me =  FindTask((STRPTR) 0);

    Forbid();

    if (((thread_ptr -> tx_thread_amiga_flags & TX_AMIGA_THREAD_ADOPTED) == 0U) ||
        (thread_ptr -> tx_thread_amiga_task != (VOID *) me))
    {
        Permit();
        return(TX_CALLER_ERROR);
    }

    sigmask =  thread_ptr -> tx_thread_amiga_run_signal;

    if (thread_ptr -> tx_thread_id != TX_THREAD_ID)
    {

        /* Already torn down under us (someone called tx_thread_terminate()
           plus tx_thread_delete()).  Just recover the signal.  */
        thread_ptr -> tx_thread_amiga_task       =  (VOID *) 0;
        thread_ptr -> tx_thread_amiga_run_signal =  0UL;
        Permit();

        SetSignal(0UL, sigmask);
        sig =  _tx_amiga_sigbit(sigmask);
        if (sig >= 0)
        {
            FreeSignal(sig);
        }
        return(TX_SUCCESS);
    }

    /* Drop the baton before we stop being a thread.  */
    if (_tx_thread_current_ptr == thread_ptr)
    {
        _tx_thread_current_ptr =  TX_NULL;
        _tx_timer_time_slice   =  ((ULONG) 0);
    }

    /* Interrupt context again: terminate/delete must not try to switch on our
       behalf now that we are nobody.  */
    _tx_thread_system_state++;

    Permit();

    (VOID) _tx_thread_terminate(thread_ptr);
    (VOID) _tx_thread_delete(thread_ptr);

    Forbid();
    _tx_thread_system_state--;
    wake =  (_tx_thread_execute_ptr != TX_NULL) ? ((UINT) TX_TRUE) : ((UINT) TX_FALSE);
    Permit();

    /* Whoever is next may run now -- if there is anybody.  See the note in
       tx_amiga_adopt_suspend() for why an empty execute pointer means the
       poke can be skipped, and what makes that safe.  */
    if (wake == (UINT) TX_TRUE)
    {
        _tx_amiga_wake_scheduler();
    }

    /* Drop anything that latched on the run signal, then give the bit back.  */
    SetSignal(0UL, sigmask);
    sig =  _tx_amiga_sigbit(sigmask);
    if (sig >= 0)
    {
        FreeSignal(sig);
    }

    return(TX_SUCCESS);
}
