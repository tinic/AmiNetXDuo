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


UINT tx_amiga_adopt_thread(TX_THREAD **thread_ptr, ULONG *generation,
                           CHAR *name, UINT priority, UINT reserved)
{

struct _tx_amiga_adopt_slot *slot;
TX_THREAD                   *thread;
struct Task                 *me;
BYTE                         sig;
ULONG                        sigmask;
UINT                         status;
ULONG                        stack_size;
VOID                        *stack_start;


    if ((thread_ptr == (TX_THREAD **) 0) || (generation == (ULONG *) 0))
    {
        return(TX_PTR_ERROR);
    }

    *thread_ptr =  TX_NULL;
    *generation =  0UL;

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

    /* The TX_THREAD is the port's, never the caller's: tx_amiga_pool.c says
       why.  This waits when every slot is taken, and it waits holding nothing.  */
    slot =  _tx_amiga_slot_claim_or_park(me, reserved);
    if (slot == (struct _tx_amiga_adopt_slot *) 0)
    {
        FreeSignal(sig);

        /* The wait ends without a slot for one of two reasons: the kernel went
           away under us, or more Tasks were already waiting than the port
           tracks.  */
        return((_tx_amiga_kernel_up == TX_FALSE) ? TX_NOT_DONE : TX_NO_MEMORY);
    }
    thread =  &slot -> as_thread;

    /* Describe the Task's real stack to ThreadX.  Nothing writes to it:
       TX_DISABLE_STACK_FILLING is set and stack checking is unavailable.  */
    stack_start =  (VOID *) me -> tc_SPLower;
    stack_size  =  (ULONG) (((UBYTE *) me -> tc_SPUpper) - ((UBYTE *) me -> tc_SPLower));

    Forbid();

    /* RECHECKED HERE, not only at the claim.  The claim and this create are
       different Forbid()s, and tx_amiga_kernel_stop() commits in between by
       clearing _tx_amiga_kernel_up: a create past that point would add a thread
       to a kernel that has already counted them and started its teardown.  */
    if ((_tx_amiga_kernel_up == TX_FALSE) || (_tx_amiga_kernel_stopping != TX_FALSE))
    {
        _tx_amiga_slot_release_locked(slot);
        Permit();
        FreeSignal(sig);
        return(TX_NOT_DONE);
    }

    /* Interrupt context for the duration of create + auto-start resume.  */
    _tx_thread_system_state++;

    _tx_amiga_adopt_task   =  (VOID *) me;
    _tx_amiga_adopt_signal =  sigmask;

    status =  _tx_thread_create(thread, name, _tx_amiga_adopted_entry, 0UL,
                                stack_start, stack_size,
                                priority, priority,
                                TX_NO_TIME_SLICE, TX_AUTO_START);

    _tx_amiga_adopt_task   =  (VOID *) 0;
    _tx_amiga_adopt_signal =  0UL;

    _tx_thread_system_state--;

    if (status != TX_SUCCESS)
    {
        _tx_amiga_slot_release_locked(slot);
        Permit();
        FreeSignal(sig);
        return(status);
    }

    *thread_ptr =  thread;
    *generation =  slot -> as_generation;

    /* Published inside the create's own Forbid: from this instant the slot is
       accounted for by the TX_THREAD in it, and the claim record that the
       unpublished sweep looks at goes away.  */
    _tx_amiga_slot_publish_locked(slot);

    /* Fast path: the baton is free and we are the chosen thread, so take it here
       instead of round-tripping through the scheduler task.  */
    if ((_tx_thread_current_ptr == TX_NULL) &&
        (_tx_thread_execute_ptr == thread) &&
        (_tx_thread_system_state == ((ULONG) 0)))
    {

        ami_baton_note(thread);
        thread -> tx_thread_run_count++;
        _tx_timer_time_slice =  thread -> tx_thread_time_slice;
        ami_budget_hold_start();
        Permit();
        return(TX_SUCCESS);
    }

    Permit();

    /* Slow path: somebody else holds the baton or outranks us.  Wait for it.  */
    _tx_amiga_wake_scheduler();

    /* REQUIRED.  Park answers TX_FALSE when the Task was marked to die while it
       waited: it sets TX_AMIGA_THREAD_ORPHANED and hands control back so the
       caller can unwind.  Returning TX_SUCCESS after that tells the application
       it is now a ThreadX thread while the flags say it is not, and every caller
       goes on to use the TX_THREAD.

       Unreachable today -- the only writer of TX_AMIGA_THREAD_DIE is
       _tx_amiga_reap(), which returns before setting it when the Task has no
       control block, and an adopted Task never has one -- so this costs a
       compare against a reaper that grows a second path.  The teardown is that
       reaper's, not ours: it owns the TX_THREAD and the handshake signal.  */
    if (_tx_amiga_thread_park(thread) == ((UINT) TX_FALSE))
    {
        Forbid();
        _tx_amiga_slot_release_locked(slot);
        Permit();
        *thread_ptr =  TX_NULL;
        *generation =  0UL;
        return(TX_NOT_DONE);
    }

    return(TX_SUCCESS);
}


