/*
 * Bridge an arbitrary Exec task into ThreadX for one stack call, including
 * the cached form used by long-lived clients.
 * SPDX-License-Identifier: MIT
 */

#include "netstack_internal.h"
#include "tx_amiga.h"
#include "aminetxduo/nxstatus.h"

#include <exec/memory.h>
#include <proto/exec.h>

LONG ami_netstack_enter(AmiNetCaller *caller)
{
    UINT status;

    caller->nc_Adopted = FALSE;
    caller->nc_Thread  = TX_NULL;
    caller->nc_Gen     = 0UL;

    if (tx_amiga_kernel_running() != TX_TRUE)
        return AMI_NET_ERR_STATE;

    if (tx_amiga_caller_is_thread() != (UINT)TX_FALSE)
        return AMI_NET_OK;

    /* The pool's reserved slot is for a caller that must not park, and in this
       tree that is exactly one: a Task inside ami_ns_lock.  Asked here rather
       than passed in by each call site, so no site can forget and none of it
       depends on whether the bracket is cached. */
    status = tx_amiga_adopt_thread(&caller->nc_Thread, &caller->nc_Gen,
                                   (CHAR *)"AmiNetXDuo caller",
                                   AMI_CALLER_PRIORITY,
                                   ami_ns_lock_held_by_me() ? (UINT)TX_TRUE
                                                            : (UINT)TX_FALSE);
    if (status != TX_SUCCESS)
    {
        AMI_ERROR("netstack: cannot adopt calling task (%ld)", (long)status);
        return AMI_NET_ERR_KERNEL;
    }

    caller->nc_Adopted = TRUE;
    return AMI_NET_OK;
}

VOID ami_netstack_leave(AmiNetCaller *caller)
{
    netstack_pool_mark_low();

    if (caller->nc_Adopted)
    {
        AMI_NX_CLEANUP(tx_amiga_orphan_thread(caller->nc_Thread, caller->nc_Gen));
        caller->nc_Adopted = FALSE;
        caller->nc_Thread  = TX_NULL;
        caller->nc_Gen     = 0UL;
    }
}

AmiNetCaller *ami_netstack_enter_alloc(VOID)
{
    AmiNetCaller *caller = (AmiNetCaller *)AllocMem(sizeof(AmiNetCaller),
                                                    MEMF_PUBLIC | MEMF_CLEAR);

    if (caller == NULL)
        return NULL;

    AMI_CENSUS_ADD(caller, sizeof(AmiNetCaller));

    if (ami_netstack_enter(caller) != AMI_NET_OK)
    {
        AMI_CENSUS_DROP(caller);
        FreeMem(caller, sizeof(AmiNetCaller));
        return NULL;
    }

    return caller;
}

VOID ami_netstack_leave_free(AmiNetCaller *caller)
{
    if (caller == NULL)
        return;

    ami_netstack_leave(caller);
    AMI_CENSUS_DROP(caller);
    FreeMem(caller, sizeof(AmiNetCaller));
}

/* A full pool took the cached thread back (tx_amiga_pool.c): the teardown
   was foreign, so the run signal it allocated here is still ours to free. */
static VOID nc_free_evicted_signal(AmiNetCaller *caller)
{
    tx_amiga_adopt_signal_free(caller->nc_Signal);
}

LONG ami_netstack_enter_cached(AmiNetCaller *caller)
{
    struct Task *me;
    UINT         status;

    caller->nc_Adopted = FALSE;

    if (tx_amiga_kernel_running() != TX_TRUE)
        return AMI_NET_ERR_STATE;

    if (tx_amiga_caller_is_thread() != (UINT)TX_FALSE)
        return AMI_NET_OK;

    me = FindTask(NULL);

    if (caller->nc_Live && caller->nc_Task == me)
    {
        status = tx_amiga_adopt_resume(caller->nc_Thread, caller->nc_Gen);
        if (status == TX_SUCCESS)
        {
            caller->nc_Adopted = TRUE;
            return AMI_NET_OK;
        }

        if (status == TX_THREAD_ERROR)
            nc_free_evicted_signal(caller);

        /* Either way the handle is finished with. Both calls check the
           generation themselves, so a slot recycled under us is refused rather
           than torn down; the fresh adoption below is then the recovery. */
        if (tx_amiga_orphan_thread(caller->nc_Thread, caller->nc_Gen) != TX_SUCCESS)
            AMI_NX_CLEANUP(tx_amiga_discard_thread(caller->nc_Thread, caller->nc_Gen));
        caller->nc_Live   = FALSE;
        caller->nc_Task   = NULL;
        caller->nc_Thread = TX_NULL;
        caller->nc_Gen    = 0UL;
    }

    if (caller->nc_Live)
    {
        AMI_ERROR("netstack: bracket used from a second task");
        return AMI_NET_ERR_STATE;
    }

    /* Publish the owner before adoption: a foreign RemTask() can run as soon
       as tx_amiga_adopt_thread() releases Forbid(). */
    caller->nc_Live = TRUE;
    caller->nc_Task = me;

    /* Same question as ami_netstack_enter(): the reserve follows who holds
       ami_ns_lock, not whether the bracket is cached. */
    status = tx_amiga_adopt_thread(&caller->nc_Thread, &caller->nc_Gen,
                                   (CHAR *)"AmiNetXDuo caller",
                                   AMI_CALLER_PRIORITY,
                                   ami_ns_lock_held_by_me() ? (UINT)TX_TRUE
                                                            : (UINT)TX_FALSE);
    if (status != TX_SUCCESS)
    {
        caller->nc_Live = FALSE;
        caller->nc_Task = NULL;
        AMI_ERROR("netstack: cannot adopt calling task (%ld)", (long)status);
        return AMI_NET_ERR_KERNEL;
    }

    caller->nc_Signal  = tx_amiga_adopt_signal(caller->nc_Thread);
    caller->nc_Adopted = TRUE;
    return AMI_NET_OK;
}

