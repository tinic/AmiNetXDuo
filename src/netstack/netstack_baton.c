/*
 * AmiNetXDuo, releasing and reacquiring the ThreadX baton around an exec
 * Wait().
 *
 * The port uses the baton model (docs/RESEARCH.md 6.2): a ThreadX thread runs
 * only if it is _tx_thread_current_ptr, and _tx_thread_schedule() does not
 * dispatch anyone while that pointer is non-NULL. The SANA-II reader threads,
 * and every driver control command, block in exec Wait() for an IORequest,
 * which ThreadX knows nothing about. Doing so while holding the baton stalls
 * the whole stack: the IP thread, the timer thread and every other socket user
 * queue behind a task that does not become runnable until a packet arrives.
 *
 * Clearing _tx_thread_current_ptr alone moves the deadlock one step later: the
 * thread is still ready, so the scheduler picks it straight back (the readers
 * are the highest-priority threads in the system), sets _tx_thread_current_ptr
 * again and pokes a run signal the thread is not waiting for. The thread must
 * come off the ready list too.
 *
 * release():  suspend the calling thread as tx_thread_suspend() does, but with
 *             _tx_thread_system_state raised so ThreadX treats it as interrupt
 *             context and does not context-switch on its behalf. Then clear
 *             the baton and wake the scheduler.
 * acquire():  resume the thread the same way, then park on the run signal
 *             until the scheduler restores the baton.
 *
 * tx_amiga_adopt.c uses the same shape: raise the system state over the window
 * in which an Exec Task touches ThreadX state without being a scheduled
 * thread, and drive _tx_thread_current_ptr and the run signal directly.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_internal.h"

#include "tx_thread.h"
#include "tx_timer.h"
#include "tx_amiga.h"

#include "aminetxduo/health.h"

#include <exec/tasks.h>
#include <proto/exec.h>

/* Port internals (port/threadx-amiga/src/). Not in tx_amiga.h because they are
   not application API, but a blocking bracket needs them. */
extern VOID _tx_amiga_wake_scheduler(VOID);
extern UINT _tx_amiga_thread_park(TX_THREAD *thread_ptr);

/* Dispatch the ready thread from this Task instead of waking the scheduler
   Task to do it.  Core lock held, baton already released.  TX_TRUE means it
   did not dispatch AND the scheduler is the only thing that can, so it has to
   be poked once the lock is dropped; the port's _tx_amiga_wake_needed() has
   the three failure cases and why two of them need no poke. */
extern UINT _tx_amiga_dispatch_or_wake(VOID);

/*
 * One entry per Exec Task currently inside a release/acquire bracket. The
 * hooks take no argument, so the thread pointer is stored in a table keyed by
 * struct Task *. The entry also records whether release() did anything, which
 * keeps acquire() from resuming a thread that was never suspended.
 *
 * Size covers the SANA-II readers (2, or 3 with IPv6) per interface, the IP
 * thread, the DHCP thread and any adopted application task that reaches the
 * driver.
 */
#define AMI_BATON_SLOTS     16

typedef struct AmiBatonSlot
{
    struct Task *bs_Task;
    TX_THREAD   *bs_Thread;
    ULONG        bs_Nesting;
} AmiBatonSlot;

static AmiBatonSlot ami_baton_slot[AMI_BATON_SLOTS];

/* Fields in aminetxduo/netstack.h. Touched only under the Forbid() the callers
   already hold. */
AmiBatonStats ami_baton_stats;

/*
 * The public anchor for those counters and for the tick task counters, so a
 * debugger on a frozen machine can find them without help from the stack.  The
 * reason and the three ways in are in include/aminetxduo/health.h.
 *
 * It points at the live counters rather than copying them, so there is nothing
 * to keep up to date and nothing to be stale at the moment it matters.
 */
static AmiHealthMark ami_health_mark;
static char          ami_health_name[] = AMI_HEALTH_NAME;
static BOOL          ami_health_up;

