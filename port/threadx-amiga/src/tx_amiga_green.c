/***************************************************************************
 * AmiNetXDuo, the green realm: ThreadX threads as coroutines inside one
 * Exec Task.
 *
 * SPDX-License-Identifier: MIT
 **************************************************************************/

/* The green realm: threads the stack creates get no Exec Task of their own and run
   as coroutines inside the realm Task.  A green thread MUST NOT block in Exec --
   its Wait() sleeps the whole realm -- and calls tx_amiga_green_wait() instead.  */

#define TX_SOURCE_CODE

#include "tx_amiga_internal.h"

#ifdef AMINETXDUO_RXPROBE
#include "aminetxduo/compat.h"      /* AMI_WARN for the probe tripwires */
#endif

#ifdef AMINETXDUO_GREEN_REALM


/* The realm scheduler's saved context (loaded by a yielding green thread,
   stored by the scheduler as it dispatches one).  */
APTR    _tx_green_scheduler_sp =  (APTR) 0;

struct _tx_green_counters   _tx_green_counters;


/* ------------------------------------------------------ signal waiters --- */

/* One slot per green thread currently inside tx_amiga_green_wait().  All fields
   are touched only under Forbid().  */

#define TX_GREEN_WAIT_SLOTS     16

struct _tx_green_waiter
{
    TX_THREAD   *gw_thread;             /* NULL = free                       */
    ULONG        gw_mask;               /* what it is waiting for            */
    ULONG        gw_received;           /* what the scheduler delivered      */
};

static struct _tx_green_waiter  _tx_green_waiter[TX_GREEN_WAIT_SLOTS];


/* The union of every registered waiter's mask.  Call under Forbid().  */
ULONG _tx_green_pending_union(VOID)
{

ULONG   mask;
UINT    i;


    mask =  0UL;
    for (i = 0U; i < (UINT) TX_GREEN_WAIT_SLOTS; i++)
    {
        if (_tx_green_waiter[i].gw_thread != TX_NULL)
        {
            mask |=  _tx_green_waiter[i].gw_mask;
        }
    }
    return(mask);
}


/* Hand latched Exec signals to the green threads waiting on them, resuming each
   one so the dispatch pass that follows can pick it by priority.  Call under
   Forbid(); the raised system_state defers every context switch to the caller. */
VOID _tx_green_deliver(ULONG sigs)
{

UINT    i;


    if (sigs == 0UL)
    {
        return;
    }

    for (i = 0U; i < (UINT) TX_GREEN_WAIT_SLOTS; i++)
    {

        struct _tx_green_waiter *gw =  &_tx_green_waiter[i];

        if ((gw -> gw_thread != TX_NULL) && ((gw -> gw_mask & sigs) != 0UL))
        {

            /* A slot whose thread was terminated or completed can never collect:
               purge it, or its mask stays in the union and the realm consumes
               those bits blind -- and, once recycled, from their new owner.  */
            if ((gw -> gw_thread -> tx_thread_state == ((UINT) TX_TERMINATED)) ||
                (gw -> gw_thread -> tx_thread_state == ((UINT) TX_COMPLETED)))
            {
                gw -> gw_thread   =  TX_NULL;
                gw -> gw_mask     =  0UL;
                gw -> gw_received =  0UL;
                continue;
            }

            gw -> gw_received |=  (gw -> gw_mask & sigs);

            /* Resume once: a slot whose thread is already resumed but has not
               yet collected keeps accumulating without a second resume, which
               _tx_thread_resume() would refuse anyway.  */
            if (gw -> gw_thread -> tx_thread_state == ((UINT) TX_SUSPENDED))
            {
                _tx_thread_system_state++;
                (VOID) _tx_thread_resume(gw -> gw_thread);
                _tx_thread_system_state--;
            }
        }
    }
}


/* TX_THREAD_TERMINATED_EXTENSION: a thread terminated while registered in a green
   wait leaves its slot, and its mask's claim on the realm's Wait(), unless it is
   purged here.  The core lock is held, and it IS the Forbid() the purge wants.  */
VOID _tx_amiga_thread_terminated(TX_THREAD *thread_ptr)
{

    _tx_green_forget(thread_ptr);
}


