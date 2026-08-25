/*
 * AmiNetXDuo, releasing and reacquiring the ThreadX baton around an exec
 * Wait().  Blocking in Exec while holding the baton stalls the whole stack,
 * and the thread must leave the ready list too or the scheduler re-picks it.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_internal.h"

#include "tx_thread.h"
#include "tx_timer.h"
#include "tx_amiga.h"

#include "aminetxduo/budget.h"
#include "aminetxduo/health.h"

#include <exec/tasks.h>
#include <proto/exec.h>

/* Port internals (port/threadx-amiga/src/). Not in tx_amiga.h because they are
   not application API, but a blocking bracket needs them. */
extern VOID _tx_amiga_wake_scheduler(VOID);
extern UINT _tx_amiga_thread_park(TX_THREAD *thread_ptr);

/* Dispatch the ready thread from this Task instead of waking the scheduler
   Task to do it.  Core lock held, baton already released.  TX_TRUE means it
   did not dispatch and the scheduler must be poked once the lock is dropped. */
extern UINT _tx_amiga_dispatch_or_wake(VOID);

/*
 * One entry per Exec Task currently inside a release/acquire bracket.  The
 * hooks take no argument, so the thread pointer is stored in a table keyed by
 * struct Task *.
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
 * The public anchor for those counters and for the tick task counters.  It
 * points at the live counters rather than copying them, so there is nothing to
 * be stale at the moment it matters.  include/aminetxduo/health.h has why.
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
 * Forget a release/acquire bracket whose Exec Task cannot return.  The
 * TX_THREAD is the identity, not bs_Task: Exec can reuse a freed Task address.
 * The caller must follow this with tx_amiga_discard_thread().
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
 * Called once tx_amiga_kernel_stop() has reported success, so no bracket can
 * be open and no thread exists to be tracked.  The table is a file static and
 * outlives the stack, and every bs_Thread in it points into a freed NX_IP.
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
 * _tx_thread_system_state must be raised only under an unbroken Forbid(), and
 * nothing under tx_thread_suspend()/resume() may Wait() while it is raised.
 * Sampled before this bracket raises anything; non-zero is the defect itself.
 */
static VOID ami_baton_observe_state(VOID)
{
    ULONG state = (ULONG)_tx_thread_system_state;

    if (state > ami_baton_stats.bs_StateMax)
        ami_baton_stats.bs_StateMax = state;
    if (state != 0)
        ami_baton_stats.bs_StateShared++;

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
        slot->bs_Nesting++;
        Permit();
        return;
    }

    thread = _tx_thread_current_ptr;

    if (thread == TX_NULL || thread->tx_thread_amiga_task != (VOID *)me)
    {
        /* Not the baton holder: either a plain Exec Task or a thread that has
           already yielded.  Blocking is safe as it is. */
        Permit();
        return;
    }

#ifdef AMINETXDUO_GREEN_REALM
    if ((thread->tx_thread_amiga_flags & TX_AMIGA_THREAD_GREEN) != 0U)
    {
        /*
         * Green code must sleep in tx_amiga_green_wait(), never around an Exec
         * Wait(): suspending it here would strand its context, because the
         * bracket's Wait() runs on the REALM Task.  This counter must be zero.
         */
        tx_amiga_green_stray_wait_note();
        Permit();
        AMI_WARN("green realm: baton bracket entered from green thread '%s'. "
                 "An unconverted Exec-blocking site is still in the realm",
                 (thread->tx_thread_name != TX_NULL) ? thread->tx_thread_name
                                                     : (CHAR *)"?");
        return;
    }
#endif

    if (slot == NULL)
        slot = ami_baton_claim(me);

    if (slot == NULL)
    {
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
    /* Off the ready list.  With the system state raised this returns here
       rather than ending in _tx_thread_system_return().  The Forbid() is held
       across it, see the note above ami_baton_observe_state(). */
    (VOID)tx_thread_suspend(thread);

    if (_tx_thread_current_ptr == thread)
    {
        /* The hold that ends here ends at a driver bracket: the holder is
           about to Wait() on an IORequest outside ThreadX entirely. */
        ami_budget_hold_end((APTR)thread, thread->tx_thread_name,
                            (ULONG)thread->tx_thread_state,
                            AMI_HOLD_SITE_BRACKET);
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
     * to block in exec Wait(), so waking the scheduler Task is two Exec
     * context switches for nothing.  Under the Forbid(), where it is valid.
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
     * Hand the baton over from here rather than waking the scheduler Task: the
     * park below then usually finds its run signal already set and returns
     * without blocking.  Under the Forbid(), where the answer is valid.
     */
    wake =  _tx_amiga_dispatch_or_wake();

    Permit();

    if (wake == (UINT) TX_TRUE)
        _tx_amiga_wake_scheduler();

    (VOID)_tx_amiga_thread_park(thread);
}

