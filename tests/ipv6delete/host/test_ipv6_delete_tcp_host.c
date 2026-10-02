/*
 * Audit N-038: _nxd_ipv6_address_delete and the TCP connections using the
 * address it deletes.
 *
 * A socket keeps its local IPv6 address as a pointer into nx_ipv6_address[]
 * (nx_tcp_socket_ipv6_addr).  The delete zeroes that entry; the socket's next
 * ACK, FIN, RST or retransmission then reaches NX_ASSERT(if_ptr) in
 * _nx_ipv6_packet_send, which on the target sleeps forever holding
 * nx_ip_protection.  The delete must reset every such connection first.
 *
 * resets:     ESTABLISHED (with a blocked sender, a blocked receiver and a
 *             disconnect callback), SYN_SENT (with a blocked connect) and
 *             TIME_WAIT sockets on the deleted address are reset by the real
 *             _nx_tcp_socket_connection_reset, before the entry is zeroed:
 *             their states after, the waiters woken with the statuses the
 *             real cleanups assign, the sender's packet still the sender's.
 *             An ESTABLISHED socket on another address is untouched.
 * exclusions: a LISTEN socket and a CLOSED socket still pointing at the
 *             deleted entry are untouched, and so is a socket now connected
 *             over IPv4 that still holds the pointer from an earlier IPv6
 *             connection (the pointer is never cleared).
 * selfdelete: a disconnect callback that deletes its own socket does not
 *             derail the walk: the next socket on the address is still reset.
 *
 * Linked for real: nxd_ipv6_address_delete.c, nx_tcp_socket_connection_reset.c,
 * nx_tcp_socket_block_cleanup.c, nx_tcp_transmit_cleanup.c,
 * nx_tcp_receive_cleanup.c, nx_tcp_connect_cleanup.c,
 * nx_tcp_disconnect_cleanup.c and nx_tcp_socket_transmit_queue_flush.c.
 * Stubbed: ThreadX (mutex depth counted, resume recorded), the packet release,
 * the multicast leave and the SYN-cache flush.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tx_api.h"
#include "tx_thread.h"
#include "tx_timer.h"
#include "nx_api.h"
#include "nx_ip.h"
#include "nx_ipv6.h"
#include "nx_tcp.h"

#include <stdio.h>
#include <string.h>

TX_THREAD      *_tx_thread_current_ptr;
volatile ULONG  _tx_thread_system_state;
volatile UINT   _tx_thread_preempt_disable;
TX_THREAD       _tx_timer_thread;
ULONG           _nx_tcp_transmit_timer_rate = 1;

static NX_IP        ip;
static NX_INTERFACE iface;
static int          held;
static int          resumed;
static int          released;
static int          bad;

UINT _tx_thread_interrupt_disable(VOID)       { return 0; }
VOID _tx_thread_interrupt_restore(UINT p)     { (void) p; }
static UINT mget(TX_MUTEX *m) { if (m == &ip.nx_ip_protection) held++; return TX_SUCCESS; }
static UINT mput(TX_MUTEX *m) { if (m == &ip.nx_ip_protection) held--; return TX_SUCCESS; }
UINT _tx_mutex_get(TX_MUTEX *m, ULONG w)      { (void) w; return mget(m); }
UINT _tx_mutex_put(TX_MUTEX *m)               { return mput(m); }
UINT _txe_mutex_get(TX_MUTEX *m, ULONG w)     { (void) w; return mget(m); }
UINT _txe_mutex_put(TX_MUTEX *m)              { return mput(m); }
VOID _tx_thread_system_resume(TX_THREAD *t)   { (void) t; _tx_thread_preempt_disable--; resumed++; }
UINT _tx_event_flags_set(TX_EVENT_FLAGS_GROUP *g, ULONG f, UINT o)  { (void) g; (void) f; (void) o; return TX_SUCCESS; }
UINT _txe_event_flags_set(TX_EVENT_FLAGS_GROUP *g, ULONG f, UINT o) { (void) g; (void) f; (void) o; return TX_SUCCESS; }
VOID _nx_tcp_cleanup_deferred(TX_THREAD *t NX_CLEANUP_PARAMETER) { (void) t; NX_CLEANUP_EXTENSION }
UINT _nx_packet_release(NX_PACKET *p)         { (void) p; released++; return NX_SUCCESS; }
UINT _nx_ipv6_multicast_leave(NX_IP *i, ULONG *a, NX_INTERFACE *f) { (void) i; (void) a; (void) f; return NX_SUCCESS; }
VOID _nx_tcp_syncache_interface_flush(NX_IP *i, NX_INTERFACE *f, NXD_IPV6_ADDRESS *a) { (void) i; (void) f; (void) a; }

static void check(const char *what, int cond)
{
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    bad |= !cond;
}

#define NSOCK 6
static NX_TCP_SOCKET sock[NSOCK];
static TX_THREAD     t_tx, t_rx, t_conn;
static NX_PACKET     tx_pkt;

/* What a disconnect callback saw: how often, and whether the entry was
   still intact (attached) at that moment.  */
