/*
 * AmiNetXDuo, multicast loopback fan-out to co-bound sharers (#38 x #47).
 *
 * The guest sockopt_test shows: two SO_REUSEPORT sockets share 0.0.0.0:5353,
 * both join 224.0.0.251, A sends a datagram to the group, and only A receives
 * it.  This host test drives the real local-send loopback path (IP send ->
 * driver -> loopback copy -> _nx_ip_packet_receive -> _nx_ipv4_packet_receive
 * -> _nx_udp_packet_receive) so the fan-out is exercised exactly as the guest
 * does, unlike test_udp_share_host.c which injects a pre-parsed datagram.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nx_api.h"
#include "nx_ip.h"
#include "nx_udp.h"
#include "nx_packet.h"

#include <stdio.h>
#include <string.h>

static unsigned long h_checks;
static unsigned long h_failures;

static void h_check(int ok, const char *what)
{
    h_checks++;

    if (!ok)
    {
        h_failures++;
        printf("FAIL %s\n", what);
    }
}

#define H_OUR_IP        IP_ADDRESS(10, 0, 0, 17)
#define H_NETMASK       IP_ADDRESS(255, 255, 255, 0)
#define H_GROUP         IP_ADDRESS(224, 0, 0, 251)
#define H_PORT          5353
#define H_PAYLOAD       "m"

static NX_IP           h_ip;
static NX_PACKET_POOL  h_pool;
static NX_UDP_SOCKET   h_m;                    /* mDNS responder, bound first */
static NX_UDP_SOCKET   h_a;                    /* the sender                  */
static NX_UDP_SOCKET   h_b;                    /* the co-bound sibling        */

static UINT h_m_cb;                            /* mDNS callback invocations   */
static UINT h_a_cb;                            /* sibling A callback invocations */
static UINT h_b_cb;                            /* sibling B callback invocations */

static VOID h_m_receive(NX_UDP_SOCKET *sock)
{
    (VOID)sock;
    h_m_cb++;
}

static VOID h_a_receive(NX_UDP_SOCKET *sock)
{
    (VOID)sock;
    h_a_cb++;
}

static VOID h_b_receive(NX_UDP_SOCKET *sock)
{
    (VOID)sock;
    h_b_cb++;
}

/* 1568 is AMI_POOL_PAYLOAD (include/aminetxduo/netstack.h). */
#define H_POOL_PAYLOAD  1568
#define H_POOL_PACKETS  64
static ULONG h_pool_memory[((H_POOL_PAYLOAD + sizeof(NX_PACKET) + 32) *
                            H_POOL_PACKETS) / sizeof(ULONG)];

static UINT h_wire_sends;

static TX_THREAD  h_caller_thread;

TX_THREAD         *_tx_thread_current_ptr = &h_caller_thread;
TX_THREAD          _tx_timer_thread;
UINT               _tx_thread_preempt_disable;
volatile ULONG     _tx_thread_system_state;

UINT _tx_thread_interrupt_disable(VOID) { return 0; }
VOID _tx_thread_interrupt_restore(UINT previous_posture) { NX_PARAMETER_NOT_USED(previous_posture); }
UINT _tx_thread_sleep(ULONG timer_ticks) { NX_PARAMETER_NOT_USED(timer_ticks); return TX_SUCCESS; }
TX_THREAD *_tx_thread_identify(VOID) { return _tx_thread_current_ptr; }
UINT _tx_mutex_get(TX_MUTEX *mutex_ptr, ULONG wait_option)
{ NX_PARAMETER_NOT_USED(mutex_ptr); NX_PARAMETER_NOT_USED(wait_option); return TX_SUCCESS; }
UINT _tx_mutex_put(TX_MUTEX *mutex_ptr) { NX_PARAMETER_NOT_USED(mutex_ptr); return TX_SUCCESS; }
UINT _tx_mutex_create(TX_MUTEX *mutex_ptr, CHAR *name, UINT inherit)
{ NX_PARAMETER_NOT_USED(mutex_ptr); NX_PARAMETER_NOT_USED(name); NX_PARAMETER_NOT_USED(inherit); return TX_SUCCESS; }
UINT _tx_mutex_delete(TX_MUTEX *mutex_ptr) { NX_PARAMETER_NOT_USED(mutex_ptr); return TX_SUCCESS; }
VOID _tx_thread_system_suspend(TX_THREAD *thread_ptr) { NX_PARAMETER_NOT_USED(thread_ptr); }
VOID _tx_thread_system_resume(TX_THREAD *thread_ptr) { NX_PARAMETER_NOT_USED(thread_ptr); }
VOID _tx_thread_system_preempt_check(VOID) { }
UINT _tx_thread_preemption_change(TX_THREAD *thread_ptr, UINT new_threshold, UINT *old_threshold)
{
    NX_PARAMETER_NOT_USED(thread_ptr);
    NX_PARAMETER_NOT_USED(new_threshold);
    if (old_threshold) { *old_threshold = 0; }
    return TX_SUCCESS;
}
UINT _tx_event_flags_set(TX_EVENT_FLAGS_GROUP *group_ptr, ULONG flags_to_set, UINT set_option)
{ NX_PARAMETER_NOT_USED(group_ptr); NX_PARAMETER_NOT_USED(flags_to_set); NX_PARAMETER_NOT_USED(set_option); return TX_SUCCESS; }
ULONG _tx_time_get(VOID) { return 0; }

