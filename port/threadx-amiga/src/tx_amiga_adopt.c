/***************************************************************************
 * AmiNetXDuo, ThreadX thread adoption for pre-existing Exec Tasks.
 *
 * SPDX-License-Identifier: MIT
 **************************************************************************/

/**************************************************************************/
/*                                                                        */
/*    tx_amiga_adopt_thread / tx_amiga_orphan_thread   AmigaOS/m68k       */
/*                                                                        */
/*    NetX Duo suspends the calling thread.  nx_tcp_socket_receive(),      */
/*    nx_packet_allocate(), nx_tcp_socket_send() and ~40 other core files  */
/*    reach into the TX_THREAD control block of whoever called them and    */
/*    park it on a suspension list (docs/RESEARCH.md 5.2).  On AmigaOS the */
/*    callers are application Exec Tasks that ThreadX never created, so    */
/*    either they become ThreadX threads (this file) or every socket call  */
/*    is marshalled to a worker pool (docs/RESEARCH.md 6.3, option B).     */
/*                                                                        */
/*    tx_amiga_adopt_thread() allocates one Exec signal in the calling     */
/*    Task, then drives the ordinary _tx_thread_create() path with         */
/*    _tx_amiga_adopt_task set, which makes _tx_thread_stack_build() bind  */
/*    to the existing Task instead of spawning one.  The thread is created */
/*    TX_AUTO_START, so it is ready the moment it exists, and the call     */
/*    does not return until the Task holds the baton: until it is          */
/*    _tx_thread_current_ptr and no other ThreadX thread is running.       */
/*                                                                        */
/*    Both create and delete happen with _tx_thread_system_state raised.   */
/*    _tx_thread_system_resume() ends in _tx_thread_system_return()        */
/*    whenever the new thread outranks _tx_thread_execute_ptr, and at that */
/*    instant the calling Task is not yet a ThreadX thread and does not    */
/*    hold the baton.  Raising system_state turns those windows into       */
/*    "interrupt" context, where ThreadX defers every context switch to    */
/*    the caller.                                                          */
/*                                                                        */
/*    What holding the baton closes:                                       */
/*                                                                        */
/*    - concurrent mutation of ThreadX ready lists, suspension lists and   */
/*      _tx_thread_current_ptr by Exec's preemptive scheduler.  Every      */
/*      access is inside Forbid(), which stops all task switching, and     */
/*      only the baton holder executes ThreadX code at all.                */
/*    - an adopted Task being preempted by Exec mid-update.  Exec can      */
/*      still preempt it (Forbid is not held between ThreadX calls), but   */
/*      the preempting task cannot enter ThreadX unless it too holds the   */
/*      baton, and it cannot while we do.                                  */
/*                                                                        */
/*    What it does not close:                                              */
/*                                                                        */
/*    - an adopted Task that blocks on something other than ThreadX while  */
/*      holding the baton.  Wait() on an Intuition port, a DOS packet or a */
/*      device IORequest leaves the baton held by a task that is not       */
/*      runnable, and the entire stack, IP thread, timer thread, every   */
/*      other socket user, stops behind it.  Nothing in the port can     */
/*      detect this; the caller must adopt on entry to a stack call,       */
/*      orphan on exit, and never hold the baton across application code.  */
/*    - a Task terminated by Exec (or crashing) while adopted never        */
/*      releases the baton.  A shared library can defend against the tidy  */
/*      case (its own Close vector) but not against a Ctrl-C handler that  */
/*      RemTask()s itself.                                                 */
/*    - priority is bounded rather than closed.  A higher-priority         */
/*      ThreadX thread made ready by the tick does not preempt the baton   */
/*      holder asynchronously (see tx_thread_context_restore.c); it runs   */
/*      at the holder's next ThreadX service call.                         */
/*                                                                        */
/*    The alternative (docs/RESEARCH.md 6.3, option B) is a worker pool:  */
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
/*        nx_tcp_socket_receive(), tx_thread_wait_abort() on the worker */
/*        plus a protocol for what the worker does next.                  */
/*                                                                        */
/*    It buys: no application task ever holds the baton, so the "adopted  */
/*    task blocks outside ThreadX" hazard disappears and a crashing       */
/*    application cannot wedge the stack.                                 */
/*                                                                        */
/*    It costs: two extra Exec context switches and a queue round-trip    */
/*    per socket call; one worker tied up for the whole duration of every */
/*    blocking call, so N bounds the number of concurrently blocked       */
/*    sockets in the machine; WaitSelect() over M sockets becomes an      */
/*    M-worker problem or needs a second, callback-driven mechanism; and  */
/*    a 4 KB stack per worker as a standing cost on a 4 MB machine, where */
/*    adoption borrows the caller's existing stack for free.              */
/*                                                                        */
/*    Adoption is the better trade provided the "never block outside      */
/*    ThreadX while adopted" rule is kept inside bsdsocket.library, where */
/*    it is one library's discipline rather than every application's.     */
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


