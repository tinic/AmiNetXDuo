/*
 * AmiNetXDuo -- whether a broadcast this host sends reaches this host's own
 * sockets.
 *
 * Reported from an A3000 running Fitz 1.21: `fitz serve ram: name ramdisk` in
 * one shell and `fitz query` in another, and the machine does not list its own
 * share.  Every other machine on the LAN lists it.  `fitz query` broadcasts
 * "LIST\n" to 255.255.255.255:17710 (fitz-common-client.c, fitz_pms_list_udp)
 * and `fitz serve` binds that port on INADDR_ANY, so the two are on the same
 * machine and never meet.
 *
 * Ethernet is simplex -- a card does not hear its own transmissions -- so the
 * copy has to be made in software.  _nx_ip_driver_packet_send() makes it for a
 * unicast to our own address and, on request, for a multicast, and did not
 * make it for a broadcast.  4.4BSD copies one back in ether_output() and Linux
 * in ip_mc_output(), which is why the same program works everywhere else.
 *
 * Real, compiled from third_party/netxduo/common/src into this binary: the
 * whole path from nx_udp_socket_send() to nx_udp_socket_receive() --
 * nx_udp_socket_send.c, nxd_udp_socket_send.c, nx_ip_packet_send.c,
 * nx_ip_header_add.c, nx_ip_route_find.c, nx_ip_driver_packet_send.c (the
 * function under test), nx_packet_copy.c, nx_ipv4_packet_receive.c,
 * nx_ip_dispatch_process.c, nx_udp_packet_receive.c and nx_udp_socket_receive.c
 * -- plus the packet pool and the UDP port table underneath them.
 *
 * Stubbed: the link driver, which counts what would have gone on the wire and
 * releases the packet, and the IP thread.  _nx_ip_packet_deferred_receive()
 * normally queues the copy for that thread; here it runs the receive inline,
 * which is the same work in the caller's stack frame and lets one send be
 * checked before the next.
 *
 * Four destinations are sent to, because the fix must not turn into "loop
 * everything back":
 *
 *   255.255.255.255   the limited broadcast Fitz uses
 *   10.0.0.255        a directed broadcast for the interface's own prefix
 *   10.0.0.17         our own address, which looped back before the fix too
 *   10.0.0.42         another host, which must not be delivered locally
 *
 * SPDX-License-Identifier: MIT
 */

#include "nx_api.h"
#include "nx_ip.h"
#include "nx_udp.h"
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
        printf("FAIL %s\n", what);
    }
}

/* The interface, and the four destinations. */
#define H_OUR_IP        IP_ADDRESS(10, 0, 0, 17)
#define H_NETMASK       IP_ADDRESS(255, 255, 255, 0)
#define H_DIRECTED      IP_ADDRESS(10, 0, 0, 255)
#define H_OTHER_HOST    IP_ADDRESS(10, 0, 0, 42)

#define H_SERVER_PORT   17710               /* FITZ_PMS_PORT */
#define H_PAYLOAD       "LIST\n"

static NX_IP           h_ip;
static NX_PACKET_POOL  h_pool;
static NX_UDP_SOCKET   h_server;            /* bound to H_SERVER_PORT       */
static NX_UDP_SOCKET   h_client;            /* the one that broadcasts      */

/* 1568 is AMI_POOL_PAYLOAD (include/aminetxduo/netstack.h). */
#define H_POOL_PAYLOAD  1568
#define H_POOL_PACKETS  24
static ULONG h_pool_memory[((H_POOL_PAYLOAD + sizeof(NX_PACKET) + 32) *
                            H_POOL_PACKETS) / sizeof(ULONG)];

/* What the stubbed driver saw. */
static UINT h_wire_sends;
static UINT h_wire_broadcasts;


/* --------------------------------------------------------------- stubs ---- */

/*
 * The caller.  NetX Duo's _nxe_ wrappers refuse a call made from no thread at
 * all (NX_THREADS_ONLY_CALLER_CHECKING), so main() lends itself an identity.
 */
static TX_THREAD  h_caller_thread;

TX_THREAD         *_tx_thread_current_ptr = &h_caller_thread;
TX_THREAD          _tx_timer_thread;
UINT               _tx_thread_preempt_disable;
volatile ULONG     _tx_thread_system_state;

UINT _tx_thread_interrupt_disable(VOID)
{
    return 0;
}

VOID _tx_thread_interrupt_restore(UINT previous_posture)
{
    NX_PARAMETER_NOT_USED(previous_posture);
}

UINT _tx_thread_sleep(ULONG timer_ticks)
{
    NX_PARAMETER_NOT_USED(timer_ticks);
    return TX_SUCCESS;
}

TX_THREAD *_tx_thread_identify(VOID)
{
    return _tx_thread_current_ptr;
}

