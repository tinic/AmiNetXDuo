/*
 * AmiNetXDuo, does a TCP connect leave from the source it was told to?
 *
 * The case is two interfaces on one subnet.  bind() names an address on the
 * second one and the destination is reachable from both, so the route lookup
 * which walks nx_ip_interface[] in order, answers with the FIRST.  Before
 * nxd_tcp_client_socket_source_connect() there was nothing to say otherwise
 * and connect() refused rather than leave from an address nobody asked for.
 *
 * The lab guest has one SANA-II card, so that topology cannot be booted; this
 * builds it out of an NX_IP with two nx_ip_interface[] entries filled in and
 * drives the real connect against it.
 *
 * Real, compiled from third_party/netxduo/common/src into this binary:
 * nxd_tcp_client_socket_connect.c and nxd_tcp_client_socket_source_connect.c
 * the whole source decision, nx_ip_route_find.c, which is what the
 * decision is made of, and nx_tcp_packet_send_syn.c, so the assertion is on
 * the source address the SYN actually carried rather than on a field.
 *
 * Stubbed: the driver and the mutexes.  _nx_ip_packet_send() records the
 * outgoing interface and releases the packet, and _nx_ip_checksum_compute()
 * records the source address handed to the TCP pseudo-header, which is the
 * one the IP header is about to be built with.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nx_api.h"
#include "nx_tcp.h"
#include "nx_ip.h"
#include "nx_packet.h"

#include <stdio.h>
#include <string.h>


/* ------------------------------------------------------------- harness ---- */

static unsigned long h_checks;
static unsigned long h_failures;

static void h_check(int ok, const char *what)
{
    h_checks++;

    if (!ok)
    {
        h_failures++;
        printf("  FAIL %s\n", what);
        return;
    }

    printf("  ok   %s\n", what);
}

/* What the SYN left with, or nothing when none was sent. */
static struct
{
    UINT          sent;
    NX_INTERFACE *interface_ptr;
    ULONG         source_ip;
    ULONG         destination_ip;
    ULONG         next_hop;
} h_syn;

static void h_reset(void)
{
    memset(&h_syn, 0, sizeof(h_syn));
}


/* --------------------------------------------------------------- stubs ---- */

ULONG _nx_tcp_fast_timer_rate;
ULONG _nx_tcp_ack_timer_rate;
ULONG _nx_tcp_transmit_timer_rate;

TX_THREAD *_tx_thread_current_ptr;
volatile ULONG _tx_thread_preempt_disable;

VOID _tx_thread_system_suspend(TX_THREAD *thread_ptr)
{
    (void)thread_ptr;
}

VOID _tx_thread_system_resume(TX_THREAD *thread_ptr)
{
    (void)thread_ptr;
}

UINT _tx_mutex_get(TX_MUTEX *mutex_ptr, ULONG wait_option)
{
    (void)mutex_ptr; (void)wait_option;
    return TX_SUCCESS;
}

UINT _tx_mutex_put(TX_MUTEX *mutex_ptr)
{
    (void)mutex_ptr;
    return TX_SUCCESS;
}

UINT _tx_thread_interrupt_disable(void)
{
    return 0;
}

VOID _tx_thread_interrupt_restore(UINT previous_posture)
{
    (void)previous_posture;
}

/* The SYN's timestamps option carries a clock reading.  Nothing here asserts on
   the value, only on the source address the SYN went out with. */
ULONG _tx_time_get(VOID)
{
    return 0;
}

/* NX_ASSERT parks the calling thread here; nothing below should trip one. */
UINT _tx_thread_sleep(ULONG timer_ticks)
{
    (void)timer_ticks;
    printf("  FAIL NX_ASSERT fired\n");
    h_failures++;
    return TX_SUCCESS;
}

/*
 * _nx_tcp_packet_send_syn() passes the source it chose here for the TCP
 * pseudo-header, which is nx_tcp_socket_connect_interface's address.  That is
 * the measurement.
 */
USHORT _nx_ip_checksum_compute(NX_PACKET *packet_ptr, ULONG protocol,
                               UINT data_length, ULONG *src_ip_addr,
                               ULONG *dest_ip_addr)
{
    (void)packet_ptr; (void)protocol; (void)data_length;

    if (src_ip_addr)
    {
        h_syn.source_ip = *src_ip_addr;
    }

    if (dest_ip_addr)
    {
        h_syn.destination_ip = *dest_ip_addr;
    }

    return 0;
}

VOID _nx_ip_packet_send(NX_IP *ip_ptr, NX_PACKET *packet_ptr,
                        ULONG destination_ip, ULONG type_of_service,
                        ULONG time_to_live, ULONG protocol, ULONG fragment,
                        ULONG next_hop_address)
{
    (void)ip_ptr; (void)type_of_service; (void)time_to_live;
    (void)protocol; (void)fragment;

    h_syn.sent++;
    h_syn.interface_ptr  = packet_ptr -> nx_packet_address.nx_packet_interface_ptr;
    h_syn.destination_ip = destination_ip;
    h_syn.next_hop       = next_hop_address;

    _nx_packet_transmit_release(packet_ptr);
}

