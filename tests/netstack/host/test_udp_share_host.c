/*
 * AmiNetXDuo, SO_REUSEPORT in the NetX Duo core.
 *
 * This is the host half of GitHub issue #38.  The NetX Duo fork lets a UDP
 * socket opt into port sharing by setting nx_udp_socket_share before bind;
 * the dispatch then fans a multicast datagram out to every same-port sharer
 * and delivers a unicast datagram to exactly one.  The BSD layer sets the flag
 * from ASF_REUSEPORT (options.c -> socket.c), and the built-in mDNS responder
 * sets it on its own 5353 socket, so this test drives the core directly.
 *
 * The topology is built by hand out of an NX_IP; ThreadX primitives and the
 * one packet-pool cleanup path are stubbed exactly as test_tcp_source_connect
 * does, and the packet header fields are set to the host-order values the IP
 * receive path leaves behind after its in-place byte swap.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nx_api.h"
#include "nx_udp.h"
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


/* --------------------------------------------------------------- stubs ---- */

TX_THREAD *_tx_thread_current_ptr;
volatile ULONG _tx_thread_preempt_disable;
ULONG _tx_thread_system_state;

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

/* Referenced by nx_packet_allocate.c's error unwind; a full pool is never hit
   here, so the no-op is enough. */
VOID _nx_packet_pool_cleanup(TX_THREAD *thread_ptr NX_CLEANUP_PARAMETER)
{
    NX_CLEANUP_EXTENSION
    (void)thread_ptr;
}

/* Referenced by nx_udp_socket_bind.c's NX_ANY_PORT and wait_option arms, both
   of which this test avoids (specific port, NX_NO_WAIT). */
UINT _nx_udp_free_port_find(NX_IP *ip_ptr, UINT port, UINT *free_port_ptr)
{
    (void)ip_ptr;
    *free_port_ptr = port;
    return NX_SUCCESS;
}

VOID _nx_udp_bind_cleanup(TX_THREAD *thread_ptr NX_CLEANUP_PARAMETER)
{
    NX_CLEANUP_EXTENSION
    (void)thread_ptr;
}


/* ---------------------------------------------------------- the machine --- */

#define H_PORT          5353U

static NX_IP          h_ip;
static NX_PACKET_POOL h_pool;
static UCHAR          h_pool_memory[16 * 256];

static NX_UDP_SOCKET  h_socket_a;
static NX_UDP_SOCKET  h_socket_b;
static NX_UDP_SOCKET  h_socket_c;   /* non-sharing */

/* A packet buffer laid out as the IP receive path leaves it: IPv4 header at
   the data start, UDP header at the prepend pointer. */
static UCHAR          h_packet_bytes[64];
static NX_PACKET      h_packet;

/* The port-table bucket a 5353 bind lands in. */
static UINT h_index(void)
{
    return (UINT)((H_PORT + (H_PORT >> 8)) & NX_UDP_PORT_TABLE_MASK);
}

/* Put a socket into the state a freshly created NetX UDP socket is in, with
   the opt-in flag off. */
static void h_socket_arm(NX_UDP_SOCKET *socket_ptr)
{
    memset(socket_ptr, 0, sizeof(*socket_ptr));
    socket_ptr -> nx_udp_socket_id   = NX_UDP_ID;
    socket_ptr -> nx_udp_socket_ip_ptr = &h_ip;
    socket_ptr -> nx_udp_socket_port = H_PORT;
    socket_ptr -> nx_udp_socket_queue_maximum = 4;
}

/* Build one IPv4/UDP datagram whose destination is `dest_ip` (host order) and
   whose UDP destination port is H_PORT. */
static void h_packet_arm(ULONG dest_ip)
{
    NX_IPV4_HEADER *ipv4;
    NX_UDP_HEADER  *udp;
    ULONG           payload;

    memset(h_packet_bytes, 0, sizeof(h_packet_bytes));

    ipv4 = (NX_IPV4_HEADER *)(VOID *)h_packet_bytes;
    /* Only the destination address is read by the dispatch path; the rest of
       the IPv4 header is not inspected here. */
    ipv4 -> nx_ip_header_destination_ip  = dest_ip;

    /* NX_IPV4_HEADER is 20 bytes; the UDP header follows it. */
    udp = (NX_UDP_HEADER *)(VOID *)(h_packet_bytes + sizeof(NX_IPV4_HEADER));
    /* The UDP header is still in wire order here: _nx_udp_packet_receive does
       the in-place NX_CHANGE_ULONG_ENDIAN itself.  Store word 0 as the raw
       network bytes -- destination-port bytes at offsets 2 and 3, which on a
       little-endian host is the high half of the ULONG -- so the swap leaves
       5353 in the low half. */
    udp -> nx_udp_header_word_0 =
        (((ULONG)H_PORT & 0x00FFUL) << 24) | (((ULONG)H_PORT & 0xFF00UL) << 8);
    udp -> nx_udp_header_word_1 = 0;

    payload = 0x11223344UL;

    memset(&h_packet, 0, sizeof(h_packet));
    h_packet.nx_packet_data_start    = h_packet_bytes;
    h_packet.nx_packet_ip_header     = h_packet_bytes;
    h_packet.nx_packet_prepend_ptr   = h_packet_bytes + sizeof(NX_IPV4_HEADER);
    h_packet.nx_packet_append_ptr    = h_packet_bytes + sizeof(NX_IPV4_HEADER)
                                       + sizeof(NX_UDP_HEADER) + sizeof(payload);
    h_packet.nx_packet_length        = sizeof(NX_UDP_HEADER) + sizeof(payload);
    h_packet.nx_packet_pool_owner    = &h_pool;
    h_packet.nx_packet_ip_version    = NX_IP_VERSION_V4;
}


