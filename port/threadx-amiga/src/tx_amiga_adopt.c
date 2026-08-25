/***************************************************************************
 * AmiNetXDuo, ThreadX thread adoption for pre-existing Exec Tasks.
 *
 * SPDX-License-Identifier: MIT
 **************************************************************************/

/* tx_amiga_adopt_thread / tx_amiga_orphan_thread.  NetX Duo suspends the calling
   thread, so an application Exec Task becomes a TX_THREAD here.  While adopted it
   must not block outside ThreadX: the whole stack stalls behind the held baton. */

#define TX_SOURCE_CODE

#include "tx_amiga_internal.h"


/* Entry function recorded in the TX_THREAD of an adopted thread.  Never invoked:
   an adopted Task enters ThreadX through tx_amiga_adopt_thread(), not through
   _tx_thread_shell_entry().  */
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


/* TX_TRUE if the calling Task is the ThreadX baton holder.  The generic answer,
   _tx_thread_system_state == 0, is wrong here: interrupt context is a Task
   holding the core lock, so that counter is whatever some other Task is doing.  */
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

    /* Fast path: the baton is free and we are the chosen thread, so take it here
       instead of round-tripping through the scheduler task.  */
    if ((_tx_thread_current_ptr == TX_NULL) &&
        (_tx_thread_execute_ptr == thread_ptr) &&
        (_tx_thread_system_state == ((ULONG) 0)))
    {

        _tx_thread_current_ptr =  thread_ptr;
        thread_ptr -> tx_thread_run_count++;
        _tx_timer_time_slice =  thread_ptr -> tx_thread_time_slice;
        ami_budget_hold_start();
        Permit();
        return(TX_SUCCESS);
    }

    Permit();

    /* Slow path: somebody else holds the baton or outranks us.  Wait for it.  */
    _tx_amiga_wake_scheduler();
    (VOID) _tx_amiga_thread_park(thread_ptr);

    return(TX_SUCCESS);
}


/* Release the baton and go dormant, keeping the TX_THREAD.  The baton is dropped
   first, so the suspend has nothing to switch away from, and system_state is raised
   so nothing switches on behalf of a Task that is no longer a thread.  */
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
        ami_budget_hold_end((APTR) thread_ptr, thread_ptr -> tx_thread_name,
                            (ULONG) thread_ptr -> tx_thread_state,
                            AMI_HOLD_SITE_SUSPEND);
        _tx_thread_current_ptr =  TX_NULL;
        _tx_timer_time_slice   =  ((ULONG) 0);
    }

    _tx_thread_system_state++;

    /* The core lock stays held across the suspend: _tx_thread_system_state is one
       global that every Task reads, so a window with it raised and task switching
       enabled makes other Tasks look like ISRs and fails their socket calls.  */
    (VOID) _tx_thread_suspend(thread_ptr);

    _tx_thread_system_state--;

    /* Wake the scheduler only if there is something to dispatch: an empty execute
       pointer means the poke would wake it to find nothing, and no dispatch is lost
       -- whatever makes a thread ready next wakes it.  Read under the core lock. */
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


/* Come back out of dormancy and acquire the baton.  Same fast-path-or-park tail
   as tx_amiga_adopt_thread(); only the registration is skipped.  */
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
        ami_budget_hold_start();
        Permit();
        return(TX_SUCCESS);
    }

    Permit();

    _tx_amiga_wake_scheduler();

    if (_tx_amiga_thread_park(thread_ptr) != TX_TRUE)
    {

        /* Torn down under us while we waited.  Tell the caller to start over with
           a fresh adoption rather than pretend it holds the baton.  */
        return(TX_CALLER_ERROR);
    }

    return(TX_SUCCESS);
}


#ifdef AMINETXDUO_GREEN_REALM

/* The free-baton fast path of the request gate: resume the cached thread ONLY if
   that takes the baton immediately, and otherwise back the resume out entirely,
   both under one Forbid(), so a decline posts no Signal and leaves no trace.  */