VOID _nx_tcp_socket_receive_queue_flush(NX_TCP_SOCKET *socket_ptr)
{
    (void)socket_ptr;
}

VOID _nx_tcp_socket_thread_suspend(TX_THREAD **suspension_list_head,
                                   VOID (*suspend_cleanup)(TX_THREAD * NX_CLEANUP_PARAMETER),
                                   NX_TCP_SOCKET *socket_ptr, TX_MUTEX *mutex_ptr,
                                   ULONG wait_option)
{
    (void)suspension_list_head; (void)suspend_cleanup; (void)socket_ptr;
    (void)mutex_ptr; (void)wait_option;
}

VOID _nx_tcp_connect_cleanup(TX_THREAD *thread_ptr NX_CLEANUP_PARAMETER)
{
    NX_CLEANUP_EXTENSION
    (void)thread_ptr;
}

VOID _nx_tcp_disconnect_cleanup(TX_THREAD *thread_ptr NX_CLEANUP_PARAMETER)
{
    NX_CLEANUP_EXTENSION
    (void)thread_ptr;
}

VOID _nx_tcp_receive_cleanup(TX_THREAD *thread_ptr NX_CLEANUP_PARAMETER)
{
    NX_CLEANUP_EXTENSION
    (void)thread_ptr;
}

VOID _nx_tcp_transmit_cleanup(TX_THREAD *thread_ptr NX_CLEANUP_PARAMETER)
{
    NX_CLEANUP_EXTENSION
    (void)thread_ptr;
}

VOID _nx_tcp_cleanup_deferred(TX_THREAD *thread_ptr NX_CLEANUP_PARAMETER)
{
    NX_CLEANUP_EXTENSION
    (void)thread_ptr;
}


/* ---------------------------------------------------------- the machine --- */

#define H_NET           0x0A000000UL     /* 10.0.0.0/24  */
#define H_MASK          0xFFFFFF00UL
#define H_IF0_ADDR      0x0A000001UL     /* 10.0.0.1     */
#define H_IF1_ADDR      0x0A000002UL     /* 10.0.0.2     */
#define H_PEER          0x0A000009UL     /* 10.0.0.9     */
#define H_ELSEWHERE     0xC0A80902UL     /* 192.168.9.2  */
#define H_ELSEWHERE_NET 0xC0A80900UL

static NX_IP        h_ip;
static NX_PACKET_POOL h_pool;
static UCHAR        h_pool_memory[16 * 256];
static NX_TCP_SOCKET h_socket;
static NX_TCP_SOCKET h_bound_marker;

static void h_interface_set(UINT index, ULONG address, ULONG mask)
{
    NX_INTERFACE *nxif = &h_ip.nx_ip_interface[index];

    memset(nxif, 0, sizeof(*nxif));
    nxif -> nx_interface_valid            = NX_TRUE;
    nxif -> nx_interface_link_up          = NX_TRUE;
    nxif -> nx_interface_index            = (UCHAR)index;
    nxif -> nx_interface_ip_address       = address;
    nxif -> nx_interface_ip_network_mask  = mask;
    nxif -> nx_interface_ip_network       = address & mask;
    nxif -> nx_interface_ip_mtu_size      = 1500;
    nxif -> nx_interface_address_mapping_needed = NX_TRUE;
}

/* Put the socket back where _nxd_tcp_client_socket_connect() expects it. */
static void h_socket_rearm(void)
{
    h_socket.nx_tcp_socket_state             = NX_TCP_CLOSED;
    h_socket.nx_tcp_socket_bound_next        = &h_bound_marker;
    h_socket.nx_tcp_socket_connect_interface = NX_NULL;
    h_socket.nx_tcp_socket_next_hop_address  = 0;
    memset(&h_socket.nx_tcp_socket_connect_ip, 0, sizeof(NXD_ADDRESS));
    h_reset();
}