int main(void)
{
    ULONG dest;
    UINT  status;

    memset(&h_ip, 0, sizeof(h_ip));
    h_ip.nx_ip_id = NX_IP_ID;
    h_ip.nx_ip_udp_port_table[h_index()] = NX_NULL;

    if (_nx_packet_pool_create(&h_pool, "host", 256, h_pool_memory,
                               sizeof(h_pool_memory)) != NX_SUCCESS)
    {
        printf("UdpShare: no packet pool\n");
        return 1;
    }

    h_ip.nx_ip_default_packet_pool = &h_pool;

    /* ---- the opt-in bind gate ------------------------------------------- */

    /* Every participant must opt in before bind, so A opts in first. */
    h_socket_arm(&h_socket_a);
    h_socket_a.nx_udp_socket_share = NX_TRUE;
    status = _nx_udp_socket_bind(&h_socket_a, H_PORT, NX_NO_WAIT);
    h_check(status == NX_SUCCESS, "first bind of 5353 succeeds");

    /* A second sharer may co-bind the port. */
    h_socket_arm(&h_socket_b);
    h_socket_b.nx_udp_socket_share = NX_TRUE;
    status = _nx_udp_socket_bind(&h_socket_b, H_PORT, NX_NO_WAIT);
    h_check(status == NX_SUCCESS, "a sharing socket co-binds 5353");

    /* A non-sharer may not. */
    h_socket_arm(&h_socket_c);
    status = _nx_udp_socket_bind(&h_socket_c, H_PORT, NX_NO_WAIT);
    h_check(status == NX_PORT_UNAVAILABLE,
            "a non-sharing socket is refused on a shared port");

    /* And the reverse: a port held without sharing will not take a sharer,
       which is the "everyone opts in" rule from the other side. */
    h_ip.nx_ip_udp_port_table[h_index()] = NX_NULL;
    h_socket_arm(&h_socket_a);
    status = _nx_udp_socket_bind(&h_socket_a, H_PORT, NX_NO_WAIT);
    h_check(status == NX_SUCCESS, "a lone non-sharing socket binds");

    h_socket_arm(&h_socket_b);
    h_socket_b.nx_udp_socket_share = NX_TRUE;
    status = _nx_udp_socket_bind(&h_socket_b, H_PORT, NX_NO_WAIT);
    h_check(status == NX_PORT_UNAVAILABLE,
            "a sharer is refused on a port held without sharing");

    /* ---- multicast fans out to every sharer ----------------------------- */

    /* Rebuild a clean two-socket shared list for the dispatch checks. */
    h_ip.nx_ip_udp_port_table[h_index()] = NX_NULL;
    h_socket_arm(&h_socket_a);
    h_socket_arm(&h_socket_b);
    h_socket_a.nx_udp_socket_share = NX_TRUE;
    h_socket_b.nx_udp_socket_share = NX_TRUE;

    status = _nx_udp_socket_bind(&h_socket_a, H_PORT, NX_NO_WAIT);
    h_check(status == NX_SUCCESS, "rearm: socket A binds");
    status = _nx_udp_socket_bind(&h_socket_b, H_PORT, NX_NO_WAIT);
    h_check(status == NX_SUCCESS, "rearm: socket B co-binds");

    dest = 0xE0000001UL;   /* 224.0.0.1, host order */
    h_packet_arm(dest);
    _nx_udp_packet_receive(&h_ip, &h_packet);

    h_check(h_socket_a.nx_udp_socket_receive_count == 1,
            "multicast: primary socket queued one");
    h_check(h_socket_b.nx_udp_socket_receive_count == 1,
            "multicast: co-bound socket received the clone");

    /* ---- unicast is delivered to exactly one ---------------------------- */

    dest = 0x0A000001UL;   /* 10.0.0.1, host order */
    h_packet_arm(dest);
    _nx_udp_packet_receive(&h_ip, &h_packet);

    h_check(h_socket_a.nx_udp_socket_receive_count == 2,
            "unicast: the primary socket got the second datagram");
    h_check(h_socket_b.nx_udp_socket_receive_count == 1,
            "unicast: the co-bound socket did not get it");

    /* ---- a non-sharing sole owner is not fanned out --------------------- */

    h_ip.nx_ip_udp_port_table[h_index()] = NX_NULL;
    h_socket_arm(&h_socket_a);
    h_socket_a.nx_udp_socket_share = NX_FALSE;

    status = _nx_udp_socket_bind(&h_socket_a, H_PORT, NX_NO_WAIT);
    h_check(status == NX_SUCCESS, "rearm: a lone non-sharing socket binds");

    dest = 0xE0000001UL;
    h_packet_arm(dest);
    _nx_udp_packet_receive(&h_ip, &h_packet);

    h_check(h_socket_a.nx_udp_socket_receive_count == 1,
            "multicast to a lone socket is delivered once, no fan-out");

    if (h_failures == 0)
    {
        printf("UdpShare: all %lu checks passed\n", h_checks);
        return 0;
    }

    printf("UdpShare: %lu of %lu checks failed\n", h_failures, h_checks);
    return 1;
}
