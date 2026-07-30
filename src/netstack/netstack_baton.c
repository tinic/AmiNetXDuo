/*
 * AmiNetXDuo -- releasing and reacquiring the ThreadX baton around an exec
 * Wait().
 *
 * The port uses the baton model (docs/RESEARCH.md 6.2): a ThreadX thread runs
 * only if it is _tx_thread_current_ptr, and _tx_thread_schedule() will not
 * dispatch anyone while that pointer is non-NULL. The SANA-II reader threads,
 * and every driver control command, block in exec Wait() for an IORequest,
 * which ThreadX knows nothing about. Doing so while holding the baton stalls
 * the whole stack: the IP thread, the timer thread and every other socket user
 * queue behind a task that will not become runnable until a packet arrives.
 *
 * Clearing _tx_thread_current_ptr alone moves the deadlock one step later: the
 * thread is still ready, so the scheduler picks it straight back (the readers
 * are the highest-priority threads in the system), sets _tx_thread_current_ptr
 * again and pokes a run signal the thread is not waiting for. The thread must
 * come off the ready list too.
 *
 * release():  suspend the calling thread as tx_thread_suspend() does, but with
 *             _tx_thread_system_state raised so ThreadX treats it as interrupt
 *             context and does not context-switch on our behalf; then clear
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

#include <exec/tasks.h>
#include <proto/exec.h>

/* Port internals (port/threadx-amiga/src/). Not in tx_amiga.h because they are
   not application API, but a blocking bracket needs them. */
extern VOID _tx_amiga_wake_scheduler(VOID);
extern UINT _tx_amiga_thread_park(TX_THREAD *thread_ptr);

/*
 * One entry per Exec Task currently inside a release/acquire bracket. The
 * hooks take no argument, so the thread pointer is stored in a table keyed by
 * struct Task *; the entry also records whether release() did anything, which
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

/*
 * What the bracket actually did, for the freeze hunt.  A hard lockup leaves no
 * Enforcer hit and no log, so the numbers have to be readable from outside
 * afterwards rather than printed as they happen.  All of these are touched
 * only under the Forbid() the callers already hold.
 *
 *   bs_Live / bs_LiveMax   tasks inside a bracket now, and the most ever
 *   bs_Full                times the table had no slot (the fail-open path)
 *   bs_Transitions         release/acquire pairs completed
 *   bs_StateMax            highest _tx_thread_system_state seen on entry
 *   bs_BatonMoved          times release() found the baton was not ours
 */
AmiBatonStats ami_baton_stats;

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

VOID ami_netstack_baton_release(VOID)
{
    struct Task   *me = FindTask(NULL);
    TX_THREAD     *thread;
    AmiBatonSlot  *slot;

    Forbid();

    slot = ami_baton_find(me);
    if (slot != NULL && slot->bs_Nesting > 0)
    {
        /* Already released further up the call chain; nothing to do. */
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
        /* Out of slots. Leave the thread running rather than suspend one we
           cannot track, and warn. */
        ami_baton_stats.bs_Full++;
        Permit();
        AMI_WARN("netstack: baton table full; '%s' will block holding the baton",
                 (thread->tx_thread_name != TX_NULL) ? thread->tx_thread_name
                                                     : (CHAR *)"?");
        return;
    }

    ami_baton_stats.bs_Live++;
    if (ami_baton_stats.bs_Live > ami_baton_stats.bs_LiveMax)
        ami_baton_stats.bs_LiveMax = ami_baton_stats.bs_Live;
    if ((ULONG)_tx_thread_system_state > ami_baton_stats.bs_StateMax)
        ami_baton_stats.bs_StateMax = (ULONG)_tx_thread_system_state;

    slot->bs_Thread  = thread;
    slot->bs_Nesting = 1;

    /* Interrupt context: _tx_thread_system_suspend() must not try to switch
       for us -- we are about to leave ThreadX entirely. */
    _tx_thread_system_state++;

    Permit();

    /* Off the ready list. With the system state raised this returns here
       rather than ending in _tx_thread_system_return(). */
    (VOID)tx_thread_suspend(thread);

    Forbid();

    if (_tx_thread_current_ptr == thread)
    {
        _tx_thread_current_ptr = TX_NULL;
        _tx_timer_time_slice   = (ULONG)0;
    }
    else
    {
        /* The baton is not ours to clear, so it stays pointing at a thread we
           have just suspended and the scheduler has nobody to dispatch. If this
           is ever non-zero after a freeze, that is the freeze. */
        ami_baton_stats.bs_BatonMoved++;
    }

    _tx_thread_system_state--;

    Permit();

    /* Somebody else may run now. */
    _tx_amiga_wake_scheduler();
}

VOID ami_netstack_baton_acquire(VOID)
{
    struct Task   *me = FindTask(NULL);
    TX_THREAD     *thread;
    AmiBatonSlot  *slot;

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

    _tx_thread_system_state++;
    if (ami_baton_stats.bs_Live > 0)
        ami_baton_stats.bs_Live--;
    ami_baton_stats.bs_Transitions++;

    Permit();

    (VOID)tx_thread_resume(thread);

    Forbid();
    _tx_thread_system_state--;
    Permit();

    _tx_amiga_wake_scheduler();

    /* Block until the scheduler makes us _tx_thread_current_ptr again. */
    (VOID)_tx_amiga_thread_park(thread);
}