/* Release the baton and go dormant, keeping the TX_THREAD.  The baton is dropped
   first, so the suspend has nothing to switch away from, and system_state is raised
   so nothing switches on behalf of a Task that is no longer a thread.  */
UINT tx_amiga_adopt_suspend(TX_THREAD *thread_ptr, ULONG generation)
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

    /* A handle whose slot has been recycled names somebody else's adoption.  */
    if (_tx_amiga_slot_held(thread_ptr, generation) == (struct _tx_amiga_adopt_slot *) 0)
    {
        Permit();
        return(TX_THREAD_ERROR);
    }

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
        ami_baton_note(TX_NULL);
        _tx_timer_time_slice   =  ((ULONG) 0);
    }

    _tx_thread_system_state++;

    /* The core lock stays held across the suspend: _tx_thread_system_state is one
       global that every Task reads, so a window with it raised and task switching
       enabled makes other Tasks look like ISRs and fails their socket calls.  */
    AMI_NX_ONLY_SUCCESS(_tx_thread_suspend(thread_ptr));

    _tx_thread_system_state--;

    /* Outside the stack now, holding nothing: a full pool may take this slot
       (_tx_amiga_adopt_evict_dormant_locked) and the next resume re-adopts.  */
    thread_ptr -> tx_thread_amiga_flags |=  TX_AMIGA_THREAD_DORMANT;

    /* A Task parked on a full pool is woken only by a release, and this is
       not one: tell it there is now something to take back.  */
    if (_tx_amiga_adopt_waiting != 0UL)
    {
        _tx_amiga_adopt_wake_waiters_locked();
    }

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
UINT tx_amiga_adopt_resume(TX_THREAD *thread_ptr, ULONG generation)
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

    if (_tx_amiga_slot_held(thread_ptr, generation) == (struct _tx_amiga_adopt_slot *) 0)
    {
        Permit();
        return(TX_THREAD_ERROR);
    }

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

    /* The Task is alive and ours here.  Its stack bounds may have moved since
       the last entry (StackSwap between calls), and the dead-holder check
       compares against this.  */
    thread_ptr -> tx_thread_amiga_task_stamp =  _tx_amiga_task_stamp(me);

    thread_ptr -> tx_thread_amiga_flags &=  ~TX_AMIGA_THREAD_DORMANT;

    _tx_thread_system_state++;

    /* Held across the resume, for the reason tx_amiga_adopt_suspend() gives.  */
    AMI_NX_ONLY_SUCCESS(_tx_thread_resume(thread_ptr));

    _tx_thread_system_state--;

    /* The same free-baton fast path tx_amiga_adopt_thread() takes.  */
    if ((_tx_thread_current_ptr == TX_NULL) &&
        (_tx_thread_execute_ptr == thread_ptr) &&
        (_tx_thread_system_state == ((ULONG) 0)))
    {

        ami_baton_note(thread_ptr);
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




ULONG tx_amiga_adopt_signal(TX_THREAD *thread_ptr)
{

    return(thread_ptr -> tx_thread_amiga_run_signal);
}


/* Free a run signal whose adoption a foreign teardown took; 0 is a no-op.  */
VOID tx_amiga_adopt_signal_free(ULONG sigmask)
{

BYTE    sig;


    sig =  _tx_amiga_sigbit(sigmask);
    if (sig >= 0)
    {
        SetSignal(0UL, sigmask);
        FreeSignal(sig);
    }
}


/* Times a discarded holder left _tx_thread_preempt_disable raised.  */
ULONG _tx_amiga_discard_preempt_resets;


UINT tx_amiga_discard_thread(TX_THREAD *thread_ptr, ULONG generation)
{

struct _tx_amiga_adopt_slot *slot;
struct Task                 *me;
ULONG                        sigmask;
BYTE                         sig;


    if (thread_ptr == TX_NULL)
    {
        return(TX_PTR_ERROR);
    }

    me =  FindTask((STRPTR) 0);

    Forbid();

    slot =  _tx_amiga_slot_held(thread_ptr, generation);
    if (slot == (struct _tx_amiga_adopt_slot *) 0)
    {
        /* Stale: the slot was recycled after this handle was taken, so the
           TX_THREAD in it is somebody else's and there is nothing to do.  */
        Permit();
        return(TX_THREAD_ERROR);
    }

    if ((thread_ptr -> tx_thread_id != TX_THREAD_ID) ||
        ((thread_ptr -> tx_thread_amiga_flags & TX_AMIGA_THREAD_ADOPTED) == 0U))
    {
        /* Already torn down, by the reaper or by an earlier discard.  The slot
           is still ours by generation, so give it back rather than leak it.  */
        _tx_amiga_slot_release_locked(slot);
        Permit();
        return(TX_SUCCESS);
    }

    /* The holder itself: ami_netstack_baton_reclaim_dead() discards a Task that
       was removed while it held the baton.  Take the baton back here, or a stale
       holder stops the whole stack.  A Task removed mid-service can also leave
       the core's preemption lockout raised; nobody else is inside the core when
       a foreign caller finds the holder dead, so the count is that Task's.  */
    if (_tx_thread_current_ptr == thread_ptr)
    {
        ami_budget_hold_end((APTR) thread_ptr, thread_ptr -> tx_thread_name,
                            (ULONG) thread_ptr -> tx_thread_state,
                            AMI_HOLD_SITE_DISCARD);
        ami_baton_note(TX_NULL);
        _tx_timer_time_slice   =  ((ULONG) 0);

        if ((_tx_thread_preempt_disable != ((UINT) 0)) &&
            (thread_ptr -> tx_thread_amiga_task != (VOID *) me))
        {
            _tx_thread_preempt_disable =  ((UINT) 0);
            _tx_amiga_discard_preempt_resets++;
        }
    }

    _tx_thread_system_state++;

    /* The core lock stays held, for the reason tx_amiga_adopt_suspend() gives.
       _tx_amiga_reap() is the one thing under delete that Wait()s, and it returns
       at its first test for an adopted thread, so nothing here blocks.  */
    AMI_NX_ONLY_SUCCESS(_tx_thread_terminate(thread_ptr));
    AMI_NX_CLEANUP(_tx_thread_delete(thread_ptr));

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

    /* AFTER the teardown, never before: until the terminate and the delete have
       run, this TX_THREAD is still on ThreadX's lists and a fresh adopter given
       the slot would create over it.  */
    _tx_amiga_slot_release_locked(slot);

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


UINT tx_amiga_orphan_thread(TX_THREAD *thread_ptr, ULONG generation)
{

struct _tx_amiga_adopt_slot *slot;
struct Task                 *me;
ULONG                        sigmask;
BYTE                         sig;
UINT                         wake;


    if (thread_ptr == TX_NULL)
    {
        return(TX_PTR_ERROR);
    }

    me =  FindTask((STRPTR) 0);

    Forbid();

    slot =  _tx_amiga_slot_held(thread_ptr, generation);
    if (slot == (struct _tx_amiga_adopt_slot *) 0)
    {
        Permit();
        return(TX_THREAD_ERROR);
    }

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
        _tx_amiga_slot_release_locked(slot);
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
        ami_baton_note(TX_NULL);
        _tx_timer_time_slice   =  ((ULONG) 0);
    }

    /* Interrupt context again: terminate/delete must not try to switch on our
       behalf now that we are nobody.  Core lock held across it, as in discard.  */
    _tx_thread_system_state++;

    AMI_NX_ONLY_SUCCESS(_tx_thread_terminate(thread_ptr));
    AMI_NX_CLEANUP(_tx_thread_delete(thread_ptr));

    _tx_thread_system_state--;

    /* Inside the lock, and before the bit is freed below: the record is what
       stops a second orphan of the same TX_THREAD from freeing it again.  */
    thread_ptr -> tx_thread_amiga_signal_owner =  (VOID *) 0;
    thread_ptr -> tx_thread_amiga_run_signal   =  0UL;

    /* AFTER the teardown; see tx_amiga_discard_thread().  */
    _tx_amiga_slot_release_locked(slot);

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


/* The port has no per-thread termination side table. */
VOID _tx_amiga_thread_terminated(TX_THREAD *thread_ptr)
{

    (VOID) thread_ptr;
}
