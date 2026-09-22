/*
 * AmiNetXDuo, releasing and reacquiring the ThreadX baton around an exec
 * Wait().  Blocking in Exec while holding the baton stalls the whole stack,
 * and the thread must leave the ready list too or the scheduler re-picks it.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_internal.h"

#include "tx_amiga.h"

/* The port's live baton-holder word (port/threadx-amiga/src/tx_amiga_internal.h).
   Declared here rather than pulled in from that private header, which wants
   TX_SOURCE_CODE. */
extern VOID *_tx_amiga_baton_holder_task;

#include "aminetxduo/health.h"

#include <exec/tasks.h>
#include <proto/exec.h>

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
    ami_health_mark.hm_Holder  = (APTR)&_tx_amiga_baton_holder_task;
    /* hm_SbLock is bsdsocket's and is set by
       ami_netstack_health_set_sblock(); it must survive this initialisation. */

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

/*
 * bsdsocket hands its master lock to the mark so a diagnostic reader can see
 * who owns it.  Set once, when the master base is built; cleared with it.  The
 * mark may not be published yet, so the value is kept either way.
 */
VOID ami_netstack_health_set_sblock(APTR sem)
{
    Forbid();
    ami_health_mark.hm_SbLock = sem;
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

/*
 * Forget a release/acquire bracket whose Exec Task cannot return.  The
 * bracket belongs to the TX_THREAD itself, so deleting that thread also
 * deletes its identity; no recycled Exec Task address can inherit it.  The
 * caller must follow this with tx_amiga_discard_thread().
 */
BOOL ami_netstack_baton_abandon(TX_THREAD *thread)
{
    BOOL found = FALSE;

    if (thread == TX_NULL)
        return FALSE;

    Forbid();

    if (thread->tx_thread_amiga_exec_wait_nesting != 0U)
    {
        thread->tx_thread_amiga_exec_wait_nesting = 0U;

        if (ami_baton_stats.bs_Live > 0)
            ami_baton_stats.bs_Live--;

        found = TRUE;
    }

    Permit();

    return found;
}

/*
 * The lock-free half of dead-task recovery, from the tick.  A Task removed by
 * Exec while it held the baton never gives it back, and the locked sweep that
 * would notice needs sb_Lock, which a caller waiting for that baton may hold.
 * So the holder is checked here first, and if its Task is gone the TX_THREAD is
 * discarded, which takes the baton back.  Only the TX_THREAD is touched: the
 * caller record and the library base belong to the sweep.
 *
 * Runs on the tick task inside Forbid() with the interrupt state raised, and
 * tx_amiga_discard_thread() is built for that context.
 */
BOOL ami_netstack_baton_reclaim_dead(VOID)
{
    TX_THREAD *holder;
    ULONG      generation;
    UINT       status;

    Forbid();

    holder = tx_amiga_exec_wait_current_locked();
    if (holder == TX_NULL || tx_amiga_adopted_task_dead(holder) == TX_FALSE)
    {
        Permit();
        return FALSE;
    }

    /* The holder's own handle, read from the pool rather than from any caller
       record: the AmiNetCaller that adopted this thread may be on the dead
       Task's stack, which Exec has already freed. */
    generation = tx_amiga_adopt_generation(holder);

    /* A holder has no released bracket, so this is a no-op; it keeps the
       abandon-before-discard order every other discard follows. */
    (VOID)ami_netstack_baton_abandon(holder);
    status = tx_amiga_discard_thread(holder, generation);

    /* Counted on the teardown, not on the diagnosis: a discard that did not
       report TX_SUCCESS took no baton back and left nothing to count. */
    if (status == TX_SUCCESS)
        ami_baton_stats.bs_Reclaimed++;

    Permit();

    if (status != TX_SUCCESS)
    {
        AMI_WARN("netstack: dead baton holder was not discarded (%ld)",
                 (LONG)status);
        return FALSE;
    }

    return TRUE;
}


/*
 * Called once tx_amiga_kernel_stop() has reported success, so no bracket can
 * be open and no TX_THREAD carrying bracket state still exists.  Only the
 * diagnostic live count outlives the kernel now.
 */
VOID ami_netstack_baton_reset(VOID)
{
    ULONG held;

    Forbid();
    held = ami_baton_stats.bs_Live;
    ami_baton_stats.bs_Live = 0;
    Permit();

    if (held != 0)
        AMI_WARN("netstack: %ld baton bracket(s) still held at shutdown. A task "
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
    UINT           wake;
    UINT           moved;
    UINT           status;

    Forbid();

    thread = tx_amiga_exec_wait_owner_locked();
    if (thread != TX_NULL &&
        thread->tx_thread_amiga_exec_wait_nesting != 0U)
    {
        thread->tx_thread_amiga_exec_wait_nesting++;
        Permit();
        return;
    }

    if (thread == TX_NULL || thread != tx_amiga_exec_wait_current_locked() ||
        thread->tx_thread_amiga_task != (VOID *)me)
    {
        /* Not the baton holder: either a plain Exec Task or a thread that has
           already yielded.  Blocking is safe as it is. */
        Permit();
        return;
    }

    ami_baton_stats.bs_Live++;
    if (ami_baton_stats.bs_Live > ami_baton_stats.bs_LiveMax)
        ami_baton_stats.bs_LiveMax = ami_baton_stats.bs_Live;
    ami_baton_observe_state();

    thread->tx_thread_amiga_exec_wait_nesting = 1U;

    /* The port owns the ready-list and baton globals.  This call is the one
       narrow boundary for suspending without dispatching through Exec; the
       surrounding Forbid() keeps it atomic with the slot published above. */
    status = tx_amiga_exec_wait_release_locked(thread, &wake, &moved);
    if (status != TX_SUCCESS)
    {
        thread->tx_thread_amiga_exec_wait_nesting = 0U;
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
    TX_THREAD     *thread;
    UINT           wake;
    UINT           status;

    Forbid();

    thread = tx_amiga_exec_wait_owner_locked();
    if (thread == TX_NULL ||
        thread->tx_thread_amiga_exec_wait_nesting == 0U)
    {
        Permit();
        return;
    }

    thread->tx_thread_amiga_exec_wait_nesting--;
    if (thread->tx_thread_amiga_exec_wait_nesting != 0U)
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