static int  cb_calls[NSOCK];
static int  cb_entry_intact[NSOCK];
static int  cb_selfdelete = -1;

static int index_of(NX_TCP_SOCKET *s) { return (int) (s - sock); }

static void unlink_socket(NX_TCP_SOCKET *s)
{
    /* nx_tcp_socket_delete.c:115-140, the ring part.  */
    if (s == s -> nx_tcp_socket_created_next)
    {
        ip.nx_ip_tcp_created_sockets_ptr = NX_NULL;
    }
    else
    {
        s -> nx_tcp_socket_created_next -> nx_tcp_socket_created_previous = s -> nx_tcp_socket_created_previous;
        s -> nx_tcp_socket_created_previous -> nx_tcp_socket_created_next = s -> nx_tcp_socket_created_next;
        if (ip.nx_ip_tcp_created_sockets_ptr == s)
            ip.nx_ip_tcp_created_sockets_ptr = s -> nx_tcp_socket_created_next;
    }
    ip.nx_ip_tcp_created_sockets_count--;
    s -> nx_tcp_socket_id = 0;
}

static VOID on_disconnect(NX_TCP_SOCKET *s)
{
    int i = index_of(s);

    cb_calls[i]++;
    cb_entry_intact[i] = ip.nx_ipv6_address[1].nxd_ipv6_address_attached == &iface;
    if (i == cb_selfdelete)
        unlink_socket(s);
}

static void setup(void)
{
    int i;

    memset(&ip, 0, sizeof(ip));
    memset(&iface, 0, sizeof(iface));
    memset(sock, 0, sizeof(sock));
    memset(cb_calls, 0, sizeof(cb_calls));
    memset(cb_entry_intact, 0, sizeof(cb_entry_intact));
    cb_selfdelete = -1;
    held = resumed = released = 0;
    _tx_thread_current_ptr = NX_NULL;

    /* Two addresses on one interface: slot 1 is deleted, slot 2 stays.  */
    for (i = 1; i <= 2; i++)
    {
        ip.nx_ipv6_address[i].nxd_ipv6_address_valid    = NX_TRUE;
        ip.nx_ipv6_address[i].nxd_ipv6_address_state    = NX_IPV6_ADDR_STATE_VALID;
        ip.nx_ipv6_address[i].nxd_ipv6_address_attached = &iface;
        ip.nx_ipv6_address[i].nxd_ipv6_address_index    = (UCHAR) i;
        ip.nx_ipv6_address[i].nxd_ipv6_address[0]       = 0x20010db8UL;
        ip.nx_ipv6_address[i].nxd_ipv6_address[3]       = (ULONG) i;
    }
    iface.nxd_interface_ipv6_address_list_head = &ip.nx_ipv6_address[1];
    ip.nx_ipv6_address[1].nxd_ipv6_address_next = &ip.nx_ipv6_address[2];

    /* The created-sockets ring, as nx_tcp_socket_create.c leaves it.  */
    for (i = 0; i < NSOCK; i++)
    {
        sock[i].nx_tcp_socket_id      = NX_TCP_ID;
        sock[i].nx_tcp_socket_ip_ptr  = &ip;
        sock[i].nx_tcp_socket_created_next     = &sock[(i + 1) % NSOCK];
        sock[i].nx_tcp_socket_created_previous = &sock[(i + NSOCK - 1) % NSOCK];
        sock[i].nx_tcp_socket_state   = NX_TCP_CLOSED;
        sock[i].nx_tcp_socket_client_type = NX_TRUE;
        sock[i].nx_tcp_socket_connect_port = 1000 + (UINT) i;   /* cleared by a reset */
        sock[i].nx_tcp_disconnect_callback = on_disconnect;
    }
    ip.nx_ip_tcp_created_sockets_ptr   = &sock[0];
    ip.nx_ip_tcp_created_sockets_count = NSOCK;
}

static void v6conn(NX_TCP_SOCKET *s, int slot)
{
    s -> nx_tcp_socket_connect_ip.nxd_ip_version        = NX_IP_VERSION_V6;
    s -> nx_tcp_socket_connect_ip.nxd_ip_address.v6[0]  = 0x20010db8UL;
    s -> nx_tcp_socket_connect_ip.nxd_ip_address.v6[3]  = 0x99UL;
    s -> nx_tcp_socket_ipv6_addr = &ip.nx_ipv6_address[slot];
}