static VOID h_driver(NX_IP_DRIVER *request)
{
    switch (request -> nx_ip_driver_command)
    {

    case NX_LINK_PACKET_SEND:
    case NX_LINK_PACKET_BROADCAST:
        h_wire_sends++;
        _nx_packet_transmit_release(request -> nx_ip_driver_packet);
        break;

    default:
        break;
    }

    request -> nx_ip_driver_status = NX_SUCCESS;
}

/* Faithful to the guest: the deferred receive runs _nx_ip_packet_receive,
   which stamps nx_packet_ip_version + nx_packet_ip_header and dispatches to
   _nx_ipv4_packet_receive through the IP's function pointer (the IP thread
   does exactly this). */
VOID _nx_ip_packet_deferred_receive(NX_IP *ip_ptr, NX_PACKET *packet_ptr)
{
    _nx_ip_packet_receive(ip_ptr, packet_ptr);
}

static VOID h_interface_setup(VOID)
{
    NX_INTERFACE *if_ptr = &h_ip.nx_ip_interface[0];

    if_ptr -> nx_interface_valid                   = NX_TRUE;
    if_ptr -> nx_interface_name                    = "eth0";
    if_ptr -> nx_interface_link_up                 = NX_TRUE;
    if_ptr -> nx_interface_address_mapping_needed  = NX_TRUE;
    if_ptr -> nx_interface_ip_address              = H_OUR_IP;
    if_ptr -> nx_interface_ip_network_mask         = H_NETMASK;
    if_ptr -> nx_interface_ip_network              = H_OUR_IP & H_NETMASK;
    if_ptr -> nx_interface_ip_mtu_size             = 1500;
    if_ptr -> nx_interface_link_driver_entry       = h_driver;
    if_ptr -> nx_interface_physical_address_msw    = 0x0000UL;
    if_ptr -> nx_interface_physical_address_lsw    = 0x00112233UL;

    /* The joined group, with loopback on, as bsd_mcast_join + #47 would leave
       it for a sender with IP_MULTICAST_LOOP on. */
    h_ip.nx_ipv4_multicast_entry[0].nx_ipv4_multicast_join_list = H_GROUP;
    h_ip.nx_ipv4_multicast_entry[0].nx_ipv4_multicast_join_interface_list = if_ptr;
    h_ip.nx_ipv4_multicast_entry[0].nx_ipv4_multicast_loopback_enable = NX_TRUE;
}

static UINT h_send_multicast(VOID)
{
    NX_PACKET *packet_ptr;
    UINT       status;

    status = nx_packet_allocate(&h_pool, &packet_ptr, NX_UDP_PACKET, NX_NO_WAIT);
    if (status != NX_SUCCESS)
    {
        h_check(0, "packet_allocate");
        return status;
    }

    status = nx_packet_data_append(packet_ptr, (VOID *)H_PAYLOAD,
                                   sizeof(H_PAYLOAD) - 1, &h_pool, NX_NO_WAIT);
    if (status != NX_SUCCESS)
    {
        h_check(0, "packet_data_append");
        nx_packet_release(packet_ptr);
        return status;
    }

    return nx_udp_socket_send(&h_a, packet_ptr, H_GROUP, H_PORT);
}