UINT _tx_mutex_get(TX_MUTEX *mutex_ptr, ULONG wait_option)
{
    NX_PARAMETER_NOT_USED(mutex_ptr);
    NX_PARAMETER_NOT_USED(wait_option);
    return TX_SUCCESS;
}

UINT _tx_mutex_put(TX_MUTEX *mutex_ptr)
{
    NX_PARAMETER_NOT_USED(mutex_ptr);
    return TX_SUCCESS;
}

UINT _tx_mutex_create(TX_MUTEX *mutex_ptr, CHAR *name, UINT inherit)
{
    NX_PARAMETER_NOT_USED(mutex_ptr);
    NX_PARAMETER_NOT_USED(name);
    NX_PARAMETER_NOT_USED(inherit);
    return TX_SUCCESS;
}

UINT _tx_mutex_delete(TX_MUTEX *mutex_ptr)
{
    NX_PARAMETER_NOT_USED(mutex_ptr);
    return TX_SUCCESS;
}

/* Nothing here suspends: every receive is NX_NO_WAIT and the datagram is
   already queued by the time it is asked for. */
VOID _tx_thread_system_suspend(TX_THREAD *thread_ptr)
{
    NX_PARAMETER_NOT_USED(thread_ptr);
}

VOID _tx_thread_system_resume(TX_THREAD *thread_ptr)
{
    NX_PARAMETER_NOT_USED(thread_ptr);
}

VOID _tx_thread_system_preempt_check(VOID)
{
}

UINT _tx_thread_preemption_change(TX_THREAD *thread_ptr, UINT new_threshold,
                                  UINT *old_threshold)
{
    NX_PARAMETER_NOT_USED(thread_ptr);
    NX_PARAMETER_NOT_USED(new_threshold);

    if (old_threshold)
    {
        *old_threshold = 0;
    }

    return TX_SUCCESS;
}

UINT _tx_event_flags_set(TX_EVENT_FLAGS_GROUP *group_ptr, ULONG flags_to_set,
                         UINT set_option)
{
    NX_PARAMETER_NOT_USED(group_ptr);
    NX_PARAMETER_NOT_USED(flags_to_set);
    NX_PARAMETER_NOT_USED(set_option);
    return TX_SUCCESS;
}

ULONG _tx_time_get(VOID)
{
    return 0;
}

/*
 * The link driver.  A SANA-II device cannot hear its own transmission, and
 * neither can this: it counts the frame and hands the packet back, which is
 * exactly the behaviour that makes the software copy necessary.
 */
static VOID h_driver(NX_IP_DRIVER *request)
{
    switch (request -> nx_ip_driver_command)
    {

    case NX_LINK_PACKET_SEND:
        h_wire_sends++;
        _nx_packet_transmit_release(request -> nx_ip_driver_packet);
        break;

    case NX_LINK_PACKET_BROADCAST:
        h_wire_broadcasts++;
        _nx_packet_transmit_release(request -> nx_ip_driver_packet);
        break;

    default:
        break;
    }

    request -> nx_ip_driver_status = NX_SUCCESS;
}

/*
 * The IP thread.  The real one wakes on an event flag and calls
 * _nx_ip_packet_receive(); running the receive inline is the same work and
 * keeps one send from overlapping the next.
 */
VOID _nx_ip_packet_deferred_receive(NX_IP *ip_ptr, NX_PACKET *packet_ptr)
{
    _nx_ipv4_packet_receive(ip_ptr, packet_ptr);
}

/* ---------------------------------------------------------------- setup --- */

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
}


/* ------------------------------------------------------------- the send --- */

/*
 * Send H_PAYLOAD from the client socket to `destination`, then drain the
 * server socket.  Returns the number of datagrams the server got.
 */
static UINT h_send_and_drain(ULONG destination, const char *label,
                             UINT expect_wire)
{
    NX_PACKET *packet_ptr;
    NX_PACKET *received;
    UINT       status;
    UINT       delivered = 0;
    UINT       before_wire = h_wire_sends + h_wire_broadcasts;

    status = nx_packet_allocate(&h_pool, &packet_ptr, NX_UDP_PACKET, NX_NO_WAIT);

    if (status != NX_SUCCESS)
    {
        h_check(0, "packet_allocate");
        return 0;
    }

    status = nx_packet_data_append(packet_ptr, (VOID *)H_PAYLOAD,
                                   sizeof(H_PAYLOAD) - 1, &h_pool, NX_NO_WAIT);

    if (status != NX_SUCCESS)
    {
        h_check(0, "packet_data_append");
        nx_packet_release(packet_ptr);
        return 0;
    }

    status = nx_udp_socket_send(&h_client, packet_ptr, destination,
                                H_SERVER_PORT);

    if (status != NX_SUCCESS)
    {
        h_check(0, "udp_socket_send");
        return 0;
    }

    /* A loopback must not replace the transmission, nor invent one. */
    h_check((h_wire_sends + h_wire_broadcasts) ==
            (before_wire + expect_wire),
            expect_wire ? "the datagram also went out on the wire"
                        : "the datagram did not go out on the wire");

    while (nx_udp_socket_receive(&h_server, &received, NX_NO_WAIT) == NX_SUCCESS)
    {
        ULONG length = 0;

        (VOID)nx_packet_length_get(received, &length);

        if ((length != (ULONG)(sizeof(H_PAYLOAD) - 1)) ||
            (memcmp(received -> nx_packet_prepend_ptr, H_PAYLOAD,
                    sizeof(H_PAYLOAD) - 1) != 0))
        {
            printf("FAIL %s: payload is not %u bytes of \"LIST\"\n",
                   label, (unsigned)(sizeof(H_PAYLOAD) - 1));
            h_failures++;
        }

        h_checks++;
        delivered++;
        nx_packet_release(received);
    }

    return delivered;
}