static void suspend(TX_THREAD *t, TX_THREAD **list, ULONG *count, VOID (*cleanup)(TX_THREAD * NX_CLEANUP_PARAMETER),
                    NX_TCP_SOCKET *s)
{
    memset(t, 0, sizeof(*t));
    t -> tx_thread_state                 = TX_TCP_IP;
    t -> tx_thread_suspend_cleanup       = cleanup;
    t -> tx_thread_suspend_control_block = s;
    t -> tx_thread_suspended_next        = t;
    t -> tx_thread_suspended_previous    = t;
    *list = t;
    if (count)
        (*count)++;
}

static UINT delete_slot1(void)
{
    UINT status = _nxd_ipv6_address_delete(&ip, 1);

    check("the delete succeeds and the entry is zeroed",
          status == NX_SUCCESS && ip.nx_ipv6_address[1].nxd_ipv6_address_attached == NX_NULL &&
          ip.nx_ipv6_address[1].nxd_ipv6_address_valid == NX_FALSE);
    check("nx_ip_protection is released", held == 0);
    return status;
}

static void arm_resets(void)
{
    setup();
    /* 0 ESTABLISHED client on slot 1, a sender and a receiver blocked.  */
    sock[0].nx_tcp_socket_state    = NX_TCP_ESTABLISHED;
    v6conn(&sock[0], 1);
    suspend(&t_tx, &sock[0].nx_tcp_socket_transmit_suspension_list,
            &sock[0].nx_tcp_socket_transmit_suspended_count, _nx_tcp_transmit_cleanup, &sock[0]);
    t_tx.tx_thread_additional_suspend_info = &tx_pkt;
    suspend(&t_rx, &sock[0].nx_tcp_socket_receive_suspension_list,
            &sock[0].nx_tcp_socket_receive_suspended_count, _nx_tcp_receive_cleanup, &sock[0]);
    /* 1 SYN_SENT client on slot 1, its connect blocked.  */
    sock[1].nx_tcp_socket_state    = NX_TCP_SYN_SENT;
    v6conn(&sock[1], 1);
    memset(&t_conn, 0, sizeof(t_conn));
    t_conn.tx_thread_state                 = TX_TCP_IP;
    t_conn.tx_thread_suspend_cleanup       = _nx_tcp_connect_cleanup;
    t_conn.tx_thread_suspend_control_block = &sock[1];
    sock[1].nx_tcp_socket_connect_suspended_thread = &t_conn;
    /* 2 TIME_WAIT server-side socket on slot 1.  */
    sock[2].nx_tcp_socket_state       = NX_TCP_TIMED_WAIT;
    sock[2].nx_tcp_socket_client_type = NX_FALSE;
    v6conn(&sock[2], 1);
    /* 3 ESTABLISHED on slot 2.  */
    sock[3].nx_tcp_socket_state    = NX_TCP_ESTABLISHED;
    v6conn(&sock[3], 2);

    delete_slot1();

    printf("     states after: ESTABLISHED->%u, SYN_SENT->%u, TIMED_WAIT->%u, other address %u\n",
           sock[0].nx_tcp_socket_state, sock[1].nx_tcp_socket_state,
           sock[2].nx_tcp_socket_state, sock[3].nx_tcp_socket_state);
    check("ESTABLISHED on the address is reset to CLOSED",
          sock[0].nx_tcp_socket_state == NX_TCP_CLOSED && sock[0].nx_tcp_socket_connect_port == 0);
    check("its disconnect callback ran once, before the entry was zeroed",
          cb_calls[0] == 1 && cb_entry_intact[0] == 1);
    check("its blocked sender woke with NX_NOT_CONNECTED (nx_tcp_transmit_cleanup.c)",
          t_tx.tx_thread_suspend_status == NX_NOT_CONNECTED &&
          sock[0].nx_tcp_socket_transmit_suspension_list == NX_NULL);
    check("and its packet is still the sender's (not released by the stack)",
          t_tx.tx_thread_additional_suspend_info == &tx_pkt && released == 0);
    check("its blocked receiver woke with NX_NOT_CONNECTED (nx_tcp_receive_cleanup.c)",
          t_rx.tx_thread_suspend_status == NX_NOT_CONNECTED &&
          sock[0].nx_tcp_socket_receive_suspension_list == NX_NULL);
    check("SYN_SENT on the address is reset to CLOSED, its connect woken NX_NOT_CONNECTED (nx_tcp_connect_cleanup.c)",
          sock[1].nx_tcp_socket_state == NX_TCP_CLOSED &&
          t_conn.tx_thread_suspend_status == NX_NOT_CONNECTED &&
          sock[1].nx_tcp_socket_connect_suspended_thread == NX_NULL);
    check("TIME_WAIT on the address is reset (server side: back to LISTEN)",
          sock[2].nx_tcp_socket_state == NX_TCP_LISTEN_STATE && sock[2].nx_tcp_socket_connect_port == 0);
    check("callbacks only for the socket that was ESTABLISHED",
          cb_calls[1] == 0 && cb_calls[2] == 0);
    check("three waiters resumed", resumed == 3);
    check("the connection on the other address is untouched",
          sock[3].nx_tcp_socket_state == NX_TCP_ESTABLISHED && sock[3].nx_tcp_socket_connect_port == 1003 &&
          cb_calls[3] == 0);
}