/* Drop any waiter record a deleted thread left behind.  Call under Forbid().  */
VOID _tx_green_forget(TX_THREAD *thread_ptr)
{

UINT    i;


    for (i = 0U; i < (UINT) TX_GREEN_WAIT_SLOTS; i++)
    {
        if (_tx_green_waiter[i].gw_thread == thread_ptr)
        {
            _tx_green_waiter[i].gw_thread   =  TX_NULL;
            _tx_green_waiter[i].gw_mask     =  0UL;
            _tx_green_waiter[i].gw_received =  0UL;
        }
    }
}


/* ---------------------------------------------------------- green wait --- */

/* TX_TRUE while the calling code runs in a green context: on the realm Task, with
   a green thread holding the baton.  */
UINT tx_amiga_green_active(VOID)
{

TX_THREAD   *current;
UINT         answer;


    if (_tx_amiga_scheduler_task == (VOID *) 0)
    {
        return((UINT) TX_FALSE);
    }
    if ((VOID *) FindTask((STRPTR) 0) != _tx_amiga_scheduler_task)
    {
        return((UINT) TX_FALSE);
    }

    Forbid();
    current =  _tx_thread_current_ptr;
    answer  =  ((current != TX_NULL) && (_tx_amiga_thread_green(current) != TX_FALSE))
               ? ((UINT) TX_TRUE) : ((UINT) TX_FALSE);
    Permit();

    return(answer);
}


/* Sleep the calling GREEN thread until one of `sigmask`'s Exec signals is latched
   on the realm Task; returns the bits.  Bits of a thread that is NOT registered are
   never consumed by the scheduler.  Must not be called holding a ThreadX mutex.  */
ULONG tx_amiga_green_wait(ULONG sigmask)
{

TX_THREAD               *thread;
struct _tx_green_waiter *gw;
ULONG                    got;
UINT                     i;


    if (sigmask == 0UL)
    {
        return(0UL);
    }

    for (;;)
    {

    Forbid();

    thread =  _tx_thread_current_ptr;
    if ((thread == TX_NULL) || (_tx_amiga_thread_green(thread) == TX_FALSE) ||
        ((VOID *) FindTask((STRPTR) 0) != _tx_amiga_scheduler_task))
    {

        /* Not a green context.  Honour the request with a plain Wait() --
           this caller owns its Task and may block.  */
        Permit();
        return(Wait(sigmask));
    }

    /* Already latched?  Consume and go.  */
    got =  SetSignal(0UL, sigmask) & sigmask;
    if (got != 0UL)
    {
        _tx_green_counters.gc_wait_fast++;
        Permit();
        return(got);
    }

    /* Register.  */
    gw =  (struct _tx_green_waiter *) 0;
    for (i = 0U; i < (UINT) TX_GREEN_WAIT_SLOTS; i++)
    {
        if (_tx_green_waiter[i].gw_thread == TX_NULL)
        {
            gw =  &_tx_green_waiter[i];
            break;
        }
    }

    if (gw == (struct _tx_green_waiter *) 0)
    {

        /* Table full.  Back off through the scheduler and run the WHOLE test
           again from the top, iteratively, so a sustained full table deepens no
           stack.  */
        Permit();
        (VOID) _tx_thread_sleep(1);
        continue;
    }

#ifdef AMINETXDUO_RXPROBE
    /* Tripwire: two waiters on OVERLAPPING masks would mean one thread's delivery
       consumes bits the other is owed.  Every legitimate waiter owns its bits, so
       masks are disjoint by construction; say so loudly the day one is not.  */
    for (i = 0U; i < (UINT) TX_GREEN_WAIT_SLOTS; i++)
    {
        if ((_tx_green_waiter[i].gw_thread != TX_NULL) &&
            ((_tx_green_waiter[i].gw_mask & sigmask) != 0UL))
        {
            AMI_WARN("green realm: waiter masks overlap (%08lx & %08lx): "
                     "a signal bit has two owners",
                     (unsigned long) _tx_green_waiter[i].gw_mask,
                     (unsigned long) sigmask);
        }
    }
#endif

    gw -> gw_thread   =  thread;
    gw -> gw_mask     =  sigmask;
    gw -> gw_received =  0UL;

    _tx_green_counters.gc_wait_slow++;

    /* Off the ready list, without switching (interrupt-context shape).  */
    _tx_thread_system_state++;
    (VOID) _tx_thread_suspend(thread);
    _tx_thread_system_state--;

    /* Release the baton and give the machine back to the realm loop.  */
    ami_budget_hold_end((APTR) thread, thread -> tx_thread_name,
                        (ULONG) thread -> tx_thread_state,
                        AMI_HOLD_SITE_BRACKET);
    _tx_thread_current_ptr =  TX_NULL;

    _tx_green_switch(&thread -> tx_thread_stack_ptr, _tx_green_scheduler_sp);

    /* Dispatched again: the scheduler delivered our signals, resumed us and
       switched in, holding the protocol Forbid().  A resume that was NOT a
       delivery collects zero -- callers own a loop around their condition.  */
    got =  gw -> gw_received;
    gw -> gw_thread   =  TX_NULL;
    gw -> gw_mask     =  0UL;
    gw -> gw_received =  0UL;

    Permit();

    return(got);

    }                                   /* overflow backoff retries here */
}


