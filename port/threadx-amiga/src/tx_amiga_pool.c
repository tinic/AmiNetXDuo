/***************************************************************************
 * AmiNetXDuo, the port's TX_THREAD pool for adopted Exec Tasks.
 *
 * SPDX-License-Identifier: MIT
 **************************************************************************/

/* WHY THE PORT OWNS THE STORAGE

   An adopted Task's TX_THREAD used to be the caller's: ami_netstack_enter() was
   handed an AmiNetCaller and adopted into the TX_THREAD inside it.  Two of those
   AmiNetCallers live on the CALLER'S OWN STACK (netstack.c, the startup and the
   shutdown path), so a RemTask() of such a Task frees the storage that
   _tx_thread_current_ptr still points at.  Recovering the baton from a dead
   holder then means reading freed memory, and nothing can make that safe.

   So the storage is the port's.  A slot outlives every Task it is lent to, and
   the pointer a caller keeps is a HANDLE: the slot address plus the generation
   the slot carried when it was claimed.  A slot recycled under a caller that
   never came back to release it fails the generation test, and that caller's
   later teardown does nothing instead of tearing down somebody else's thread.

   WHY SIXTEEN

   A slot is held for as long as an Exec Task is inside the stack, and a CACHED
   bracket holds one for the whole life of the caller's bsdsocket base
   (AmiNetSocketBase.sb_NxCaller), not just for one call.  The cap is therefore
   "concurrent networking programs", not "concurrent socket calls".  Sixteen is
   far past what the 4 MB machines this targets run at once, and costs sixteen
   TX_THREADs of BSS in the library.  Seventeen does not fail: it waits.

   WHY ONE OF THEM IS RESERVED

   One caller must never park: the one inside ami_ns_lock.  The stack's
   interface-control calls take that lock and then adopt, and if such a caller
   waited for a slot it would hold the lock for as long as the pool stayed
   full -- while a slot holder that wanted the lock waited on it.  That is a
   cycle, and it is the only one.

   So the last slot is kept for a caller that holds ami_ns_lock, and only for
   such a caller.  ONE is enough and that is provable rather than guessed: the
   lock is exclusive, so at most one Task holds it, and a nested adoption from
   that same Task finds itself already a thread and claims nothing more.  The
   other fifteen are for everybody, cached or not.

   An earlier version reserved four and keyed them off "is this adoption cached"
   instead.  That protected nothing: it let a thirteenth cached user wait with
   four slots idle, and with AMINETXDUO_NXCACHE=OFF every ordinary call is a
   transient one, so transients could take all sixteen and leave the lock holder
   exactly where it started.  The property that matters is who holds the lock,
   and src/netstack/netstack_lock.c answers that exactly.  */

#define TX_SOURCE_CODE

#include "tx_amiga_internal.h"


#ifndef TX_AMIGA_ADOPT_SLOTS
#define TX_AMIGA_ADOPT_SLOTS        16
#endif

/* Tasks that may be waiting for a slot at once.  Only ever reached when all
   sixteen slots are taken, so this is the depth of the queue behind a full
   pool and not a second limit on the stack's users.  */
#ifndef TX_AMIGA_ADOPT_WAITERS
#define TX_AMIGA_ADOPT_WAITERS      32
#endif

/* Slots only a caller that may not park can take.  */
#ifndef TX_AMIGA_ADOPT_RESERVE
#define TX_AMIGA_ADOPT_RESERVE      1
#endif


struct _tx_amiga_adopt_waiter
{
    struct Task    *aw_task;            /* 0 marks the entry free                 */
    ULONG           aw_signal;
    ULONG           aw_stamp;           /* what the Task looked like when it       */
};                                      /* registered; see the wake below          */


/* BSS, not an allocation made at kernel start.  Two reasons: a stale handle is
   range-checked against this array, and that check must be answerable when the
   kernel is down, which a freed pool cannot do without the pointer having been
   compared against freed storage; and a Task parked on a full pool must not
   have the table it is registered in freed under it by tx_amiga_kernel_stop().
   Nothing here needs initialising -- a zeroed slot is a free slot.  */
