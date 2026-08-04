/*
 * AmiNetXDuo, the ThreadX thread and timer surface fuzz_dhcp stands on.
 *
 * fuzz_nxstub.c covers the mutex and event-flag calls the DNS and mDNS
 * parsers reach. The DHCP client is a different shape: it owns a thread and a
 * timer and creates both in nx_dhcp_create(), so the whole thread/timer
 * surface has to resolve even though the driver never calls it, fd_run()
 * enters the option parser directly, below all of this.
 *
 * Every function here returns TX_SUCCESS and does nothing. That is safe only
 * because nothing runs: a stub that fired would mean the driver had wandered
 * out of the parser and into the client's state machine, and the sweep would
 * be testing something other than the parse. If one of these ever needs real
 * behaviour, the driver is wrong, not the stub.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tx_api.h"
#include "tx_thread.h"
#include "tx_timer.h"

/* The globals ThreadX would own. _tx_thread_current_ptr NULL reads as "no
   thread is running", which is true here. */
TX_THREAD       *_tx_thread_current_ptr;
volatile ULONG   _tx_thread_system_state;
volatile UINT    _tx_thread_preempt_disable;
TX_THREAD        _tx_timer_thread;

UINT _tx_thread_create(TX_THREAD *thread_ptr, CHAR *name_ptr,
                       VOID (*entry_function)(ULONG entry_input),
                       ULONG entry_input, VOID *stack_start, ULONG stack_size,
                       UINT priority, UINT preempt_threshold,
                       ULONG time_slice, UINT auto_start)
{
    (VOID)thread_ptr; (VOID)name_ptr; (VOID)entry_function;
    (VOID)entry_input; (VOID)stack_start; (VOID)stack_size;
    (VOID)priority; (VOID)preempt_threshold; (VOID)time_slice;
    (VOID)auto_start;
    return TX_SUCCESS;
}

UINT _tx_thread_delete(TX_THREAD *thread_ptr)
{
    (VOID)thread_ptr;
    return TX_SUCCESS;
}

UINT _tx_thread_resume(TX_THREAD *thread_ptr)
{
    (VOID)thread_ptr;
    return TX_SUCCESS;
}

UINT _tx_thread_suspend(TX_THREAD *thread_ptr)
{
    (VOID)thread_ptr;
    return TX_SUCCESS;
}

UINT _tx_thread_terminate(TX_THREAD *thread_ptr)
{
    (VOID)thread_ptr;
    return TX_SUCCESS;
}

UINT _tx_thread_sleep(ULONG timer_ticks)
{
    (VOID)timer_ticks;
    return TX_SUCCESS;
}

UINT _tx_thread_preemption_change(TX_THREAD *thread_ptr, UINT new_threshold,
                                  UINT *old_threshold)
{
    (VOID)thread_ptr; (VOID)new_threshold;
    if (old_threshold != TX_NULL)
        *old_threshold = 0;
    return TX_SUCCESS;
}

TX_THREAD *_tx_thread_identify(VOID)
{
    return TX_NULL;
}

VOID _tx_thread_system_preempt_check(VOID)
{
}

VOID _tx_thread_system_resume(TX_THREAD *thread_ptr)
{
    (VOID)thread_ptr;
}

VOID _tx_thread_system_suspend(TX_THREAD *thread_ptr)
{
    (VOID)thread_ptr;
}

UINT _tx_thread_interrupt_disable(VOID)
{
    return 0;
}

VOID _tx_thread_interrupt_restore(UINT previous_posture)
{
    (VOID)previous_posture;
}

UINT _tx_event_flags_create(TX_EVENT_FLAGS_GROUP *group_ptr, CHAR *name_ptr)
{
    (VOID)group_ptr; (VOID)name_ptr;
    return TX_SUCCESS;
}

UINT _tx_event_flags_delete(TX_EVENT_FLAGS_GROUP *group_ptr)
{
    (VOID)group_ptr;
    return TX_SUCCESS;
}

UINT _tx_timer_create(TX_TIMER *timer_ptr, CHAR *name_ptr,
                      VOID (*expiration_function)(ULONG input),
                      ULONG expiration_input, ULONG initial_ticks,
                      ULONG reschedule_ticks, UINT auto_activate)
{
    (VOID)timer_ptr; (VOID)name_ptr; (VOID)expiration_function;
    (VOID)expiration_input; (VOID)initial_ticks; (VOID)reschedule_ticks;
    (VOID)auto_activate;
    return TX_SUCCESS;
}

UINT _tx_timer_activate(TX_TIMER *timer_ptr)
{
    (VOID)timer_ptr;
    return TX_SUCCESS;
}

UINT _tx_timer_deactivate(TX_TIMER *timer_ptr)
{
    (VOID)timer_ptr;
    return TX_SUCCESS;
}

UINT _tx_timer_delete(TX_TIMER *timer_ptr)
{
    (VOID)timer_ptr;
    return TX_SUCCESS;
}