/* ------------------------------------------------- relinquish delivery --- */

/* TX_THREAD_RELINQUISH_PORT_PREPARE.  A green thread's wakeups latch on the realm
   Task and are delivered only at scheduler passes, so relinquish must deliver them
   here or it compares against lists frozen at pass entry and no-ops.  */
VOID _tx_amiga_relinquish_prepare(VOID)
{

ULONG   mask;
ULONG   pending;


    if (tx_amiga_green_active() == ((UINT) TX_FALSE))
    {
        return;
    }

    Forbid();

    if (_tx_amiga_tick_run.tr_realm != ((UINT) TX_FALSE))
    {
        _tx_amiga_tick_deliver((UINT) TX_TRUE);
    }

    mask =  _tx_green_pending_union();
    if (mask != 0UL)
    {
        pending =  SetSignal(0UL, mask) & mask;
        if (pending != 0UL)
        {
            _tx_green_deliver(pending);
        }
    }

    Permit();
}


/* ------------------------------------------------------- first activation --- */

/* Where a green thread's initial frame "returns" to.  The dispatcher's protocol
   Forbid() is still held; give it back, then enter the thread.  If shell_entry ever
   returns there is no Exec Task to destroy: park the context for good.  */
VOID _tx_green_thread_begin(VOID)
{

TX_THREAD   *thread;


    Permit();

    _tx_thread_shell_entry();

    for (;;)
    {

        Forbid();
        thread =  _tx_thread_current_ptr;
        if ((thread != TX_NULL) &&
            ((VOID *) FindTask((STRPTR) 0) == _tx_amiga_scheduler_task))
        {
            ami_budget_hold_end((APTR) thread, thread -> tx_thread_name,
                                (ULONG) thread -> tx_thread_state,
                                AMI_HOLD_SITE_YIELD);
            _tx_thread_current_ptr =  TX_NULL;
            _tx_green_switch(&thread -> tx_thread_stack_ptr,
                             _tx_green_scheduler_sp);
        }
        Permit();
    }
}


/* --------------------------------------------------------- initial frame --- */

/* Lay a first-activation frame on a green thread's stack: the 44-byte d2-d7/a2-a6
   image (zeroed) under _tx_green_thread_begin as the return address.  Must mirror
   _tx_green_switch()'s pop exactly; both files say 44.  */
VOID _tx_green_stack_build(TX_THREAD *thread_ptr)
{

ULONG   top;
ULONG  *frame;
UINT    i;


    /* Longword-align the top of the caller-supplied stack window.  */
    top =  ((ULONG) thread_ptr -> tx_thread_stack_end) & ~3UL;

    /* Return address first (highest), the register image below it.  */
    frame        =  (ULONG *) (top - 4UL - 44UL);
    for (i = 0U; i < 11U; i++)
    {
        frame[i] =  0UL;
    }
    frame[11]    =  (ULONG) _tx_green_thread_begin;

    thread_ptr -> tx_thread_stack_ptr =  (VOID *) frame;
}


/* -------------------------------------------------------- stray-Wait net --- */

/* The probe build's net: netstack_internal.h and sana2_internal.h #define Wait() to
   this, so an Exec Wait() reachable from a green thread is counted (gs_stray_wait,
   MUST read zero) and converted.  Here, so the passthrough calls the real Wait(). */