static struct _tx_amiga_adopt_slot      _tx_amiga_adopt_pool[TX_AMIGA_ADOPT_SLOTS];
static struct _tx_amiga_adopt_waiter    _tx_amiga_adopt_waiters[TX_AMIGA_ADOPT_WAITERS];

/* Monotonic and NEVER reset, including across a kernel stop and restart: a
   handle left over from the previous kernel must not match a slot in the next
   one.  Wraps after 2^32 adoptions and skips 0 when it does, so the only way a
   stale handle can match is to be exactly 2^32 adoptions old.  */
static ULONG                            _tx_amiga_adopt_generation;

ULONG   _tx_amiga_adopt_parks;          /* adoptions that had to wait for a slot */
ULONG   _tx_amiga_adopt_waiting;        /* Tasks waiting for one RIGHT NOW       */
ULONG   _tx_amiga_adopt_peak;           /* high-water slots in use               */
ULONG   _tx_amiga_adopt_waiter_full;    /* refusals: the waiter table was full   */
ULONG   _tx_amiga_adopt_unpublished_freed;  /* slots whose claimer died first    */


/* The slot a TX_THREAD * names, or 0.  Pointer arithmetic only: a stale or
   foreign pointer is rejected without being dereferenced.  */
static struct _tx_amiga_adopt_slot *_tx_amiga_slot_of(TX_THREAD *thread_ptr)
{

UBYTE                           *base;
UBYTE                           *end;
ULONG                            index;
struct _tx_amiga_adopt_slot     *slot;


    if (thread_ptr == TX_NULL)
    {
        return((struct _tx_amiga_adopt_slot *) 0);
    }

    base =  (UBYTE *) &_tx_amiga_adopt_pool[0];
    end  =  (UBYTE *) &_tx_amiga_adopt_pool[TX_AMIGA_ADOPT_SLOTS];

    if ((((UBYTE *) thread_ptr) < base) || (((UBYTE *) thread_ptr) >= end))
    {
        return((struct _tx_amiga_adopt_slot *) 0);
    }

    index =  ((ULONG) (((UBYTE *) thread_ptr) - base)) /
             ((ULONG) sizeof(struct _tx_amiga_adopt_slot));
    slot  =  &_tx_amiga_adopt_pool[index];

    /* An interior pointer into a slot is not that slot's handle.  */
    if (&slot -> as_thread != thread_ptr)
    {
        return((struct _tx_amiga_adopt_slot *) 0);
    }

    return(slot);
}


ULONG tx_amiga_adopt_slots(VOID)
{

    return((ULONG) TX_AMIGA_ADOPT_SLOTS);
}


ULONG tx_amiga_adopt_reserve(VOID)
{

    return((ULONG) TX_AMIGA_ADOPT_RESERVE);
}


ULONG tx_amiga_adopt_slots_free(VOID)
{

ULONG   i;
ULONG   free_slots;


    free_slots =  0UL;

    Forbid();
    for (i = 0UL; i < ((ULONG) TX_AMIGA_ADOPT_SLOTS); i++)
    {
        if (_tx_amiga_adopt_pool[i].as_busy == ((UINT) 0))
        {
            free_slots++;
        }
    }
    Permit();

    return(free_slots);
}


ULONG tx_amiga_adopt_generation(TX_THREAD *thread_ptr)
{

struct _tx_amiga_adopt_slot *slot;


    slot =  _tx_amiga_slot_of(thread_ptr);

    if ((slot == (struct _tx_amiga_adopt_slot *) 0) || (slot -> as_busy == ((UINT) 0)))
    {
        return(0UL);
    }

    return(slot -> as_generation);
}


UINT tx_amiga_adopt_handle_valid(TX_THREAD *thread_ptr, ULONG generation)
{

    if (generation == 0UL)
    {
        return((UINT) TX_FALSE);
    }

    return((tx_amiga_adopt_generation(thread_ptr) == generation)
           ? ((UINT) TX_TRUE) : ((UINT) TX_FALSE));
}