int main(void)
{
    NXD_ADDRESS peer;
    UINT        status;

    memset(&h_ip, 0, sizeof(h_ip));
    h_ip.nx_ip_id = NX_IP_ID;

    if (_nx_packet_pool_create(&h_pool, "host", 256, h_pool_memory,
                               sizeof(h_pool_memory)) != NX_SUCCESS)
    {
        printf("SourceConnect: no packet pool\n");
        return 1;
    }

    h_ip.nx_ip_default_packet_pool = &h_pool;

    /* Two interfaces on one subnet: the topology the whole thing is about. */
    h_interface_set(0, H_IF0_ADDR, H_MASK);
    h_interface_set(1, H_IF1_ADDR, H_MASK);

    if (_nx_tcp_socket_create(&h_ip, &h_socket, "host", NX_IP_NORMAL,
                              NX_FRAGMENT_OKAY, NX_IP_TIME_TO_LIVE, 2048,
                              NX_NULL, NX_NULL) != NX_SUCCESS)
    {
        printf("SourceConnect: no socket\n");
        return 1;
    }

    peer.nxd_ip_version    = NX_IP_VERSION_V4;
    peer.nxd_ip_address.v4 = H_PEER;

    /* ---- what the route answers when nobody named a source -------------- */

    h_socket_rearm();
    status = _nxd_tcp_client_socket_connect(&h_socket, &peer, 80, NX_NO_WAIT);

    h_check(status == NX_IN_PROGRESS, "an unnamed source connects");
    h_check(h_syn.sent == 1, "one SYN went out");
    h_check(h_syn.interface_ptr == &h_ip.nx_ip_interface[0],
            "it left by the first interface, which is what the route picks");
    h_check(h_syn.source_ip == H_IF0_ADDR,
            "its source is 10.0.0.1");

    /* ---- the same connect, told to leave from the other address --------- */

    h_socket_rearm();
    status = _nxd_tcp_client_socket_source_connect(&h_socket, &peer, 80, 1,
                                                   NX_NO_WAIT);

    h_check(status == NX_IN_PROGRESS, "a named source connects");
    h_check(h_syn.sent == 1, "one SYN went out");
    h_check(h_syn.interface_ptr == &h_ip.nx_ip_interface[1],
            "it left by the SECOND interface, against the route's own answer");
    h_check(h_syn.source_ip == H_IF1_ADDR,
            "its source is 10.0.0.2, the address that was asked for");
    h_check(h_socket.nx_tcp_socket_next_hop_address == H_PEER,
            "the next hop is the peer, on that interface");

    /* ---- naming the first one is still the first one -------------------- */

    h_socket_rearm();
    status = _nxd_tcp_client_socket_source_connect(&h_socket, &peer, 80, 0,
                                                   NX_NO_WAIT);

    h_check(status == NX_IN_PROGRESS && h_syn.source_ip == H_IF0_ADDR,
            "index 0 is 10.0.0.1");

    /* ---- a source with no route to the peer is refused, not sent -------- */

    h_interface_set(1, H_ELSEWHERE, H_MASK);

    h_socket_rearm();
    status = _nxd_tcp_client_socket_source_connect(&h_socket, &peer, 80, 1,
                                                   NX_NO_WAIT);

    h_check(status == NX_IP_ADDRESS_ERROR,
            "a source on another subnet is NX_IP_ADDRESS_ERROR");
    h_check(h_syn.sent == 0, "and no SYN was sent");
    h_check(h_socket.nx_tcp_socket_state == NX_TCP_CLOSED,
            "and the socket is left closed");

    /*
     * The same interface, still on another subnet, but now with a default
     * gateway on ITS network.  _nx_ip_route_find() takes the gateway only
     * when the constraint allows it, so this is the arm that shows the
     * constraint is a filter over the whole lookup and not over one loop.
     */
    h_ip.nx_ip_gateway_address   = H_ELSEWHERE_NET | 1UL;
    h_ip.nx_ip_gateway_interface = &h_ip.nx_ip_interface[1];

    h_socket_rearm();
    status = _nxd_tcp_client_socket_source_connect(&h_socket, &peer, 80, 1,
                                                   NX_NO_WAIT);

    h_check(status == NX_IN_PROGRESS && h_syn.source_ip == H_ELSEWHERE,
            "a source whose interface has the gateway connects through it");
    h_check(h_socket.nx_tcp_socket_next_hop_address == (H_ELSEWHERE_NET | 1UL),
            "and the next hop is the gateway");

    h_ip.nx_ip_gateway_address   = 0;
    h_ip.nx_ip_gateway_interface = NX_NULL;

    /* ---- an interface that is not there --------------------------------- */

    h_ip.nx_ip_interface[1].nx_interface_link_up = NX_FALSE;

    h_socket_rearm();
    status = _nxd_tcp_client_socket_source_connect(&h_socket, &peer, 80, 1,
                                                   NX_NO_WAIT);

    h_check(status == NX_NO_INTERFACE_ADDRESS,
            "a source on a down interface is NX_NO_INTERFACE_ADDRESS");
    h_check(h_syn.sent == 0, "and no SYN was sent");

    h_ip.nx_ip_interface[1].nx_interface_valid = NX_FALSE;

    h_socket_rearm();
    status = _nxd_tcp_client_socket_source_connect(&h_socket, &peer, 80, 1,
                                                   NX_NO_WAIT);

    h_check(status == NX_NO_INTERFACE_ADDRESS,
            "so is a source on an interface that was never configured");

    /* ---- the unnamed connect is unchanged by any of it ------------------ */

    h_interface_set(1, H_IF1_ADDR, H_MASK);

    h_socket_rearm();
    status = _nxd_tcp_client_socket_connect(&h_socket, &peer, 80, NX_NO_WAIT);

    h_check(status == NX_IN_PROGRESS && h_syn.source_ip == H_IF0_ADDR,
            "and an unnamed source still routes as it always did");

    printf("SourceConnect: %lu checks, %lu failures\n", h_checks, h_failures);
    return (h_failures == 0) ? 0 : 1;
}