#ifdef AMINETXDUO_RXPROBE

ULONG ami_green_checked_wait(ULONG sigmask)
{

    if (tx_amiga_green_active() != ((UINT) 0))
    {
        tx_amiga_green_stray_wait_note();
        AMI_WARN("green realm: Exec Wait() from green context (converted). "
                 "An unconverted blocking site is still in the realm");
        return(tx_amiga_green_wait(sigmask));
    }

    return(Wait(sigmask));
}

#endif /* AMINETXDUO_RXPROBE */


/* ------------------------------------------------------- the request gate --- */

/* The client boundary: a caller's bracket becomes "submit the continuation, park,
   one Signal back", the continuation being what _tx_green_switch() saves on its
   stack.  Ctrl-C lands on the parked OWNER, which collects it for gate_breaks().  */

#ifndef TX_GATE_SIDE_BYTES
#define TX_GATE_SIDE_BYTES      1024UL
#endif

volatile UINT   _tx_amiga_gate_bind_pending =  0U;


/* Never invoked: a gate proxy is entered only through its captured context,
   not through _tx_thread_shell_entry().  If it ever were, sleeping forever
   in ThreadX keeps the realm alive, unlike an Exec Wait() would.  */
static VOID _tx_gate_proxy_entry(ULONG id)
{

    (VOID) id;
    for (;;)
    {
        (VOID) _tx_thread_sleep(0x7FFFFFFFUL);
    }
}


static BYTE _tx_gate_sigbit(ULONG sigmask)
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


UINT tx_amiga_gate_bind(TX_AMIGA_GATE *gate, CHAR *name, UINT priority)
{

struct Task *me;
BYTE         sig;
UINT         status;


    if (gate == (TX_AMIGA_GATE *) 0)
    {
        return(TX_PTR_ERROR);
    }
    if (priority >= ((UINT) TX_MAX_PRIORITIES))
    {
        return(TX_PRIORITY_ERROR);
    }
    if ((_tx_amiga_kernel_up == TX_FALSE) ||
        (_tx_amiga_kernel_stopping != TX_FALSE))
    {
        return(TX_NOT_DONE);
    }
    if (gate -> ag_Live != 0U)
    {
        return(TX_NOT_DONE);
    }

    me =  FindTask((STRPTR) 0);

    gate -> ag_Side =  (APTR) AllocMem(TX_GATE_SIDE_BYTES, MEMF_PUBLIC);
    if (gate -> ag_Side == (APTR) 0)
    {
        return(TX_NO_MEMORY);
    }

    /* The completion bit is the OWNER's: it Wait()s on it, so it must
       allocate it, the same rule the adoption run signal follows.  */
    sig =  AllocSignal(-1);
    if (sig < 0)
    {
        FreeMem(gate -> ag_Side, TX_GATE_SIDE_BYTES);
        gate -> ag_Side =  (APTR) 0;
        return(TX_NO_MEMORY);
    }

    gate -> ag_DoneMask  =  1UL << ((ULONG) sig);
    gate -> ag_Task      =  me;
    gate -> ag_ResumeSP  =  (APTR) 0;
    gate -> ag_Breaks    =  0UL;
    gate -> ag_BreakMask =  0UL;
    gate -> ag_Done      =  0U;
    gate -> ag_Active    =  0U;
    gate -> ag_OwnerDead =  0U;

    Forbid();

    /* Interrupt context across the create, the adopt handshake's shape.  The
       internal create is deliberate: _txe_thread_create() would refuse the owner's
       stack region as overlapping, and it is shared serially, one bracket at a time. */
    _tx_thread_system_state++;
    _tx_amiga_gate_bind_pending =  1U;

    status =  _tx_thread_create(&gate -> ag_Thread, name, _tx_gate_proxy_entry,
                                0UL,
                                (VOID *) me -> tc_SPLower,
                                (ULONG) (((UBYTE *) me -> tc_SPUpper) -
                                         ((UBYTE *) me -> tc_SPLower)),
                                priority, priority,
                                TX_NO_TIME_SLICE, TX_DONT_START);

    _tx_amiga_gate_bind_pending =  0U;
    _tx_thread_system_state--;

    Permit();

    if (status != TX_SUCCESS)
    {
        FreeMem(gate -> ag_Side, TX_GATE_SIDE_BYTES);
        gate -> ag_Side     =  (APTR) 0;
        gate -> ag_DoneMask =  0UL;
        FreeSignal(sig);
        return(status);
    }

    gate -> ag_Live =  1U;

    return(TX_SUCCESS);
}