/* netstack_internal.h says why this is a pointer and not a call. */
static VOID (*ami_baton_sampler)(VOID);

VOID ami_netstack_baton_set_sampler(VOID (*fn)(VOID))
{
    Forbid();
    ami_baton_sampler = fn;
    Permit();
}

VOID ami_netstack_health_publish(VOID)
{
    if (ami_health_up)
        return;

    ami_health_mark.hm_Magic   = AMI_HEALTH_MAGIC;
    ami_health_mark.hm_Version = (UWORD)AMI_HEALTH_VERSION;
    ami_health_mark.hm_Size    = (UWORD)sizeof(AmiHealthMark);
    ami_health_mark.hm_Tick    = (APTR)tx_amiga_tick_stats_live();
    ami_health_mark.hm_Baton   = (APTR)&ami_baton_stats;
    ami_health_mark.hm_Mem     = (APTR)ami_mem_stats();

    InitSemaphore(&ami_health_mark.hm_Semaphore);
    ami_health_mark.hm_Semaphore.ss_Link.ln_Name = ami_health_name;
    ami_health_mark.hm_Semaphore.ss_Link.ln_Pri  = 0;

    /* Second stack on one machine: the first one's mark stays, and this one
       goes unpublished rather than giving FindSemaphore() two answers. */
    Forbid();
    if (FindSemaphore((STRPTR)ami_health_name) == NULL)
    {
        AddSemaphore(&ami_health_mark.hm_Semaphore);
        ami_health_up = TRUE;
    }
    Permit();
}

VOID ami_netstack_health_unpublish(VOID)
{
    if (!ami_health_up)
        return;

    /* Before the counters can go: a reader holds Forbid() across find and
       copy, so this cannot take the mark out from under one. */
    Forbid();
    RemSemaphore(&ami_health_mark.hm_Semaphore);
    ami_health_up = FALSE;
    Permit();
}

/* Callers hold Forbid() around both of these. */
static AmiBatonSlot *ami_baton_find(struct Task *task)
{
    UWORD i;

    for (i = 0; i < AMI_BATON_SLOTS; i++)
    {
        if (ami_baton_slot[i].bs_Task == task)
            return &ami_baton_slot[i];
    }

    return NULL;
}

static AmiBatonSlot *ami_baton_claim(struct Task *task)
{
    UWORD i;

    for (i = 0; i < AMI_BATON_SLOTS; i++)
    {
        if (ami_baton_slot[i].bs_Task == NULL)
        {
            ami_baton_slot[i].bs_Task    = task;
            ami_baton_slot[i].bs_Thread  = NULL;
            ami_baton_slot[i].bs_Nesting = 0;
            return &ami_baton_slot[i];
        }
    }

    return NULL;
}

/*
 * Forget a release/acquire bracket whose Exec Task cannot return.
 *
 * The TX_THREAD is the identity, not bs_Task. Exec can reuse a freed Task
 * address, while the cached TX_THREAD lives in its opener's retained library
 * base and is not reused. Matching the task would let a new task inherit the
 * dead one's slot; matching the thread can only remove the intended bracket.
 *
 * This only removes the netstack's record. The caller must follow it with
 * tx_amiga_discard_thread(), which unlinks the TX_THREAD from ThreadX's
 * created, ready and object-suspension lists. Keeping the operations separate
 * leaves this file independent of how the adopted thread's storage is owned.
 */
BOOL ami_netstack_baton_abandon(TX_THREAD *thread)
{
    BOOL  found = FALSE;
    UWORD i;

    if (thread == TX_NULL)
        return FALSE;

    Forbid();

    for (i = 0; i < AMI_BATON_SLOTS; i++)
    {
        AmiBatonSlot *slot = &ami_baton_slot[i];

        if (slot->bs_Thread != thread)
            continue;

        slot->bs_Task    = NULL;
        slot->bs_Thread  = NULL;
        slot->bs_Nesting = 0;

        if (ami_baton_stats.bs_Live > 0)
            ami_baton_stats.bs_Live--;

        found = TRUE;
        break;
    }

    Permit();

    return found;
}