/* Signal every Task still waiting for a slot.  Caller holds Forbid().

   An entry is only signalled once it has been shown, under Disable(), to name a
   Task that is still on one of Exec's scheduler lists, that still owns the bit
   it asked to be woken on, and that still carries the stamp it registered with.
   For a wrong Task to be signalled its address would have to have been recycled
   inside the window since the last release BY a Task that had allocated the
   same signal bit AND had the same stack bounds and name pointer.

   An entry is DROPPED only on the first two: a Task Exec has removed, or a Task
   at that address that has given the bit back, is not the waiter.  A stamp
   mismatch alone neither signals nor drops, and never touches the registered
   bit -- it retries on the next release -- because dropping there is the one
   thing that could lose a live waiter's only wakeup, and a parked adopter is
   inside this port with nothing that can move its stack bounds.

   `final` is the shutdown pass, and it is the answer to what a retained
   mismatch costs.  After tx_amiga_kernel_stop() there are no more releases, so
   "retries on the next release" would keep the entry for ever.  But a waiter
   that is really parked is inside this port, in Wait(), and cannot change its
   stack bounds, its name pointer or its UniqueID, so on this pass a mismatch
   means the waiter has gone and its address now holds a different Task.  That
   entry is cleared WITHOUT a Signal: the Task at the address never asked for
   one.  A match is signalled as on any other pass, and every entry is cleared
   either way.  */
VOID _tx_amiga_adopt_wake_scan(UINT final)
{

ULONG            i;
struct Task     *task;


    Disable();

    for (i = 0UL; i < ((ULONG) TX_AMIGA_ADOPT_WAITERS); i++)
    {

        task =  _tx_amiga_adopt_waiters[i].aw_task;
        if (task == (struct Task *) 0)
        {
            continue;
        }

        if ((tx_amiga_task_alive_locked(task) == ((UINT) TX_FALSE)) ||
            ((task -> tc_SigAlloc & _tx_amiga_adopt_waiters[i].aw_signal) == 0UL))
        {
            _tx_amiga_adopt_waiters[i].aw_task =  (struct Task *) 0;
            if (_tx_amiga_adopt_waiting > 0UL)
            {
                _tx_amiga_adopt_waiting--;
            }
            continue;
        }

        if (_tx_amiga_task_stamp(task) != _tx_amiga_adopt_waiters[i].aw_stamp)
        {
            /* Ordinary pass: retain, silently.  Final pass: the waiter is
               gone, so the entry goes and nobody is signalled.  */
            if (final != ((UINT) TX_FALSE))
            {
                _tx_amiga_adopt_waiters[i].aw_task =  (struct Task *) 0;
                if (_tx_amiga_adopt_waiting > 0UL)
                {
                    _tx_amiga_adopt_waiting--;
                }
            }
            continue;
        }

        Signal(task, _tx_amiga_adopt_waiters[i].aw_signal);

        if (final != ((UINT) TX_FALSE))
        {
            /* Nothing will scan this table again.  The waiter's own cleanup
               only clears an entry that still names it, so clearing here
               cannot take one a later waiter has claimed.  */
            _tx_amiga_adopt_waiters[i].aw_task =  (struct Task *) 0;
            if (_tx_amiga_adopt_waiting > 0UL)
            {
                _tx_amiga_adopt_waiting--;
            }
        }
    }

    Enable();
}


VOID _tx_amiga_adopt_wake_waiters_locked(VOID)
{

    _tx_amiga_adopt_wake_scan((UINT) TX_FALSE);
}


VOID _tx_amiga_adopt_wake_waiters(VOID)
{

    Forbid();
    _tx_amiga_adopt_wake_scan((UINT) TX_FALSE);
    Permit();
}


