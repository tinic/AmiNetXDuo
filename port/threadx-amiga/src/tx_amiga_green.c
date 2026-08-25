/***************************************************************************
 * AmiNetXDuo, the green realm: ThreadX threads as coroutines inside one
 * Exec Task.
 *
 * SPDX-License-Identifier: MIT
 **************************************************************************/

/**************************************************************************/
/*                                                                        */
/*    tx_amiga_green                                    AmigaOS/m68k       */
/*                                                                        */
/*  DESCRIPTION                                                           */
/*                                                                        */
/*    Everything the green realm adds to the port that is not a rewrite    */
/*    of one of the eight port files: the initial stack frame, the first-  */
/*    activation shim, the Exec-signal wait that replaces the baton's      */
/*    release-around-Wait() bracket, and the counters the prototype is     */
/*    measured by.                                                         */
/*                                                                        */
/*    The model (docs/THREADING-OPTIONS.md option 4): threads the stack    */
/*    creates through tx_thread_create() no longer get an Exec Task each.  */
/*    Their context is their ThreadX stack plus a saved SP, and the realm  */
/*    Task -- the master Task sitting in tx_kernel_enter() -- enters and   */
/*    leaves those contexts with _tx_green_switch().  A ThreadX handoff    */
/*    between two such threads is a stack switch, not an Exec              */
/*    Signal/Wait/dispatch round trip.  Adopted threads (application       */
/*    Tasks entering through bsdsocket) keep their own Exec Task and the   */
/*    baton protocol; the realm hands them the baton exactly the way the   */
/*    old scheduler Task did, and takes it back at their next yield.       */
/*                                                                        */
/*    A green thread MUST NOT block in Exec: its Wait() would put the      */
/*    whole realm Task to sleep with every other green thread's work on    */
/*    it.  The sites that used to do so (the SANA-II reader loop, the      */
/*    synchronous device commands) call tx_amiga_green_wait() instead,     */
/*    which suspends only the green thread and leaves the one real Wait()  */
/*    to the realm's idle loop.  The probe build turns any stray Exec      */
/*    Wait() from green context into a counted, logged green wait (see     */
/*    ami_green_checked_wait in src/netstack/netstack_baton.c).            */
/*                                                                        */
/**************************************************************************/

#define TX_SOURCE_CODE

#include "tx_amiga_internal.h"

#ifdef AMINETXDUO_GREEN_REALM


/* The realm scheduler's saved context (loaded by a yielding green thread,
   stored by the scheduler as it dispatches one).  */
APTR    _tx_green_scheduler_sp =  (APTR) 0;

struct _tx_green_counters   _tx_green_counters;


/* ------------------------------------------------------ signal waiters --- */

/*
 * One slot per green thread currently inside tx_amiga_green_wait().  The
 * realm has few green threads (three readers, IP, mDNS, AutoIP, two DHCPv6),
 * and only the readers and the occasional control command wait on signals,
 * so a small fixed table under the port's own Forbid() discipline is enough.
 *
 * All fields are touched only under Forbid().
 */

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


/*
 * Hand latched Exec signals to the green threads waiting on them, resuming
 * each one so the dispatch pass that follows can pick it by priority.  Call
 * under Forbid().  The received bits accumulate in the slot; the woken
 * thread collects them (and frees the slot) when it runs.
 *
 * The resume uses the adoption files' shape: _tx_thread_system_state is
 * raised so _tx_thread_system_resume() defers every context switch back to
 * the caller -- the realm loop, which is about to run its own dispatch.
 */
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

/*
 * TX_TRUE while the calling code runs in a green context: on the realm Task,
 * with a green thread holding the baton.  The tick Task, adopted callers and
 * plain Exec Tasks all answer TX_FALSE.
 */
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


/*
 * Sleep the calling GREEN thread until one of `sigmask`'s Exec signals is
 * latched on the realm Task, and return the bits that arrived.  The green
 * replacement for ami_sana2_block_enter(); Wait(mask); ami_sana2_block_leave().
 *
 * The signals belong to the realm Task (a green thread's CreateMsgPort() and
 * AllocSignal() ran on it), so a latched bit is consumed here with
 * SetSignal() when it is already pending, and otherwise by the realm's
 * Wait()/SetSignal() and routed through the waiter slot.  Bits belonging to
 * a thread that is NOT registered are never consumed by the scheduler --
 * _tx_green_pending_union() covers registered waiters only -- so a signal
 * that arrives while its thread is busy stays latched until that thread
 * waits again.  No lost wakeups.
 *
 * Must not be called holding a ThreadX mutex (same discipline the baton
 * bracket demanded), and only from a green thread.
 */
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

        /* Table full -- cannot happen with the stack's thread census, but a
           spin here would hang the realm.  Busy-wait via the scheduler: yield
           and re-test, which keeps every other thread running.  */
        Permit();
        _tx_thread_sleep(1);
        return(tx_amiga_green_wait(sigmask));
    }

    gw -> gw_thread   =  thread;
    gw -> gw_mask     =  sigmask;
    gw -> gw_received =  0UL;

    _tx_green_counters.gc_wait_slow++;

    /* Off the ready list, without switching (interrupt-context shape, the
       same one netstack_baton.c and tx_amiga_adopt.c use).  */
    _tx_thread_system_state++;
    (VOID) _tx_thread_suspend(thread);
    _tx_thread_system_state--;

    /* Release the baton and give the machine back to the realm loop.  The
       hold that ends here ends at a would-be driver bracket, which is the
       site tag the holder ring already names.  */
    ami_budget_hold_end((APTR) thread, thread -> tx_thread_name,
                        (ULONG) thread -> tx_thread_state,
                        AMI_HOLD_SITE_BRACKET);
    _tx_thread_current_ptr =  TX_NULL;

    _tx_green_switch(&thread -> tx_thread_stack_ptr, _tx_green_scheduler_sp);

    /* Dispatched again: the scheduler delivered our signals, resumed us and
       switched in, holding the protocol Forbid().  Collect and free.  */
    got =  gw -> gw_received;
    gw -> gw_thread   =  TX_NULL;
    gw -> gw_mask     =  0UL;
    gw -> gw_received =  0UL;

    Permit();

    return(got);
}


