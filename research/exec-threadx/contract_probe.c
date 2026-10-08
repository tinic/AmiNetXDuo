/* Compile against the real pinned headers, never substitute test structs.
 * SPDX-License-Identifier: MIT */
#define NX_SOURCE_CODE
#include "nx_api.h"
#include "tx_thread.h"
#include "tx_timer.h"
#include <stddef.h>
_Static_assert(sizeof(ULONG)==4, "research ABI probe requires m68k ILP32");
const unsigned long anx_research_contract_sizes[] = {
    sizeof(TX_THREAD), sizeof(TX_MUTEX), sizeof(TX_EVENT_FLAGS_GROUP), sizeof(TX_TIMER),
    offsetof(TX_THREAD,tx_thread_suspend_cleanup),
    offsetof(TX_THREAD,tx_thread_suspend_status),
    offsetof(TX_THREAD,tx_thread_suspended_next),
    offsetof(TX_THREAD,tx_thread_timer),
    offsetof(NX_IP,nx_ip_thread), offsetof(NX_IP,nx_ip_protection)
};
void anx_research_contract_probe(TX_THREAD *thread)
{
    thread->tx_thread_suspend_control_block=TX_NULL;
    thread->tx_thread_additional_suspend_info=TX_NULL;
    thread->tx_thread_timer.tx_timer_internal_remaining_ticks=0;
    (void)_tx_thread_current_ptr;
    (void)_tx_thread_preempt_disable;
    (void)_tx_thread_system_state;
}