UINT tx_amiga_adopt_try_resume(TX_THREAD *thread_ptr)
{

struct Task *me;
UINT         taken;


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

    /* A cheap refusal before touching the lists: somebody holds the baton, or
       something already ready outranks us.  */
    if ((_tx_thread_current_ptr != TX_NULL) ||
        (_tx_thread_execute_ptr != TX_NULL) ||
        (_tx_thread_system_state != ((ULONG) 0)))
    {
        Permit();
        return(TX_NOT_DONE);
    }

    _tx_thread_system_state++;

    (VOID) _tx_thread_resume(thread_ptr);

    /* The adopt fast-path condition, minus the system_state term (ours is
       the only raise, and it comes back down either way).  */
    taken =  ((_tx_thread_current_ptr == TX_NULL) &&
              (_tx_thread_execute_ptr == thread_ptr))
             ? ((UINT) TX_TRUE) : ((UINT) TX_FALSE);

    if (taken == ((UINT) TX_FALSE))
    {

        /* The resume surfaced somebody who outranks us.  Put the thread back;
           execute_ptr is recomputed by the suspend under this same Forbid().  */
        (VOID) _tx_thread_suspend(thread_ptr);
        _tx_thread_system_state--;
        Permit();
        return(TX_NOT_DONE);
    }

    _tx_thread_system_state--;

    _tx_thread_current_ptr =  thread_ptr;
    thread_ptr -> tx_thread_run_count++;
    _tx_timer_time_slice =  thread_ptr -> tx_thread_time_slice;
    ami_budget_hold_start();

    Permit();

    return(TX_SUCCESS);
}


/* Whether the baton is immediately takeable, for the policy decision before a
   first-ever adoption.  A hint, not a lock: the answer can be stale, and both
   outcomes remain correct.  Only the try-resume above is the atomic form.  */
UINT tx_amiga_baton_free(VOID)
{

UINT    answer;


    Forbid();
    answer =  ((_tx_thread_current_ptr == TX_NULL) &&
               (_tx_thread_execute_ptr == TX_NULL) &&
               (_tx_thread_system_state == ((ULONG) 0)))
              ? ((UINT) TX_TRUE) : ((UINT) TX_FALSE);
    Permit();

    return(answer);
}

#endif /* AMINETXDUO_GREEN_REALM */


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

    /* Somebody else's Task must not be left as the baton holder.  Should be
       unreachable, but a stale baton stops the whole stack.  */
    if (_tx_thread_current_ptr == thread_ptr)
    {
        ami_budget_hold_end((APTR) thread_ptr, thread_ptr -> tx_thread_name,
                            (ULONG) thread_ptr -> tx_thread_state,
                            AMI_HOLD_SITE_DISCARD);
        _tx_thread_current_ptr =  TX_NULL;
        _tx_timer_time_slice   =  ((ULONG) 0);
    }

    _tx_thread_system_state++;

    /* The core lock stays held, for the reason tx_amiga_adopt_suspend() gives.
       _tx_amiga_reap() is the one thing under delete that Wait()s, and it returns
       at its first test for an adopted thread, so nothing here blocks.  */
    (VOID) _tx_thread_terminate(thread_ptr);
    (VOID) _tx_thread_delete(thread_ptr);

    _tx_thread_system_state--;

    /* Only the Task that allocated a signal bit may FreeSignal() it, so a foreign
       caller can only drop the registration and leave the bit to die with its
       owner.  When the owner is calling, leaving it loses one of its 32.  */
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

    /* Tested BEFORE the tx_thread_amiga_task check, and against signal_owner:
       _tx_amiga_reap() zeroes tx_thread_amiga_task under delete, so an ownership
       test here could never reach this branch and would leak one of the 32 bits. */
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
        ami_budget_hold_end((APTR) thread_ptr, thread_ptr -> tx_thread_name,
                            (ULONG) thread_ptr -> tx_thread_state,
                            AMI_HOLD_SITE_ORPHAN);
        _tx_thread_current_ptr =  TX_NULL;
        _tx_timer_time_slice   =  ((ULONG) 0);
    }

    /* Interrupt context again: terminate/delete must not try to switch on our
       behalf now that we are nobody.  Core lock held across it, as in discard.  */
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

    /* Handed on directly when the baton was free; the poke is the fallback.  See
       tx_amiga_adopt_suspend() for why an empty execute pointer skips it.  */
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


/* Whether a block of memory falls inside the stack of a thread still on ThreadX's
   created list, which _txe_thread_create() refuses with TX_PTR_ERROR.  The
   comparison is that call's, so TX_FALSE is the answer it will give.  */

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

        if ((stack_start <= other_end) && (stack_end >= other_start))
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