UINT tx_amiga_gate_call(TX_AMIGA_GATE *gate, ULONG break_mask)
{

ULONG   *frame;
ULONG    top;
UINT     i;


    if ((gate == (TX_AMIGA_GATE *) 0) || (gate -> ag_Live == 0U))
    {
        return(TX_NOT_DONE);
    }
    if ((VOID *) FindTask((STRPTR) 0) != (VOID *) gate -> ag_Task)
    {
        return(TX_CALLER_ERROR);
    }
    if ((_tx_amiga_kernel_up == TX_FALSE) ||
        (_tx_amiga_kernel_stopping != TX_FALSE) ||
        (gate -> ag_OwnerDead != 0U) ||
        (gate -> ag_Active != 0U) ||
        (gate -> ag_Thread.tx_thread_state != ((UINT) TX_SUSPENDED)))
    {
        return(TX_NOT_DONE);
    }

    /* The side frame is rebuilt per call (a switch consumes it): eleven
       registers with the gate in the a2 slot, __tx_gate_park_entry above
       them.  Same 44-byte shape _tx_green_stack_build() lays.  */
    top   =  (((ULONG) gate -> ag_Side) + TX_GATE_SIDE_BYTES) & ~3UL;
    frame =  (ULONG *) (top - 4UL - 44UL);
    for (i = 0U; i < 11U; i++)
    {
        frame[i] =  0UL;
    }
    frame[6]  =  (ULONG) gate;                  /* a2 after the movem pop     */
    frame[11] =  (ULONG) _tx_gate_park_entry;

    gate -> ag_BreakMask =  break_mask;
    gate -> ag_Breaks    =  0UL;
    gate -> ag_Done      =  0U;
    gate -> ag_Active    =  1U;

    /* Drop a completion bit a previous call may have left latched.  */
    (VOID) SetSignal(0UL, gate -> ag_DoneMask);

    Forbid();

    _tx_green_counters.gc_gate_calls++;

    /* The capture: everything from here to gate_return becomes the proxy's context.
       It continues first in the parker, on the side stack, with this Forbid() held;
       then the realm dispatches the proxy and execution RESUMES RIGHT HERE.  */
    _tx_green_switch(&gate -> ag_Thread.tx_thread_stack_ptr,
                     (APTR) frame);

    Permit();

    return(TX_SUCCESS);
}


/* The side-stack half of the owner: resume the proxy, wake the realm, sleep in the
   ONE boundary Wait() collecting break bits, then jump back into the leave-side
   context gate_return left.  Entered with the capture Forbid() held; no return. */
VOID _tx_gate_park(TX_AMIGA_GATE *gate)
{

APTR    junk;
ULONG   sigs;


    /* The proxy's context is complete (the capture switch wrote it), so it
       may run the moment the realm picks it.  Interrupt-context shape, as
       everywhere a non-thread touches ThreadX state.  */
    _tx_thread_system_state++;
    (VOID) _tx_thread_resume(&gate -> ag_Thread);
    _tx_thread_system_state--;

    Permit();

    /* Only the realm can enter a green context; always poke it.  The signal
       latches, so a realm already awake pays one compare.  */
    _tx_amiga_wake_scheduler();

    for (;;)
    {

        sigs =  Wait(gate -> ag_DoneMask | gate -> ag_BreakMask);

        if ((sigs & gate -> ag_BreakMask) != 0UL)
        {
            gate -> ag_Breaks |=  sigs & gate -> ag_BreakMask;
        }
        if (gate -> ag_Done != 0U)
        {
            break;
        }
    }

    /* Adopt the leave-side context.  The resumed side (the tail of
       tx_amiga_gate_return, running on this Task from here on) Permit()s.  */
    Forbid();
    _tx_green_switch(&junk, gate -> ag_ResumeSP);

    /* Unreachable: the context above never switches back.  */
}


