/*
 * AmiNetXDuo, releasing and reacquiring the ThreadX baton around an exec
 * Wait().  Blocking in Exec while holding the baton stalls the whole stack,
 * and the thread must leave the ready list too or the scheduler re-picks it.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_internal.h"

#include "tx_amiga.h"

#include "aminetxduo/health.h"

#include <exec/tasks.h>
#include <proto/exec.h>

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
 * The port's scheduler state must be zero before a wait bracket begins.
 * Sampled before the port raises anything; non-zero is the defect itself.
 */
static VOID ami_baton_observe_state(VOID)
{
    ULONG state = tx_amiga_exec_wait_system_state_locked();

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
    UINT           moved;
    UINT           status;

    Forbid();

    slot = ami_baton_find(me);
    if (slot != NULL && slot->bs_Nesting > 0)
    {
        slot->bs_Nesting++;
        Permit();
        return;
    }

    thread = tx_amiga_exec_wait_current_locked();

    if (thread == TX_NULL || thread->tx_thread_amiga_task != (VOID *)me)
    {
        /* Not the baton holder: either a plain Exec Task or a thread that has
           already yielded.  Blocking is safe as it is. */
        Permit();
        return;
    }


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

    /* The port owns the ready-list and baton globals.  This call is the one
       narrow boundary for suspending without dispatching through Exec; the
       surrounding Forbid() keeps it atomic with the slot published above. */
    status = tx_amiga_exec_wait_release_locked(thread, &wake, &moved);
    if (status != TX_SUCCESS)
    {
        slot->bs_Task    = NULL;
        slot->bs_Thread  = NULL;
        slot->bs_Nesting = 0;
        if (ami_baton_stats.bs_Live > 0)
            ami_baton_stats.bs_Live--;
        Permit();
        AMI_ERROR("netstack: a thread would not suspend to release the baton");
        return;
    }
    if (moved != TX_FALSE)
    {
        /* The baton belongs to another thread, so it stays pointing at a thread
           just suspended and the scheduler has nobody to dispatch. If this is
           ever non-zero after a freeze, that is the freeze. */
        ami_baton_stats.bs_BatonMoved++;
    }

    Permit();

    if (wake == (UINT) TX_TRUE)
        tx_amiga_exec_wait_wake();
}

VOID ami_netstack_baton_acquire(VOID)
{
    struct Task   *me = FindTask(NULL);
    TX_THREAD     *thread;
    AmiBatonSlot  *slot;
    UINT           wake;
    UINT           status;

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

    if (ami_baton_stats.bs_Live > 0)
        ami_baton_stats.bs_Live--;
    ami_baton_stats.bs_Transitions++;

    /* Held across the resume, same rule as release().  The port owns the
       scheduler state that keeps this from dispatching mid-transaction. */
    status = tx_amiga_exec_wait_resume_locked(thread, &wake);
    if (status != TX_SUCCESS)
    {
        Permit();
        AMI_ERROR("netstack: a thread waiting for the baton was not resumed");
        return;
    }

    Permit();

    if (wake == (UINT) TX_TRUE)
        tx_amiga_exec_wait_wake();

    /*
     * OPTIONAL.  Park answers TX_FALSE when the Task was marked to die while
     * it waited for the baton: it is no longer a ThreadX thread, and this
     * function has already handed the baton on, so the caller returns without
     * one.  Nothing above can be undone from here -- the slot is released and
     * the resume has happened -- so this says what was lost rather than
     * pretending to recover.
     */
    if (tx_amiga_exec_wait_park(thread) == (UINT)TX_FALSE)
        AMI_ERROR("netstack: a task waiting for the baton was orphaned; "
                  "it continues without one");
}