int main(void)
{
    UINT status;
    UINT n_m = 0, n_a = 0, n_b = 0;
    NX_PACKET *received;

    printf("AmiNetXDuo multicast share loopback\n");

    memset(&h_ip, 0, sizeof(h_ip));

    status = nx_packet_pool_create(&h_pool, "host pool", H_POOL_PAYLOAD,
                                   (VOID *)h_pool_memory,
                                   sizeof(h_pool_memory));
    h_check(status == NX_SUCCESS, "packet_pool_create");
    if (status != NX_SUCCESS)
    {
        return 1;
    }

    h_ip.nx_ip_id                  = NX_IP_ID;
    h_ip.nx_ip_default_packet_pool = &h_pool;
    h_ip.nx_ip_address             = H_OUR_IP;
    h_ip.nx_ip_network_mask        = H_NETMASK;
    h_ip.nx_ip_driver_mtu          = 1500;
    h_ip.nx_ipv4_packet_receive    = _nx_ipv4_packet_receive;

    h_interface_setup();

    status = nx_udp_enable(&h_ip);
    h_check(status == NX_SUCCESS, "udp_enable");

    status = nx_udp_socket_create(&h_ip, &h_m, "mdns", NX_IP_NORMAL,
                                  NX_DONT_FRAGMENT, 0x80, 8);
    h_check(status == NX_SUCCESS, "udp_socket_create m");

    status = nx_udp_socket_create(&h_ip, &h_a, "a", NX_IP_NORMAL,
                                  NX_DONT_FRAGMENT, 0x80, 8);
    h_check(status == NX_SUCCESS, "udp_socket_create a");

    status = nx_udp_socket_create(&h_ip, &h_b, "b", NX_IP_NORMAL,
                                  NX_DONT_FRAGMENT, 0x80, 8);
    h_check(status == NX_SUCCESS, "udp_socket_create b");

    /* All three opt into #38 sharing before bind; mDNS binds first so it is the
       port-list primary (head of the bound list), matching the guest. */
    h_m.nx_udp_socket_share = NX_TRUE;
    h_a.nx_udp_socket_share = NX_TRUE;
    h_b.nx_udp_socket_share = NX_TRUE;

    h_m.nx_udp_receive_callback = h_m_receive;
    h_a.nx_udp_receive_callback = h_a_receive;
    h_b.nx_udp_receive_callback = h_b_receive;

    status = nx_udp_socket_bind(&h_m, H_PORT, NX_NO_WAIT);
    h_check(status == NX_SUCCESS, "bind m 5353 (primary)");

    status = nx_udp_socket_bind(&h_a, H_PORT, NX_NO_WAIT);
    h_check(status == NX_SUCCESS, "bind a 5353");

    status = nx_udp_socket_bind(&h_b, H_PORT, NX_NO_WAIT);
    h_check(status == NX_SUCCESS, "bind b 5353 (co-bind)");

    if (h_failures)
    {
        printf("setup failed\n");
        return 1;
    }

    printf("pool available before send: %u\n",
           h_pool.nx_packet_pool_available);

    status = h_send_multicast();
    h_check(status == NX_SUCCESS, "send multicast 224.0.0.251:5353");
    printf("pool available after send:  %u\n",
           h_pool.nx_packet_pool_available);

    while (nx_udp_socket_receive(&h_m, &received, NX_NO_WAIT) == NX_SUCCESS)
    {
        n_m++;
        nx_packet_release(received);
    }
    while (nx_udp_socket_receive(&h_a, &received, NX_NO_WAIT) == NX_SUCCESS)
    {
        n_a++;
        nx_packet_release(received);
    }
    while (nx_udp_socket_receive(&h_b, &received, NX_NO_WAIT) == NX_SUCCESS)
    {
        n_b++;
        nx_packet_release(received);
    }

    printf("multicast: primary M %u (cb %u), sender A %u (cb %u), sibling B %u (cb %u)\n",
           n_m, h_m_cb, n_a, h_a_cb, n_b, h_b_cb);
    printf("wire sends: %u\n", h_wire_sends);

    h_check(n_m == 1, "primary mDNS socket queued its copy");
    h_check(h_m_cb == 1, "primary mDNS receive callback fired once");
    h_check(n_a == 1, "sender A received its own multicast");
    h_check(h_a_cb == 1, "sender A callback fired once");
    h_check(n_b == 1, "co-bound B received the fan-out clone");
    h_check(h_b_cb == 1, "co-bound B callback fired once");
    h_check(h_wire_sends == 1, "the datagram also went out on the wire");

    printf("%lu checks, %lu failures\n", h_checks, h_failures);
    return (h_failures == 0) ? 0 : 1;
}