VOID tx_amiga_gate_return(TX_AMIGA_GATE *gate)
{

TX_THREAD   *thread;
UINT         dead;


    thread =  &gate -> ag_Thread;

    Forbid();

    /* Release the baton first, so the suspend has nothing to switch away
       from -- tx_amiga_adopt_suspend()'s order.  */
    ami_budget_hold_end((APTR) thread, thread -> tx_thread_name,
                        (ULONG) thread -> tx_thread_state,
                        AMI_HOLD_SITE_SUSPEND);
    _tx_thread_current_ptr =  TX_NULL;
    _tx_timer_time_slice   =  ((ULONG) 0);

    _tx_thread_system_state++;
    (VOID) _tx_thread_suspend(thread);
    _tx_thread_system_state--;

    gate -> ag_Done =  1U;
    dead            =  gate -> ag_OwnerDead;

    if (dead == 0U)
    {
        Signal((struct Task *) gate -> ag_Task, gate -> ag_DoneMask);
    }
    else
    {

        /* Nobody comes back for the leave-side context.  Mark the flight
           over so the heartbeat's next pass may reap the dormant proxy.  */
        gate -> ag_Active =  0U;
    }

    /* Store the leave-side context and give the machine to the realm loop.  The
       parked owner resumes it, so execution continues after this call on the
       OWNER Task.  */
    _tx_green_switch(&gate -> ag_ResumeSP, _tx_green_scheduler_sp);

    /* === On the owning Task again, under the parker's Forbid(). === */
    Permit();

    gate -> ag_Active =  0U;

    /* The parker's Wait() consumed any break bits; the contract (EINTR
       leaves the signal set, transfer.c) wants them still pending.  */
    if (gate -> ag_Breaks != 0UL)
    {
        (VOID) SetSignal(gate -> ag_Breaks, gate -> ag_Breaks);
    }
}


ULONG tx_amiga_gate_breaks(const TX_AMIGA_GATE *gate)
{

    if (gate == (const TX_AMIGA_GATE *) 0)
    {
        return(0UL);
    }
    return(gate -> ag_Breaks);
}


VOID tx_amiga_gate_release(TX_AMIGA_GATE *gate)
{

BYTE    sig;


    if (gate == (TX_AMIGA_GATE *) 0)
    {
        return;
    }

    if (gate -> ag_Live != 0U)
    {

        Forbid();
        _tx_thread_system_state++;
        (VOID) _tx_thread_terminate(&gate -> ag_Thread);
        (VOID) _tx_thread_delete(&gate -> ag_Thread);
        _tx_thread_system_state--;
        Permit();

        gate -> ag_Live =  0U;
    }

    if (gate -> ag_Side != (APTR) 0)
    {
        FreeMem(gate -> ag_Side, TX_GATE_SIDE_BYTES);
        gate -> ag_Side =  (APTR) 0;
    }

    /* The bit belongs to the owner's Task; only it can give it back.  A
       foreign releaser leaves the bit to die with its Task, the same rule
       tx_amiga_discard_thread() states.  */
    if ((gate -> ag_DoneMask != 0UL) &&
        ((VOID *) FindTask((STRPTR) 0) == (VOID *) gate -> ag_Task))
    {
        (VOID) SetSignal(0UL, gate -> ag_DoneMask);
        sig =  _tx_gate_sigbit(gate -> ag_DoneMask);
        if (sig >= 0)
        {
            FreeSignal(sig);
        }
    }
    gate -> ag_DoneMask =  0UL;
    gate -> ag_Task     =  (struct Task *) 0;
}


UINT tx_amiga_gate_orphan(TX_AMIGA_GATE *gate)
{

UINT    reaped;


    if ((gate == (TX_AMIGA_GATE *) 0) || (gate -> ag_Live == 0U))
    {
        return((UINT) TX_TRUE);
    }

    reaped =  (UINT) TX_FALSE;

    Forbid();

    gate -> ag_OwnerDead =  1U;

    /* Safe to reap unless the realm is INSIDE the proxy's context this
       instant.  Suspended-mid-flight is safe: terminate runs the ordinary
       suspension cleanup and the realm never re-enters the context.  */
    if (_tx_thread_current_ptr != &gate -> ag_Thread)
    {

        _tx_thread_system_state++;
        (VOID) _tx_thread_terminate(&gate -> ag_Thread);
        (VOID) _tx_thread_delete(&gate -> ag_Thread);
        _tx_thread_system_state--;

        gate -> ag_Live =  0U;

        if (gate -> ag_Side != (APTR) 0)
        {
            FreeMem(gate -> ag_Side, TX_GATE_SIDE_BYTES);
            gate -> ag_Side =  (APTR) 0;
        }

        /* The signal bit died with the owner; drop only the record.  */
        gate -> ag_DoneMask =  0UL;
        gate -> ag_Task     =  (struct Task *) 0;

        reaped =  (UINT) TX_TRUE;
    }

    Permit();

    return(reaped);
}