/*
 * TX_TRUE if the calling Task is the ThreadX baton holder.
 *
 * NetX Duo's caller checks ask "is a thread calling me" and the generic answer
 * is _tx_thread_system_state == 0, which on a target means "no ISR is running"
 * a property of the CPU, and therefore the same answer for everybody.  Here
 * interrupt context is a Task holding the core lock, and the teardown paths in
 * this file raise the counter with switching enabled because
 * _tx_thread_delete() can Wait() in the reaper.  So the generic answer is
 * whatever some other Task happens to be doing.
 *
 * This is the question the port can answer exactly, and it rejects everything
 * the generic one rejects: the tick task, the scheduler task and any Task
 * ThreadX has never adopted all fail it, because none of them backs
 * _tx_thread_current_ptr.  See port/netxduo-amiga/inc/nx_port.h.
 */
UINT tx_amiga_caller_is_thread(VOID)
{

TX_THREAD   *current;
UINT         answer;


    Forbid();

    current =  _tx_thread_current_ptr;
    answer  =  ((current != TX_NULL) &&
                (current -> tx_thread_amiga_task == (VOID *) FindTask((STRPTR) 0)))
               ? ((UINT) TX_TRUE) : ((UINT) TX_FALSE);

    Permit();

    return(answer);
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
       which is why adoption has to happen on the caller's own context and
       cannot be arranged on its behalf.  */
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
 * Release the baton and go dormant, keeping the TX_THREAD.
 *
 * The baton is dropped first, so that by the time _tx_thread_suspend() runs the
 * thread is an ordinary ready thread rather than the running one and
 * _tx_thread_system_suspend() has nothing to switch away from.
 * _tx_thread_system_state is raised across it for the same reason
 * tx_amiga_orphan_thread() raises it: a suspend that decides to switch would do
 * so on behalf of a Task that is no longer a ThreadX thread.
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

    /* Release the baton before we stop being runnable.  */
    if (_tx_thread_current_ptr == thread_ptr)
    {
        _tx_thread_current_ptr =  TX_NULL;
        _tx_timer_time_slice   =  ((ULONG) 0);
    }

    _tx_thread_system_state++;

    /* The core lock stays held across the suspend.  _tx_thread_system_state is
       one global counter and every Task reads it, NetX Duo's caller checks
       included, so a window in which it is raised with task switching enabled
       makes every other Task look like an ISR, and a socket call in flight on
       one comes back NX_CALLER_ERROR.  Nothing under _tx_thread_suspend() can
       Wait() while it is raised: every _tx_thread_system_return() in
       tx_thread_system_suspend.c is behind TX_THREAD_SYSTEM_RETURN_CHECK, which
       tests exactly this counter.  */
    (VOID) _tx_thread_suspend(thread_ptr);

    _tx_thread_system_state--;

    /*
     * Wake the scheduler only if there is something for it to dispatch.
     *
     * _tx_thread_execute_ptr == TX_NULL means no ThreadX thread is ready, so
     * the poke would wake the scheduler Task, have it find nothing and put it
     * back to sleep, two Exec context switches, measured at 202 us on a
     * 14 MHz 68020, a quarter of what this bracket used to cost.  Skipping it
     * cannot lose a dispatch: every path that makes a thread ready afterwards
     * wakes the scheduler itself, from an interrupt
     * (tx_thread_context_restore.c) or from whoever holds the baton
     * (_tx_thread_system_return).  Read under the core lock so the answer cannot
     * be stale by the time it is used.
     */
    wake =  (_tx_amiga_dispatch_inline() == ((UINT) TX_FALSE)) &&
            (_tx_thread_execute_ptr != TX_NULL)
            ? ((UINT) TX_TRUE) : ((UINT) TX_FALSE);

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
 * Come back out of dormancy and acquire the baton.  The tail is the same
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

    /* Held across the resume, for the reason tx_amiga_adopt_suspend() gives.  */
    (VOID) _tx_thread_resume(thread_ptr);

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

struct Task *me;
ULONG        sigmask;
BYTE         sig;


    if (thread_ptr == TX_NULL)
    {
        return(TX_PTR_ERROR);
    }

    me =  FindTask((STRPTR) 0);

    Forbid();

    if ((thread_ptr -> tx_thread_id != TX_THREAD_ID) ||
        ((thread_ptr -> tx_thread_amiga_flags & TX_AMIGA_THREAD_ADOPTED) == 0U))
    {
        Permit();
        return(TX_THREAD_ERROR);
    }

    /* Somebody else's Task must not be left as the baton holder.  This should
       be unreachable, since a dormant cached thread never is, but a stale baton
       stops the whole stack and costs nothing to rule out.  */
    if (_tx_thread_current_ptr == thread_ptr)
    {
        _tx_thread_current_ptr =  TX_NULL;
        _tx_timer_time_slice   =  ((ULONG) 0);
    }

    _tx_thread_system_state++;

    /* The core lock stays held, for the reason tx_amiga_adopt_suspend() gives.
       _tx_amiga_reap() is the one thing under delete that Wait()s, and it
       returns at its first test for an adopted thread, which this is, checked
       above, so nothing here blocks.  */
    (VOID) _tx_thread_terminate(thread_ptr);
    (VOID) _tx_thread_delete(thread_ptr);

    _tx_thread_system_state--;

    /* Discard exists for the case where somebody ELSE is tearing this down:
       FreeSignal() only works on the Task that allocated the bit, so a foreign
       caller can only drop the registration and leave the bit to die with its
       owner.  When the owner is the one calling, the bit is recoverable and
       leaving it is a permanent loss out of that Task's 32, netstack.c
       reaches here on the owning Task from both ami_netstack_enter_cached()
       and ami_netstack_release().  */
    sigmask =  0UL;
    if (thread_ptr -> tx_thread_amiga_signal_owner == (VOID *) me)
    {
        sigmask =  thread_ptr -> tx_thread_amiga_run_signal;
        thread_ptr -> tx_thread_amiga_signal_owner =  (VOID *) 0;
        thread_ptr -> tx_thread_amiga_run_signal   =  0UL;
    }

    Permit();

    if (sigmask != 0UL)
    {
        SetSignal(0UL, sigmask);
        sig =  _tx_amiga_sigbit(sigmask);
        if (sig >= 0)
        {
            FreeSignal(sig);
        }
    }

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

    if ((thread_ptr -> tx_thread_amiga_flags & TX_AMIGA_THREAD_ADOPTED) == 0U)
    {
        Permit();
        return(TX_CALLER_ERROR);
    }

    sigmask =  thread_ptr -> tx_thread_amiga_run_signal;

    /* The already-torn-down case is tested BEFORE the tx_thread_amiga_task
       check, and against tx_thread_amiga_signal_owner rather than against it.
       _tx_amiga_reap() zeroes tx_thread_amiga_task for an adopted thread on the
       way through _tx_thread_delete(), so a caller arriving here after that ran
       failed the old ownership test, took TX_CALLER_ERROR, and this branch,
       the one written to recover the bit, could never be reached.  Thirty-two
       adoptions of a torn-down thread on one Task and no adoption on it ever
       succeeds again.  netstack.c's ami_netstack_enter_cached() is the path
       that does exactly this.  */
    if (thread_ptr -> tx_thread_id != TX_THREAD_ID)
    {

        if (thread_ptr -> tx_thread_amiga_signal_owner != (VOID *) me)
        {
            Permit();
            return(TX_CALLER_ERROR);
        }

        thread_ptr -> tx_thread_amiga_task         =  (VOID *) 0;
        thread_ptr -> tx_thread_amiga_signal_owner =  (VOID *) 0;
        thread_ptr -> tx_thread_amiga_run_signal   =  0UL;
        Permit();

        SetSignal(0UL, sigmask);
        sig =  _tx_amiga_sigbit(sigmask);
        if (sig >= 0)
        {
            FreeSignal(sig);
        }
        return(TX_SUCCESS);
    }

    if (thread_ptr -> tx_thread_amiga_task != (VOID *) me)
    {
        Permit();
        return(TX_CALLER_ERROR);
    }

    /* Release the baton before we stop being a thread.  */
    if (_tx_thread_current_ptr == thread_ptr)
    {
        _tx_thread_current_ptr =  TX_NULL;
        _tx_timer_time_slice   =  ((ULONG) 0);
    }

    /* Interrupt context again: terminate/delete must not try to switch on our
       behalf now that we are nobody.  The core lock stays held across it, same
       rule and same reasoning as tx_amiga_discard_thread().  */
    _tx_thread_system_state++;

    (VOID) _tx_thread_terminate(thread_ptr);
    (VOID) _tx_thread_delete(thread_ptr);

    _tx_thread_system_state--;

    /* Inside the lock, and before the bit is freed below: the record is what
       stops a second orphan of the same TX_THREAD from freeing it again.  */
    thread_ptr -> tx_thread_amiga_signal_owner =  (VOID *) 0;
    thread_ptr -> tx_thread_amiga_run_signal   =  0UL;

    wake =  (_tx_amiga_dispatch_inline() == ((UINT) TX_FALSE)) &&
            (_tx_thread_execute_ptr != TX_NULL)
            ? ((UINT) TX_TRUE) : ((UINT) TX_FALSE);
    Permit();

    /* Whoever is next may run now, if there is anybody.  Handed on directly
       when the baton was free; the poke is the fallback.  See the note in
       tx_amiga_adopt_suspend() for why an empty execute pointer means it can
       be skipped altogether, and what makes that safe.  */
    if (wake == (UINT) TX_TRUE)
    {
        _tx_amiga_wake_scheduler();
    }

    /* Drop anything that latched on the run signal, then free the bit.  */
    SetSignal(0UL, sigmask);
    sig =  _tx_amiga_sigbit(sigmask);
    if (sig >= 0)
    {
        FreeSignal(sig);
    }

    return(TX_SUCCESS);
}


