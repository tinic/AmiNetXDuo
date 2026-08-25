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


/* ------------------------------------------------------------- statistics --- */

VOID tx_amiga_green_stats(TX_AMIGA_GREEN_STATS *stats)
{

    if (stats == (TX_AMIGA_GREEN_STATS *) 0)
    {
        return;
    }

    Forbid();
    stats -> gs_switches   =  _tx_green_counters.gc_switches;
    stats -> gs_external   =  _tx_green_counters.gc_external;
    stats -> gs_idle_waits =  _tx_green_counters.gc_idle_waits;
    stats -> gs_wait_fast  =  _tx_green_counters.gc_wait_fast;
    stats -> gs_wait_slow  =  _tx_green_counters.gc_wait_slow;
    stats -> gs_stray_wait =  _tx_green_counters.gc_stray_wait;
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
        stats -> gs_switches   =  0UL;
        stats -> gs_external   =  0UL;
        stats -> gs_idle_waits =  0UL;
        stats -> gs_wait_fast  =  0UL;
        stats -> gs_wait_slow  =  0UL;
        stats -> gs_stray_wait =  0UL;
    }
}

VOID tx_amiga_green_stray_wait_note(VOID)
{
}

#endif /* AMINETXDUO_GREEN_REALM */