/* The shutdown pass; see _tx_amiga_adopt_wake_scan().  */
VOID _tx_amiga_adopt_wake_waiters_final(VOID)
{

    Forbid();
    _tx_amiga_adopt_wake_scan((UINT) TX_TRUE);
    Permit();
}


/* Take a free slot, or 0 when the pool is full.  Caller holds Forbid().
   `reserved` is TX_TRUE only for a caller that must not park -- one holding
   ami_ns_lock -- and is the only kind that may have the reserved tail.  */
struct _tx_amiga_adopt_slot *_tx_amiga_slot_claim_locked(UINT reserved)
{

ULONG                            i;
ULONG                            used;
ULONG                            limit;
UBYTE                           *bytes;
struct _tx_amiga_adopt_slot     *found;


    /* THE SAME Forbid() THE STOP COMMITS UNDER.  tx_amiga_kernel_stop() clears
       _tx_amiga_kernel_up and raises _tx_amiga_kernel_stopping inside one
       Forbid(); reading them here means a claim either happens wholly before
       that commit or sees it, and never lands a fresh adoption on a kernel
       that has already counted its threads and started tearing down.  */
    if ((_tx_amiga_kernel_up == TX_FALSE) || (_tx_amiga_kernel_stopping != TX_FALSE))
    {
        return((struct _tx_amiga_adopt_slot *) 0);
    }

    found =  (struct _tx_amiga_adopt_slot *) 0;
    used  =  0UL;
    limit =  (reserved != ((UINT) TX_FALSE))
             ? ((ULONG) TX_AMIGA_ADOPT_SLOTS)
             : ((ULONG) (TX_AMIGA_ADOPT_SLOTS - TX_AMIGA_ADOPT_RESERVE));

    for (i = 0UL; i < ((ULONG) TX_AMIGA_ADOPT_SLOTS); i++)
    {
        if (_tx_amiga_adopt_pool[i].as_busy != ((UINT) 0))
        {
            used++;
        }
        else if ((found == (struct _tx_amiga_adopt_slot *) 0) && (i < limit))
        {
            found =  &_tx_amiga_adopt_pool[i];
        }
        else
        {
            /* Taken, or beyond what this kind of adoption may have.  */
        }
    }

    if (found == (struct _tx_amiga_adopt_slot *) 0)
    {
        return(found);
    }

    /* A fresh TX_THREAD.  _tx_thread_create() fills in what it owns, but the
       port's own extension fields are read before it runs (tx_thread_stack_build.c)
       and a slot must never carry a word of its last tenant.  */
    bytes =  (UBYTE *) &found -> as_thread;
    for (i = 0UL; i < ((ULONG) sizeof(TX_THREAD)); i++)
    {
        bytes[i] =  0;
    }

    _tx_amiga_adopt_generation++;
    if (_tx_amiga_adopt_generation == 0UL)
    {
        _tx_amiga_adopt_generation =  1UL;
    }

    found -> as_generation =  _tx_amiga_adopt_generation;
    found -> as_busy       =  (UINT) 1;

    /* Recorded under the claim's own Forbid, so there is no instant at which a
       busy slot has no owner.  The claimer has to leave Forbid() before the
       create, and a RemTask() in that window would otherwise leak the slot;
       tx_amiga_adopt_sweep_unpublished() is what gives it back.  */
    found -> as_claimer     =  FindTask((STRPTR) 0);
    found -> as_claim_stamp =  _tx_amiga_task_stamp(found -> as_claimer);
    found -> as_published   =  (UINT) TX_FALSE;

    used++;
    if (used > _tx_amiga_adopt_peak)
    {
        _tx_amiga_adopt_peak =  used;
    }

    return(found);
}


/* Give a slot back.  ONLY after the TX_THREAD in it has been torn down: the
   generation is cleared here, so every handle that named this adoption stops
   matching at this instant, and the waiters are woken from the same Forbid().  */