static void arm_exclusions(void)
{
    setup();
    sock[0].nx_tcp_socket_state       = NX_TCP_LISTEN_STATE;
    sock[0].nx_tcp_socket_client_type = NX_FALSE;
    sock[0].nx_tcp_socket_ipv6_addr   = &ip.nx_ipv6_address[1];    /* stale, from its last connection */
    sock[1].nx_tcp_socket_state       = NX_TCP_CLOSED;
    sock[1].nx_tcp_socket_ipv6_addr   = &ip.nx_ipv6_address[1];
    /* An ESTABLISHED IPv4 connection on a socket whose last IPv6
       connection left the pointer at slot 1.  */
    sock[2].nx_tcp_socket_state       = NX_TCP_ESTABLISHED;
    sock[2].nx_tcp_socket_connect_ip.nxd_ip_version   = NX_IP_VERSION_V4;
    sock[2].nx_tcp_socket_connect_ip.nxd_ip_address.v4 = 0xc0000207UL;
    sock[2].nx_tcp_socket_ipv6_addr   = &ip.nx_ipv6_address[1];

    delete_slot1();

    check("the LISTEN socket with a stale pointer is untouched",
          sock[0].nx_tcp_socket_state == NX_TCP_LISTEN_STATE && sock[0].nx_tcp_socket_connect_port == 1000 &&
          cb_calls[0] == 0);
    check("the CLOSED socket is untouched",
          sock[1].nx_tcp_socket_state == NX_TCP_CLOSED && sock[1].nx_tcp_socket_connect_port == 1001 &&
          cb_calls[1] == 0);
    check("the IPv4 connection holding a stale pointer to the slot is untouched",
          sock[2].nx_tcp_socket_state == NX_TCP_ESTABLISHED && sock[2].nx_tcp_socket_connect_port == 1002 &&
          sock[2].nx_tcp_socket_connect_ip.nxd_ip_version == NX_IP_VERSION_V4 && cb_calls[2] == 0);
    check("nothing woken", resumed == 0);
}

static void arm_selfdelete(void)
{
    setup();
    sock[0].nx_tcp_socket_state    = NX_TCP_ESTABLISHED;
    v6conn(&sock[0], 1);
    sock[1].nx_tcp_socket_state    = NX_TCP_ESTABLISHED;
    v6conn(&sock[1], 1);
    sock[2].nx_tcp_socket_state    = NX_TCP_ESTABLISHED;
    v6conn(&sock[2], 2);
    cb_selfdelete = 0;

    delete_slot1();

    check("the socket whose callback deleted it was reset first",
          cb_calls[0] == 1 && sock[0].nx_tcp_socket_id == 0);
    check("the walk went on: the next socket on the address is reset, once",
          sock[1].nx_tcp_socket_state == NX_TCP_CLOSED && cb_calls[1] == 1);
    check("the other address is untouched",
          sock[2].nx_tcp_socket_state == NX_TCP_ESTABLISHED && cb_calls[2] == 0);
    check("the ring is intact without the deleted socket",
          ip.nx_ip_tcp_created_sockets_count == NSOCK - 1 &&
          ip.nx_ip_tcp_created_sockets_ptr -> nx_tcp_socket_created_previous -> nx_tcp_socket_created_next ==
              ip.nx_ip_tcp_created_sockets_ptr);
}

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "usage: %s resets|exclusions|selfdelete\n", argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "resets") == 0)
        arm_resets();
    else if (strcmp(argv[1], "exclusions") == 0)
        arm_exclusions();
    else if (strcmp(argv[1], "selfdelete") == 0)
        arm_selfdelete();
    else
        return 2;
    printf("%s\n", bad ? "FAIL" : "PASS");
    return bad;
}