/*
 * The table is a file static, so it outlives the stack it describes. Every
 * bs_Thread in it points into the NX_IP that ami_ns_destroy() has just freed.
 * A slot left behind by a Task that died mid-bracket keeps that pointer: the
 * next Task Exec puts at the same address inherits the slot, and its acquire()
 * hands the dangling TX_THREAD to tx_thread_resume().
 *
 * Called once tx_amiga_kernel_stop() has reported success, so no bracket can be
 * open and no thread exists to be tracked. That bounds the leak REENTRANCY.md
 * records to one stack lifetime instead of one seglist lifetime. It does not
 * fix the leak, which still needs a liveness test for a struct Task * that Exec
 * does not offer cheaply.
 */
VOID ami_netstack_baton_reset(VOID)
{
    UWORD i;
    UWORD held = 0;

    Forbid();

    for (i = 0; i < AMI_BATON_SLOTS; i++)
    {
        if (ami_baton_slot[i].bs_Task != NULL)
            held++;

        ami_baton_slot[i].bs_Task    = NULL;
        ami_baton_slot[i].bs_Thread  = NULL;
        ami_baton_slot[i].bs_Nesting = 0;
    }

    ami_baton_stats.bs_Live = 0;

    Permit();

    if (held != 0)
        AMI_WARN("netstack: %ld baton slot(s) still held at shutdown. A task "
                 "died inside a release/acquire bracket", (LONG)held);
}

/*
 * _tx_thread_system_state is one global counter and every Task reads it.  A
 * window in which it is raised with task switching enabled makes every other
 * Task look like an ISR.  A blocking ThreadX service entered on one comes back
 * without blocking, after it has already linked the caller into the suspension
 * list of the object and bumped the suspended count of the object.  The list
 * and the count then disagree, and the next _tx_event_flags_set() walks off the
 * end of the list into whatever offset 0x80 of address zero holds.
 *
 * So the counter must be raised only under an unbroken Forbid(), which is what
 * tx_thread_context_save.c and tx_amiga_adopt.c already do.  Nothing under
 * tx_thread_suspend()/tx_thread_resume() can Wait() while it is raised: every
 * _tx_thread_system_return() in tx_thread_system_{suspend,resume}.c is behind
 * TX_THREAD_SYSTEM_RETURN_CHECK, which tests exactly this counter.
 *
 * Sampled here, under Forbid() and before this bracket raises anything.  A
 * non-zero reading means another Task has it raised while this one runs, which
 * is the defect itself.  netstat -h reports both numbers.
 */
static VOID ami_baton_observe_state(VOID)
{
    ULONG state = (ULONG)_tx_thread_system_state;

    if (state > ami_baton_stats.bs_StateMax)
        ami_baton_stats.bs_StateMax = state;
    if (state != 0)
        ami_baton_stats.bs_StateShared++;

    /* A packet pool draining is the other thing that goes wrong here, and this
       is where the driver paths pass often enough to catch it. */
    if (ami_baton_sampler != NULL)
        ami_baton_sampler();
}