/* ----------------------------------------------------------------- main --- */

int main(void)
{
    UINT status;
    UINT delivered;

    printf("AmiNetXDuo broadcast loopback\n");

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

    h_interface_setup();

    status = nx_udp_enable(&h_ip);
    h_check(status == NX_SUCCESS, "udp_enable");

    status = nx_udp_socket_create(&h_ip, &h_server, "server", NX_IP_NORMAL,
                                  NX_DONT_FRAGMENT, 0x80, 8);
    h_check(status == NX_SUCCESS, "udp_socket_create server");

    status = nx_udp_socket_bind(&h_server, H_SERVER_PORT, NX_NO_WAIT);
    h_check(status == NX_SUCCESS, "udp_socket_bind server");

    status = nx_udp_socket_create(&h_ip, &h_client, "client", NX_IP_NORMAL,
                                  NX_DONT_FRAGMENT, 0x80, 8);
    h_check(status == NX_SUCCESS, "udp_socket_create client");

    status = nx_udp_socket_bind(&h_client, NX_ANY_PORT, NX_NO_WAIT);
    h_check(status == NX_SUCCESS, "udp_socket_bind client");

    if (h_failures)
    {
        printf("setup failed\n");
        return 1;
    }

    /* The report: a limited broadcast, which is what `fitz query` sends. */
    delivered = h_send_and_drain(NX_IP_LIMITED_BROADCAST, "255.255.255.255", 1);
    printf("  255.255.255.255  ->  %u delivered locally, %u on the wire\n",
           delivered, h_wire_broadcasts);
    h_check(delivered == 1, "a limited broadcast reaches our own socket");

    /* A directed broadcast for this interface's prefix takes the same branch. */
    h_wire_broadcasts = 0;
    delivered = h_send_and_drain(H_DIRECTED, "10.0.0.255", 1);
    printf("  10.0.0.255       ->  %u delivered locally, %u on the wire\n",
           delivered, h_wire_broadcasts);
    h_check(delivered == 1, "a directed broadcast reaches our own socket");

    /* Our own address looped back before the fix and must still. */
    h_wire_sends = 0;
    delivered = h_send_and_drain(H_OUR_IP, "10.0.0.17", 0);
    printf("  10.0.0.17        ->  %u delivered locally, %u on the wire\n",
           delivered, h_wire_sends);
    h_check(delivered == 1, "a unicast to ourselves still loops back");

    /*
     * Another host on the same prefix.  There is no ARP entry, so the packet
     * is queued for one and never reaches the driver -- which is the point:
     * nothing about it is delivered locally.
     */
    h_wire_sends = 0;
    h_wire_broadcasts = 0;

    {
        NX_PACKET *packet_ptr;
        NX_PACKET *received;

        status = nx_packet_allocate(&h_pool, &packet_ptr, NX_UDP_PACKET,
                                    NX_NO_WAIT);
        h_check(status == NX_SUCCESS, "packet_allocate");

        status = nx_packet_data_append(packet_ptr, (VOID *)H_PAYLOAD,
                                       sizeof(H_PAYLOAD) - 1, &h_pool,
                                       NX_NO_WAIT);
        h_check(status == NX_SUCCESS, "packet_data_append");

        status = nx_udp_socket_send(&h_client, packet_ptr, H_OTHER_HOST,
                                    H_SERVER_PORT);
        h_check(status == NX_SUCCESS, "udp_socket_send to another host");

        delivered = 0;

        while (nx_udp_socket_receive(&h_server, &received, NX_NO_WAIT) ==
               NX_SUCCESS)
        {
            delivered++;
            nx_packet_release(received);
        }
    }

    printf("  10.0.0.42        ->  %u delivered locally\n", delivered);
    h_check(delivered == 0, "a unicast to another host is not delivered here");

    printf("%lu checks, %lu failures\n", h_checks, h_failures);

    return (h_failures == 0) ? 0 : 1;
}
