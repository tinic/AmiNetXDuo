/***************************************************************************
 * Eclipse ThreadX, AmigaOS/m68k port: release the hosted scheduler baton
 * around an Exec wait.
 *
 * SPDX-License-Identifier: MIT
 **************************************************************************/

#define TX_SOURCE_CODE

#include "tx_amiga_internal.h"

/* This is intentionally the only public observation of the core's interrupt
   state.  The netstack records a non-zero value as a broken bracket invariant;
   it no longer binds the ThreadX global itself. */
ULONG tx_amiga_exec_wait_system_state_locked(VOID)
{
    return (ULONG)_tx_thread_system_state;
}

TX_THREAD *tx_amiga_exec_wait_current_locked(VOID)
{
    return _tx_thread_current_ptr;
}

UINT tx_amiga_exec_wait_release_locked(TX_THREAD *thread_ptr,
                                        UINT *wake, UINT *moved)
{
    UINT status;

    if (thread_ptr == TX_NULL || wake == TX_NULL || moved == TX_NULL)
        return TX_PTR_ERROR;

    *wake  = TX_FALSE;
    *moved = TX_FALSE;

    /* Raising system_state makes tx_thread_suspend() unlink the thread and
       return here instead of entering _tx_thread_system_return(). */
    _tx_thread_system_state++;
    status = _tx_thread_suspend(thread_ptr);

    if (status == TX_SUCCESS)
    {
        if (_tx_thread_current_ptr == thread_ptr)
        {
            ami_budget_hold_end((APTR)thread_ptr, thread_ptr->tx_thread_name,
                                (ULONG)thread_ptr->tx_thread_state,
                                AMI_HOLD_SITE_BRACKET);
            _tx_thread_current_ptr = TX_NULL;
            _tx_timer_time_slice   = (ULONG)0;
        }
        else
            *moved = TX_TRUE;
    }

    _tx_thread_system_state--;

    if (status == TX_SUCCESS)
        *wake = _tx_amiga_dispatch_or_wake();

    return status;
}

UINT tx_amiga_exec_wait_resume_locked(TX_THREAD *thread_ptr, UINT *wake)
{
    UINT status;

    if (thread_ptr == TX_NULL || wake == TX_NULL)
        return TX_PTR_ERROR;

    *wake = TX_FALSE;
    _tx_thread_system_state++;
    status = _tx_thread_resume(thread_ptr);
    _tx_thread_system_state--;

    if (status == TX_SUCCESS)
        *wake = _tx_amiga_dispatch_or_wake();

    return status;
}

VOID tx_amiga_exec_wait_wake(VOID)
{
    _tx_amiga_wake_scheduler();
}

UINT tx_amiga_exec_wait_park(TX_THREAD *thread_ptr)
{
    if (thread_ptr == TX_NULL)
        return TX_FALSE;

    return _tx_amiga_thread_park(thread_ptr);
}
