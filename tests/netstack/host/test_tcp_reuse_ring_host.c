/* Bounded contract test of the actual vendor bind/unbind list operations.
 * Scheduler services are inert; every ring node is a live local object.
 * SPDX-License-Identifier: MIT
 */
#include "nx_api.h"
#include "nx_tcp.h"
#include <stdio.h>

static unsigned checks, failures, mutex_depth, suspended;
static TX_THREAD thread;
static TX_THREAD **suspended_on;
TX_THREAD *_tx_thread_current_ptr = &thread;
/* NX_RAND's explicit-port path is not taken in these cases. */
int ami_random_rand(void);
int ami_random_rand(void) { return 0; }

#define CHECK(c) do { ++checks; if (!(c)) { ++failures; \
    fprintf(stderr, "line %d: %s\n", __LINE__, #c); } } while (0)

UINT _tx_mutex_get(TX_MUTEX *mutex, ULONG wait)
{
    (void)mutex; (void)wait;
    ++mutex_depth;
    return TX_SUCCESS;
}
UINT _tx_mutex_put(TX_MUTEX *mutex)
{
    (void)mutex;
    CHECK(mutex_depth == 1);
    if (mutex_depth) --mutex_depth;
    return TX_SUCCESS;
}
UINT _tx_thread_interrupt_disable(void) { return 0; }
VOID _tx_thread_interrupt_restore(UINT posture) { (void)posture; }
UINT _nx_tcp_free_port_find(NX_IP *ip, UINT port, UINT *out)
{
    (void)ip;
    *out = port;
    return NX_SUCCESS;
}
VOID _nx_tcp_client_bind_cleanup(TX_THREAD *t NX_CLEANUP_PARAMETER)
{
    (void)t;
    NX_CLEANUP_EXTENSION
    CHECK(0);
}
VOID _nx_tcp_socket_thread_suspend(TX_THREAD **head,
    VOID (*cleanup)(TX_THREAD * NX_CLEANUP_PARAMETER), NX_TCP_SOCKET *s,
    TX_MUTEX *mutex, ULONG wait)
{
    (void)cleanup; (void)s; (void)wait;
    ++suspended;
    suspended_on = head;
    thread.tx_thread_suspend_status = NX_PORT_UNAVAILABLE;
    (void)_tx_mutex_put(mutex);
}
VOID _nx_tcp_socket_block_cleanup(NX_TCP_SOCKET *s)
{
    s->nx_tcp_socket_state = NX_TCP_CLOSED;
}
VOID _nx_tcp_socket_receive_queue_flush(NX_TCP_SOCKET *s)
{
    (void)s;
    CHECK(0);
}
VOID _nx_tcp_socket_thread_resume(TX_THREAD **head, UINT status)
{
    (void)head; (void)status;
    CHECK(0);
}

static UINT bucket(UINT port)
{
    return (port + (port >> 8)) & NX_TCP_PORT_TABLE_MASK;
}
static void init_socket(NX_TCP_SOCKET *s, NX_IP *ip)
{
    *s = (NX_TCP_SOCKET){0};
    s->nx_tcp_socket_ip_ptr = ip;
    s->nx_tcp_socket_state = NX_TCP_CLOSED;
}
static unsigned ring_size(NX_IP *ip, NX_TCP_SOCKET *wanted)
{
    NX_TCP_SOCKET *head = ip->nx_ip_tcp_port_table[bucket(80)];
    NX_TCP_SOCKET *p = head;
    unsigned n = 0, found = 0;
    if (!head) return 0;
    do {
        CHECK(p->nx_tcp_socket_bound_next != NX_NULL);
        CHECK(p->nx_tcp_socket_bound_previous != NX_NULL);
        if (!p->nx_tcp_socket_bound_next || !p->nx_tcp_socket_bound_previous)
            return 0;
        CHECK(p->nx_tcp_socket_bound_next->nx_tcp_socket_bound_previous == p);
        CHECK(p->nx_tcp_socket_bound_previous->nx_tcp_socket_bound_next == p);
        if (p == wanted) found = 1;
        ++n;
        p = p->nx_tcp_socket_bound_next;
    } while (p != head && n < 4);
    CHECK(p == head);
    return found ? n : 0;
}

static void run_reuse(unsigned layout)
{
    NX_IP ip = {0};
    NX_TCP_SOCKET a, b, c;
    const unsigned total = layout ? 3 : 2;
    init_socket(&a, &ip); init_socket(&b, &ip); init_socket(&c, &ip);
    CHECK(bucket(16) == bucket(80));
    if (layout == 2) CHECK(_nx_tcp_client_socket_bind(&b, 16, 0) == NX_SUCCESS);
    CHECK(_nx_tcp_client_socket_bind(&a, 80, 0) == NX_SUCCESS);
    if (layout == 1) CHECK(_nx_tcp_client_socket_bind(&b, 16, 0) == NX_SUCCESS);
    a.nx_tcp_socket_state = NX_TCP_TIMED_WAIT;
    c.nx_tcp_socket_reuse_address = NX_TRUE;
    CHECK(_nx_tcp_client_socket_bind(&c, 80, 0) == NX_SUCCESS);
    CHECK(ring_size(&ip, &a) == total);
    CHECK(ring_size(&ip, &c) == total);
    if (layout) CHECK(ring_size(&ip, &b) == total);
    CHECK(_nx_tcp_client_socket_unbind(&a) == NX_SUCCESS);
    CHECK(a.nx_tcp_socket_bound_next == NX_NULL);
    CHECK(ring_size(&ip, &c) == total - 1);
    if (layout) CHECK(ring_size(&ip, &b) == total - 1);
    CHECK(_nx_tcp_client_socket_bind(&c, 80, 0) == NX_ALREADY_BOUND);
    CHECK(mutex_depth == 0);
}

static void run_refusal(UINT reuse, UINT state, ULONG wait)
{
    NX_IP ip = {0};
    NX_TCP_SOCKET a, b, c;
    unsigned before = suspended;
    init_socket(&a, &ip); init_socket(&b, &ip); init_socket(&c, &ip);
    CHECK(_nx_tcp_client_socket_bind(&b, 16, 0) == NX_SUCCESS);
    CHECK(_nx_tcp_client_socket_bind(&a, 80, 0) == NX_SUCCESS);
    a.nx_tcp_socket_state = state;
    c.nx_tcp_socket_reuse_address = reuse;
    CHECK(_nx_tcp_client_socket_bind(&c, 80, wait) == NX_PORT_UNAVAILABLE);
    CHECK(ring_size(&ip, &a) == 2);
    CHECK(ring_size(&ip, &b) == 2);
    CHECK(c.nx_tcp_socket_bound_next == NX_NULL);
    if (wait) {
        CHECK(suspended == before + 1);
        CHECK(suspended_on == &a.nx_tcp_socket_bind_suspension_list);
        CHECK(c.nx_tcp_socket_bound_previous == &a);
        CHECK(c.nx_tcp_socket_bind_in_progress == &thread);
    } else CHECK(suspended == before);
    CHECK(mutex_depth == 0);
}

int main(void)
{
    run_reuse(0); run_reuse(1); run_reuse(2);
    run_refusal(NX_FALSE, NX_TCP_TIMED_WAIT, 0);
    run_refusal(NX_TRUE, NX_TCP_ESTABLISHED, 0);
    run_refusal(NX_TRUE, NX_TCP_ESTABLISHED, 1);
    printf("tcp_reuse_ring: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