/**************************************************************************/
/*                                                                        */
/*    tx_amiga_stack_in_use                             AmigaOS/m68k       */
/*                                                                        */
/*    Whether a block of memory falls inside the stack of a thread that is */
/*    still on ThreadX's created list.  _txe_thread_create() refuses such  */
/*    a block with TX_PTR_ERROR, so a caller that allocates a stack can    */
/*    ask first and take another block instead of failing.                 */
/*                                                                        */
/*    It is not a hypothetical.  An adopted thread's stack is its Exec     */
/*    Task's own, and a Task that exits without orphaning leaves the       */
/*    TX_THREAD registered (see tx_amiga_adopt_resume() above): the memory */
/*    goes back to the system and the allocator hands it out again, while  */
/*    ThreadX still reads it as somebody's stack.  bsdsocket.library is    */
/*    kept open on purpose by the command that starts the network, so its  */
/*    cached adoption outlives the command every time.                     */
/*                                                                        */
/*    The comparison is _txe_thread_create()'s, so an answer of TX_FALSE   */
/*    is the same answer that call will give.                              */
/*                                                                        */
/**************************************************************************/

UINT tx_amiga_stack_in_use(const VOID *start, ULONG size)
{

UBYTE       *stack_start;
UBYTE       *stack_end;
TX_THREAD   *thread;
ULONG        remaining;
UINT         result =  (UINT) TX_FALSE;


    if ((start == TX_NULL) || (size == ((ULONG) 0)))
    {
        return(result);
    }

    stack_start =  (UBYTE *) start;
    stack_end   =  stack_start + size - ((ULONG) 1);

    Forbid();

    thread    =  _tx_thread_created_ptr;
    remaining =  _tx_thread_created_count;

    while ((thread != TX_NULL) && (remaining != ((ULONG) 0)))
    {
        UBYTE   *other_start =  (UBYTE *) thread -> tx_thread_stack_start;
        UBYTE   *other_end   =  (UBYTE *) thread -> tx_thread_stack_end;

        if (((stack_start >= other_start) && (stack_start < other_end)) ||
            ((stack_end   >= other_start) && (stack_end   < other_end)))
        {
            result =  (UINT) TX_TRUE;
            break;
        }

        thread =  thread -> tx_thread_created_next;
        remaining--;
    }

    Permit();

    return(result);
}