VOID _tx_amiga_slot_release_locked(struct _tx_amiga_adopt_slot *slot)
{

    if (slot == (struct _tx_amiga_adopt_slot *) 0)
    {
        return;
    }

    slot -> as_generation  =  0UL;
    slot -> as_busy        =  (UINT) 0;
    slot -> as_claimer     =  (struct Task *) 0;
    slot -> as_claim_stamp =  0UL;
    slot -> as_published   =  (UINT) TX_FALSE;

    _tx_amiga_adopt_wake_waiters_locked();
}


/* The TX_THREAD in the slot now exists and the caller has its handle.  From
   here the ordinary dead-holder reclaim owns the slot, so the claim record
   goes.  Caller holds Forbid(), the same one the create ran under.  */
VOID _tx_amiga_slot_publish_locked(struct _tx_amiga_adopt_slot *slot)
{

    if (slot == (struct _tx_amiga_adopt_slot *) 0)
    {
        return;
    }

    slot -> as_published   =  (UINT) TX_TRUE;
    slot -> as_claimer     =  (struct Task *) 0;
    slot -> as_claim_stamp =  0UL;
}


/* Give back every slot whose claimer died before it could publish.

   This is the half of dead-holder recovery the baton reclaim cannot see: a
   Task removed between taking a slot and creating the TX_THREAD in it leaves a
   busy slot with no thread, so there is nothing for tx_amiga_adopted_task_dead()
   to be asked about.  A slot that HAS been published needs none of this -- its
   TX_THREAD is on ThreadX's lists, so the corpse is dispatched, becomes the
   baton holder and is reclaimed from the tick like any other.

   THE CLAIMER IS THE ADDRESS AND THE STAMP.  Between the claim and the
   publish the claimer is inside tx_amiga_adopt_thread(), so its stack bounds,
   name pointer and UniqueID cannot move: a Task at that address with another
   stamp is not the claimer, whose slot is freed as if the address were empty.
   The address alone kept such a slot for as long as the recycling Task lived,
   across a kernel restart too, since the pool is BSS.  A claimer that is alive
   with its own stamp -- stalled between the claim and the create -- is never
   touched.

   Runs from the tick.  Takes its own Forbid(); the liveness test is the one
   question that may be asked about storage Exec may already have freed, so the
   stamp is read only after it has answered "still there", under the same
   Disable().  How many it has ever freed is _tx_amiga_adopt_unpublished_freed.  */
VOID tx_amiga_adopt_sweep_unpublished(VOID)
{

ULONG            i;
struct Task     *claimer;


    Forbid();

    for (i = 0UL; i < ((ULONG) TX_AMIGA_ADOPT_SLOTS); i++)
    {
        if ((_tx_amiga_adopt_pool[i].as_busy == ((UINT) 0)) ||
            (_tx_amiga_adopt_pool[i].as_published != ((UINT) TX_FALSE)))
        {
            continue;
        }

        claimer =  _tx_amiga_adopt_pool[i].as_claimer;
        if (claimer == (struct Task *) 0)
        {
            continue;
        }

        Disable();
        if ((tx_amiga_task_alive_locked(claimer) != ((UINT) TX_FALSE)) &&
            (_tx_amiga_task_stamp(claimer) == _tx_amiga_adopt_pool[i].as_claim_stamp))
        {
            Enable();
            continue;
        }
        Enable();

        _tx_amiga_slot_release_locked(&_tx_amiga_adopt_pool[i]);
        _tx_amiga_adopt_unpublished_freed++;
    }

    Permit();
}


/* FOR THE PORT'S OWN HARNESS.  Leaves exactly the residue a Task removed
   between the claim and the create leaves: a busy slot, claimed by this Task,
   with no TX_THREAD in it.  Nothing in the stack calls this.  */
UINT tx_amiga_adopt_claim_orphan(VOID)
{

struct _tx_amiga_adopt_slot *slot;


    Forbid();
    slot =  _tx_amiga_slot_claim_locked((UINT) TX_FALSE);
    Permit();

    return((slot != (struct _tx_amiga_adopt_slot *) 0)
           ? ((UINT) TX_TRUE) : ((UINT) TX_FALSE));
}