VOID ami_netstack_leave_cached(AmiNetCaller *caller)
{
    netstack_pool_mark_low();

    if (!caller->nc_Adopted)
        return;

    caller->nc_Adopted = FALSE;

    if (caller->nc_Live && caller->nc_Task == FindTask(NULL))
    {
        if (tx_amiga_adopt_suspend(caller->nc_Thread, caller->nc_Gen) == TX_SUCCESS)
            return;

        caller->nc_Live = FALSE;
        caller->nc_Task = NULL;
    }

    AMI_NX_CLEANUP(tx_amiga_orphan_thread(caller->nc_Thread, caller->nc_Gen));
    caller->nc_Thread = TX_NULL;
    caller->nc_Gen    = 0UL;
}

VOID ami_netstack_release(AmiNetCaller *caller)
{
    struct Task *me;
    UINT         status;

    if (caller == NULL || !caller->nc_Live)
        return;

    me = FindTask(NULL);

    /* Release is a teardown call and must not be reached from inside a
       bracket, so drop the baton the ordinary way first. */
    if (caller->nc_Adopted && caller->nc_Task == me)
    {
        caller->nc_Adopted = FALSE;
        AMI_NX_CLEANUP(tx_amiga_orphan_thread(caller->nc_Thread, caller->nc_Gen));
        caller->nc_Live   = FALSE;
        caller->nc_Task   = NULL;
        caller->nc_Thread = TX_NULL;
        caller->nc_Gen    = 0UL;
        return;
    }

    if (caller->nc_Task == me)
    {
        status = tx_amiga_adopt_resume(caller->nc_Thread, caller->nc_Gen);
        if (status == TX_SUCCESS)
            AMI_NX_CLEANUP(tx_amiga_orphan_thread(caller->nc_Thread, caller->nc_Gen));
        else if (status == TX_THREAD_ERROR)
            nc_free_evicted_signal(caller);
        else
            AMI_NX_CLEANUP(tx_amiga_discard_thread(caller->nc_Thread, caller->nc_Gen));
    }
    else
    {
        /*
         * The sweep's path: the Task this record belongs to is gone. Its
         * TX_THREAD may already have been reclaimed from the tick and the slot
         * handed to somebody else, so the handle is checked and everything that
         * touches the TX_THREAD happens inside ONE Forbid(): a slot is only
         * ever released under Forbid(), so it cannot be recycled between the
         * check and the discard.
         */
        Forbid();
        if (tx_amiga_adopt_handle_valid(caller->nc_Thread, caller->nc_Gen)
            != (UINT)TX_FALSE)
        {
            (VOID)ami_netstack_baton_abandon(caller->nc_Thread);
            status = tx_amiga_discard_thread(caller->nc_Thread, caller->nc_Gen);
        }
        else
        {
            status = TX_SUCCESS;        /* already reclaimed; nothing to do */
        }
        Permit();

        if (status != TX_SUCCESS && status != TX_THREAD_ERROR)
        {
            AMI_WARN("netstack: cannot discard dead task's ThreadX context "
                     "(%ld)", (LONG)status);
        }
    }

    caller->nc_Adopted = FALSE;
    caller->nc_Live    = FALSE;
    caller->nc_Task    = NULL;
    caller->nc_Thread  = TX_NULL;
    caller->nc_Gen     = 0UL;
    caller->nc_Signal  = 0UL;
}