VOID ami_netstack_baton_release(VOID)
{
    struct Task   *me = FindTask(NULL);
    TX_THREAD     *thread;
    AmiBatonSlot  *slot;
    UINT           wake;

    Forbid();

    slot = ami_baton_find(me);
    if (slot != NULL && slot->bs_Nesting > 0)
    {
        /* Already released further up the call chain, so there is nothing to
           do. */
        slot->bs_Nesting++;
        Permit();
        return;
    }

    thread = _tx_thread_current_ptr;

    if (thread == TX_NULL || thread->tx_thread_amiga_task != (VOID *)me)
    {
        /*
         * Not the baton holder: either a plain Exec Task (startup, before the
         * kernel is up) or a thread that has already yielded. Blocking is
         * safe as it is.
         */
        Permit();
        return;
    }

    if (slot == NULL)
        slot = ami_baton_claim(me);

    if (slot == NULL)
    {
        /* Out of slots. Leave the thread running rather than suspend one that
           cannot be tracked, and warn. */
        ami_baton_stats.bs_Full++;
        Permit();
        AMI_WARN("netstack: baton table full. '%s' will block and hold "
                 "the baton",
                 (thread->tx_thread_name != TX_NULL) ? thread->tx_thread_name
                                                     : (CHAR *)"?");
        return;
    }

    ami_baton_stats.bs_Live++;
    if (ami_baton_stats.bs_Live > ami_baton_stats.bs_LiveMax)
        ami_baton_stats.bs_LiveMax = ami_baton_stats.bs_Live;
    ami_baton_observe_state();

    slot->bs_Thread  = thread;
    slot->bs_Nesting = 1;

    /* Interrupt context: _tx_thread_system_suspend() must not switch, because
       this Task is about to leave ThreadX entirely. */
    _tx_thread_system_state++;

    /* Off the ready list. With the system state raised this returns here
       rather than ending in _tx_thread_system_return(). The Forbid() is held
       across it, see the note above ami_baton_observe_state(). */
    (VOID)tx_thread_suspend(thread);

    if (_tx_thread_current_ptr == thread)
    {
        _tx_thread_current_ptr = TX_NULL;
        _tx_timer_time_slice   = (ULONG)0;
    }
    else
    {
        /* The baton belongs to another thread, so it stays pointing at a thread
           just suspended and the scheduler has nobody to dispatch. If this is
           ever non-zero after a freeze, that is the freeze. */
        ami_baton_stats.bs_BatonMoved++;
    }

    _tx_thread_system_state--;

    /*
     * Another thread can run now.  Dispatch it from here: this Task is about
     * to block in exec Wait(), so waking the scheduler Task runs the same five
     * stores and goes back to sleep, two Exec context switches for nothing.
     * Under the Forbid(), where the answer is valid.
     */
    wake =  _tx_amiga_dispatch_or_wake();

    Permit();

    if (wake == (UINT) TX_TRUE)
        _tx_amiga_wake_scheduler();
}

VOID ami_netstack_baton_acquire(VOID)
{
    struct Task   *me = FindTask(NULL);
    TX_THREAD     *thread;
    AmiBatonSlot  *slot;
    UINT           wake;

    Forbid();

    slot = ami_baton_find(me);
    if (slot == NULL || slot->bs_Nesting == 0)
    {
        Permit();
        return;
    }

    slot->bs_Nesting--;
    if (slot->bs_Nesting > 0)
    {
        Permit();
        return;
    }

    thread          = slot->bs_Thread;
    slot->bs_Thread = NULL;
    slot->bs_Task   = NULL;

    if (thread == TX_NULL)
    {
        Permit();
        return;
    }

    ami_baton_observe_state();

    _tx_thread_system_state++;
    if (ami_baton_stats.bs_Live > 0)
        ami_baton_stats.bs_Live--;
    ami_baton_stats.bs_Transitions++;

    /* Held across the resume, same rule as release(). */
    (VOID)tx_thread_resume(thread);

    _tx_thread_system_state--;

    /*
     * Hand the baton over from here rather than waking the scheduler Task to
     * do it.  This is the thread that is about to run, so the usual case is
     * that _tx_thread_execute_ptr is already this one and the dispatch is the
     * five stores in _tx_amiga_dispatch_inline(): the park below then finds
     * its run signal already set and returns without blocking.  Going through
     * the scheduler costs two Exec context switches instead, which is what
     * every other handoff in the port stopped paying (docs/RESEARCH.md 89).
     *
     * Under the Forbid(), because the answer is only good while it is held.
     */
    wake =  _tx_amiga_dispatch_or_wake();

    Permit();

    if (wake == (UINT) TX_TRUE)
        _tx_amiga_wake_scheduler();

    /* Block until the scheduler makes this thread _tx_thread_current_ptr
       again. */
    (VOID)_tx_amiga_thread_park(thread);
}