/* ------------------------------------------------------- first activation --- */

/*
 * Where a green thread's initial frame "returns" to.  The dispatcher's
 * protocol Forbid() is still held; give it back, then enter the thread the
 * way every ThreadX port does.
 *
 * _tx_thread_shell_entry() is not supposed to return (a completed thread
 * suspends itself, and the green _tx_thread_system_return() parks its
 * context on the scheduler).  If it does return -- the preempt-disable /
 * system-state guard declined the suspend -- there is no Exec Task to
 * destroy here and no caller to unwind to: park the context on the
 * scheduler for good.
 */
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

/*
 * Lay a first-activation frame on a green thread's stack: the 44-byte
 * d2-d7/a2-a6 image (zeroed -- the tree is not -fbaserel, no register
 * carries an ambient base) under _tx_green_thread_begin as the return
 * address.  Must mirror _tx_green_switch()'s pop exactly; both files say 44.
 */
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

/*
 * The probe build's net under the whole realm: netstack_internal.h and
 * sana2_internal.h #define Wait() to this in GREEN_REALM+RXPROBE builds, so
 * any Exec Wait() still reachable from a green thread -- the one defect
 * class that hangs the realm outright -- is caught, counted (gs_stray_wait,
 * MUST read zero) and converted into the green wait it should have been.
 * Ordinary contexts pass straight through.  It lives here and not in the
 * netstack because this file never sees that macro, so the passthrough
 * below calls the real Wait().
 */
#ifdef AMINETXDUO_RXPROBE

#include "aminetxduo/compat.h"

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

/*
 * The client boundary, docs/THREADING-OPTIONS.md option 4: an application
 * Task's bracket becomes "submit the continuation, park, one Signal back".
 * The continuation is captured by _tx_green_switch() itself -- what it saves
 * on the caller's stack IS the request -- and the same switch moves the
 * caller onto its side stack, where the parker resumes the proxy and sleeps.
 * The realm then dispatches the proxy like any green thread: the vector body
 * resumes at the capture point, on the caller's stack memory, under the
 * realm's feet, free to suspend in NetX at stack-switch cost.
 *
 * What the parked owner still owns is its Exec signal state: Ctrl-C lands on
 * the OWNER, so the parker's Wait() covers the break mask too and collects
 * what arrives for the body to poll through tx_amiga_gate_breaks().  The
 * bits are re-posted to the owner at gate_return, preserving the "EINTR
 * leaves the signal set" contract.
 */

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
       internal create is used on purpose: _txe_thread_create() would refuse
       the owner's stack region as overlapping when a second base on the same
       Task binds, and the region really is shared -- serially, one bracket
       at a time, which the nest counter already guarantees.  */
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

    /* The capture: everything from here to gate_return becomes the proxy's
       context.  First continuation: the parker, on the side stack, with this
       Forbid() still held.  Second continuation: the realm dispatches the
       proxy and execution RESUMES RIGHT HERE on the realm Task, holding the
       dispatcher's Forbid(), per the switch protocol.  */
    _tx_green_switch(&gate -> ag_Thread.tx_thread_stack_ptr,
                     (APTR) frame);

    Permit();

    return(TX_SUCCESS);
}


/* The side-stack half of the owner: resume the proxy, wake the realm, then
   sleep in the ONE boundary Wait() collecting break bits, and finally jump
   back into the leave-side context gate_return left behind.  Entered from
   __tx_gate_park_entry with the capture Forbid() held; never returns.  */
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

    /* Store the leave-side context and give the machine to the realm loop.
       The parked owner resumes it -- execution continues after this call on
       the OWNER Task -- once the realm lets the machine go and the owner's
       Wait() returns.  */
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
    }
}

VOID tx_amiga_green_stray_wait_note(VOID)
{
}

VOID tx_amiga_gate_fallback_note(VOID)
{
}

#endif /* AMINETXDUO_GREEN_REALM */
