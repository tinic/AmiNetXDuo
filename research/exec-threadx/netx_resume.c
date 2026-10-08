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
    /* Resume hooks must never park. The abort policy below acquires the lock
     * before entering the raw ThreadX abort, while the target is still waiting. */
    if (mutex->tx_mutex_ownership_count && mutex->tx_mutex_owner!=_tx_thread_identify())
        return 0;
    if (_tx_mutex_get(mutex,TX_NO_WAIT)!=TX_SUCCESS) return 0;
    /* ThreadX abort already set TX_SUSPENDED/TX_WAIT_ABORTED. The real NetX
     * cleanup removes its node but neither overwrites status nor resumes it.
     * No packet producer can run between this cleanup and backend resume. */
    t->cleanup_at_suspend(thread,t->sequence_at_suspend);
    cleaned=!thread->tx_thread_suspend_cleanup && thread->tx_thread_state==TX_SUSPENDED &&
        thread->tx_thread_suspend_status==TX_WAIT_ABORTED;
    if (_tx_mutex_put(mutex)!=TX_SUCCESS) return 0;
    return cleaned;
}

int anx_netx_receive_abort_policy(AnxTxThread *t, UINT *status)
{
    TX_THREAD *thread=t->thread;
    NX_TCP_SOCKET *socket;
    TX_MUTEX *mutex;
    uint32_t operation=t->operation;
    ULONG sequence=thread->tx_thread_suspension_sequence;
    if (_tx_thread_system_state || !_tx_thread_identify() || _tx_thread_preempt_disable)
        return 0;
    /* Sleep, mutex, UDP and already-completed targets retain raw semantics. */
    if (thread->tx_thread_state!=TX_TCP_IP ||
        (thread->tx_thread_suspend_cleanup!=_nx_tcp_receive_cleanup &&
         thread->tx_thread_suspend_cleanup!=_nx_tcp_cleanup_deferred)) {
        *status=anx_tx_original_wait_abort(thread);
        return 1;
    }
    if (!operation || t->cleanup_at_suspend!=_nx_tcp_receive_cleanup ||
        t->control_at_suspend!=thread->tx_thread_suspend_control_block ||
        t->sequence_at_suspend!=sequence || t->resume_cleanup!=anx_netx_receive_abort_cleanup)
        return 0;
    socket=(NX_TCP_SOCKET *)t->control_at_suspend;
    if (!socket || socket->nx_tcp_socket_id!=NX_TCP_ID || !socket->nx_tcp_socket_ip_ptr)
        return 0;
    mutex=&socket->nx_tcp_socket_ip_ptr->nx_ip_protection;
    /* The wrapper pins t across this wait. Socket/IP storage must be retained
     * separately; no object deletion/freeing is implemented by this spike. */
    if (_tx_mutex_get(mutex,TX_WAIT_FOREVER)!=TX_SUCCESS) {
        *status=TX_WAIT_ABORT_ERROR; /* acquisition cancelled; target untouched */
        return 1;
    }
    /* Arrival/timeout/new suspension can win while this caller is parked.
     * operation is stable across private cleanup-grace rearming. */
    if (t->operation!=operation || thread->tx_thread_suspension_sequence!=sequence ||
        thread->tx_thread_suspend_control_block!=socket || thread->tx_thread_state!=TX_TCP_IP ||
        (thread->tx_thread_suspend_cleanup!=_nx_tcp_receive_cleanup &&
         thread->tx_thread_suspend_cleanup!=_nx_tcp_cleanup_deferred))
        *status=TX_WAIT_ABORT_ERROR;
    else *status=anx_tx_original_wait_abort(thread);
    return _tx_mutex_put(mutex)==TX_SUCCESS;
}
