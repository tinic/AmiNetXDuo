/* Finish a deferred TCP receive abort before the target becomes READY.
 * Explicitly opt in; other TCP operations and real ISR callers are unsupported.
 * SPDX-License-Identifier: MIT */
#define NX_SOURCE_CODE
#include "netx_resume.h"
#include "nx_api.h"
#include "nx_tcp.h"
#include "tx_thread.h"

int anx_netx_receive_abort_cleanup(AnxTxThread *t)
{
    TX_THREAD *thread=t->thread;
    NX_TCP_SOCKET *socket;
    TX_MUTEX *mutex;
    int cleaned;
    if (_tx_thread_system_state || !_tx_thread_identify() || _tx_thread_preempt_disable ||
        thread->tx_thread_state!=TX_SUSPENDED || thread->tx_thread_suspend_status!=TX_WAIT_ABORTED ||
        thread->tx_thread_suspend_cleanup!=_nx_tcp_cleanup_deferred ||
        t->cleanup_at_suspend!=_nx_tcp_receive_cleanup ||
        t->control_at_suspend!=thread->tx_thread_suspend_control_block ||
        t->sequence_at_suspend!=thread->tx_thread_suspension_sequence)
        return 0;
    socket=(NX_TCP_SOCKET *)t->control_at_suspend;
    /* Storage must remain alive: ID checks cannot make a freed pointer safe. */
    if (!socket || socket->nx_tcp_socket_id!=NX_TCP_ID || !socket->nx_tcp_socket_ip_ptr)
        return 0;
    mutex=&socket->nx_tcp_socket_ip_ptr->nx_ip_protection;
    if (_tx_mutex_get(mutex,TX_WAIT_FOREVER)!=TX_SUCCESS) return 0;
    /* ThreadX abort already set TX_SUSPENDED/TX_WAIT_ABORTED. The real NetX
     * cleanup removes its node but neither overwrites status nor resumes it.
     * No packet producer can run between this cleanup and backend resume. */
    t->cleanup_at_suspend(thread,t->sequence_at_suspend);
    cleaned=!thread->tx_thread_suspend_cleanup && thread->tx_thread_state==TX_SUSPENDED &&
        thread->tx_thread_suspend_status==TX_WAIT_ABORTED;
    if (_tx_mutex_put(mutex)!=TX_SUCCESS) return 0;
    return cleaned;
}