VOID tx_amiga_gate_fallback_note(VOID)
{

    Forbid();
    _tx_green_counters.gc_gate_fallback++;
    Permit();
}


VOID tx_amiga_gate_fast_note(VOID)
{

    Forbid();
    _tx_green_counters.gc_gate_fast++;
    Permit();
}


/* ------------------------------------------------------------- statistics --- */

VOID tx_amiga_green_stats(TX_AMIGA_GREEN_STATS *stats)
{

    if (stats == (TX_AMIGA_GREEN_STATS *) 0)
    {
        return;
    }

    Forbid();
    stats -> gs_switches      =  _tx_green_counters.gc_switches;
    stats -> gs_external      =  _tx_green_counters.gc_external;
    stats -> gs_idle_waits    =  _tx_green_counters.gc_idle_waits;
    stats -> gs_wait_fast     =  _tx_green_counters.gc_wait_fast;
    stats -> gs_wait_slow     =  _tx_green_counters.gc_wait_slow;
    stats -> gs_stray_wait    =  _tx_green_counters.gc_stray_wait;
    stats -> gs_gate_calls    =  _tx_green_counters.gc_gate_calls;
    stats -> gs_gate_fallback =  _tx_green_counters.gc_gate_fallback;
    stats -> gs_gate_fast     =  _tx_green_counters.gc_gate_fast;

    /* How many of the 16 allocatable bits (16..31) the realm Task has out.  Every
       green thread's CreateMsgPort()/AllocSignal() draws from this one budget.  */
    stats -> gs_realm_sigbits =  0UL;
    if (_tx_amiga_scheduler_task != (VOID *) 0)
    {

        ULONG   alloc =  ((struct Task *) _tx_amiga_scheduler_task)
                             -> tc_SigAlloc >> 16;

        while (alloc != 0UL)
        {
            stats -> gs_realm_sigbits +=  alloc & 1UL;
            alloc >>=  1;
        }
    }
    Permit();
}

VOID tx_amiga_green_stray_wait_note(VOID)
{

    Forbid();
    _tx_green_counters.gc_stray_wait++;
    Permit();
}

#else /* !AMINETXDUO_GREEN_REALM */

/* The public accessors exist in every build, so tools can link against them
   and read zeros from a baton build.  */

UINT tx_amiga_green_active(VOID)
{
    return((UINT) 0);
}

ULONG tx_amiga_green_wait(ULONG sigmask)
{
    if (sigmask == 0UL)
    {
        return(0UL);
    }
    return(Wait(sigmask));
}

VOID tx_amiga_green_stats(TX_AMIGA_GREEN_STATS *stats)
{
    if (stats != (TX_AMIGA_GREEN_STATS *) 0)
    {
        stats -> gs_switches      =  0UL;
        stats -> gs_external      =  0UL;
        stats -> gs_idle_waits    =  0UL;
        stats -> gs_wait_fast     =  0UL;
        stats -> gs_wait_slow     =  0UL;
        stats -> gs_stray_wait    =  0UL;
        stats -> gs_gate_calls    =  0UL;
        stats -> gs_gate_fallback =  0UL;
        stats -> gs_realm_sigbits =  0UL;
        stats -> gs_gate_fast     =  0UL;
    }
}

VOID tx_amiga_green_stray_wait_note(VOID)
{
}

VOID tx_amiga_gate_fallback_note(VOID)
{
}

VOID tx_amiga_gate_fast_note(VOID)
{
}

/* TX_THREAD_TERMINATED_EXTENSION's target; there is no waiter table to
   purge in a baton build.  */
VOID _tx_amiga_thread_terminated(TX_THREAD *thread_ptr)
{
    (VOID) thread_ptr;
}

#endif /* AMINETXDUO_GREEN_REALM */