/* The slot behind a validated handle, for the release sites.  Caller holds
   Forbid().  */
struct _tx_amiga_adopt_slot *_tx_amiga_slot_held(TX_THREAD *thread_ptr, ULONG generation)
{

struct _tx_amiga_adopt_slot *slot;


    slot =  _tx_amiga_slot_of(thread_ptr);

    if ((slot == (struct _tx_amiga_adopt_slot *) 0) ||
        (slot -> as_busy == ((UINT) 0)) ||
        (generation == 0UL) ||
        (slot -> as_generation != generation))
    {
        return((struct _tx_amiga_adopt_slot *) 0);
    }

    return(slot);
}


/* Wait for a slot.

   THE PARK PATH HOLDS NOTHING.  It is reached from tx_amiga_adopt_thread()
   before anything has been adopted, so the caller holds no ThreadX baton; it is
   reached before ami_netstack_enter() returns, which every bsdsocket vector
   calls before it takes sb_Lock, so the caller holds no semaphore of ours; and
   the Wait() below is made with Forbid() released.  The only thing that frees a
   slot is another Task completing its own teardown, and nothing here can stop
   one doing that.

   Returns the claimed slot, or 0 when the waiter table was full or the kernel
   went away while we waited.  */
struct _tx_amiga_adopt_slot *_tx_amiga_slot_claim_or_park(struct Task *me, UINT reserved)
{

struct _tx_amiga_adopt_slot *slot;
LONG                         index;
BYTE                         sig;
ULONG                        sigmask;
ULONG                        i;


    Forbid();
    slot =  _tx_amiga_slot_claim_locked(reserved);
    Permit();

    if (slot != (struct _tx_amiga_adopt_slot *) 0)
    {
        return(slot);
    }

    sig =  AllocSignal(-1);
    if (sig < 0)
    {
        return((struct _tx_amiga_adopt_slot *) 0);
    }
    sigmask =  1UL << ((ULONG) sig);

    index =  -1;

    Forbid();

    for (i = 0UL; i < ((ULONG) TX_AMIGA_ADOPT_WAITERS); i++)
    {
        if (_tx_amiga_adopt_waiters[i].aw_task == (struct Task *) 0)
        {
            _tx_amiga_adopt_waiters[i].aw_task   =  me;
            _tx_amiga_adopt_waiters[i].aw_signal =  sigmask;
            _tx_amiga_adopt_waiters[i].aw_stamp  =  _tx_amiga_task_stamp(me);
            _tx_amiga_adopt_waiting++;
            index =  (LONG) i;
            break;
        }
    }

    /* Registered BEFORE this last look, so a release between the two arrives as
       a signal rather than being missed.  */
    if (index >= 0)
    {
        slot =  _tx_amiga_slot_claim_locked(reserved);
    }

    Permit();

    if (index < 0)
    {
        _tx_amiga_adopt_waiter_full++;
        FreeSignal(sig);
        return((struct _tx_amiga_adopt_slot *) 0);
    }

    while (slot == (struct _tx_amiga_adopt_slot *) 0)
    {

        if ((_tx_amiga_kernel_up == TX_FALSE) || (_tx_amiga_kernel_stopping != TX_FALSE))
        {
            break;
        }

        (VOID) Wait(sigmask);

        Forbid();
        slot =  _tx_amiga_slot_claim_locked(reserved);
        Permit();
    }

    Forbid();
    if (_tx_amiga_adopt_waiters[index].aw_task == me)
    {
        _tx_amiga_adopt_waiters[index].aw_task =  (struct Task *) 0;
        if (_tx_amiga_adopt_waiting > 0UL)
        {
            _tx_amiga_adopt_waiting--;
        }
    }
    Permit();

    SetSignal(0UL, sigmask);
    FreeSignal(sig);

    if (slot != (struct _tx_amiga_adopt_slot *) 0)
    {
        _tx_amiga_adopt_parks++;
    }

    return(slot);
}
