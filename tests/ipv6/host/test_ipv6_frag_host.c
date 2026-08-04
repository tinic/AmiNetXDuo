/*
 * AmiNetXDuo -- inbound fragment reassembly and what bounds it.
 *
 * nx_ip_fragment_enable() is now called, so a fragmented datagram is held and
 * put back together instead of counted and released.  What that adds is a
 * queue with the packet pool behind it, and the pool on the smallest supported
 * machine is AMI_POOL_MIN_PACKETS -- sixteen.  A first fragment whose tail
 * never arrives pins a whole pool packet for the reassembly timeout however
 * few bytes it carried, so the interesting cases here are the ones a lab
 * network will not produce on demand: a tail that never comes, a pool already
 * spent, and a sender asking for a longer hold than it is entitled to.
 *
 * What is checked:
 *
 *   1. Three IPv6 fragments arriving in order become one datagram, delivered
 *      once, with the fragment header gone and the length the sum of the
 *      parts.  Arriving in the reverse order they become the same datagram --
 *      the assembly list is ordered by offset, not by arrival.
 *
 *   2. A fragment arriving while the pool is at or below the reserve is
 *      refused and queued nowhere.  Without this the reserve does not exist
 *      and sixteen forty-byte packets take the whole pool of a 1 MB machine.
 *      One packet above the reserve is still accepted, so the guard is a
 *      bound and not an off switch.
 *
 *   3. A datagram whose tail never arrives is released, in full, after
 *      NX_IPV6_MAX_REASSEMBLY_TIME sweeps and not before.
 *
 *   4. The IPv4 hold time is NX_IPV4_MAX_REASSEMBLY_TIME whatever TTL the
 *      sender wrote.  RFC 791 section 3.2 makes it MAX(timeout, TTL), which
 *      lets a peer pin a pool packet for 255 seconds by asking; RFC 1122
 *      3.3.2 asks for a fixed value instead.
 *
 * Real, compiled from third_party/netxduo/common/src into this binary:
 * nx_ipv6_process_fragment_option.c, nx_ip_fragment_assembly.c and
 * nx_ip_fragment_timeout_check.c -- the queue, the reassembly and the sweep,
 * so the list state each check reads is the list the stack would have.
 *
 * Stubbed: the packet pool, _nx_packet_release, _nx_ip_dispatch_process and
 * the two ICMP error senders.  The pool stub is a counter rather than storage
 * because every check here is about the count.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nx_api.h"
#include "nx_ip.h"
#include "nx_ipv6.h"
#include "nx_packet.h"
#include "nx_icmpv4.h"
#include "nx_icmpv6.h"

#include <stdio.h>
#include <stdlib.h>
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


/* --------------------------------------------------------------- stubs ---- */

static TX_THREAD h_caller_thread;

TX_THREAD     *_tx_thread_current_ptr = &h_caller_thread;
TX_THREAD      _tx_timer_thread;
UINT           _tx_thread_preempt_disable;
volatile ULONG _tx_thread_system_state;

UINT _tx_thread_interrupt_disable(VOID)
{
    return 0;
}

VOID _tx_thread_interrupt_restore(UINT previous_posture)
{
    NX_PARAMETER_NOT_USED(previous_posture);
}

TX_THREAD *_tx_thread_identify(VOID)
{
    return _tx_thread_current_ptr;
}

/* NX_ASSERT's failure arm, an endless sleep on a real target. */
UINT _tx_thread_sleep(ULONG timer_ticks)
{
    NX_PARAMETER_NOT_USED(timer_ticks);

    printf("FAIL an NX_ASSERT fired\n");
    exit(1);
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

UINT _tx_event_flags_set(TX_EVENT_FLAGS_GROUP *group_ptr, ULONG flags_to_set, UINT set_option)
{
    NX_PARAMETER_NOT_USED(group_ptr);
    NX_PARAMETER_NOT_USED(flags_to_set);
    NX_PARAMETER_NOT_USED(set_option);
    return TX_SUCCESS;
}

static UINT       h_released;            /* packets handed to the pool back  */
static NX_PACKET *h_delivered;           /* the last reassembled datagram    */
static UINT       h_deliveries;
static UINT       h_delivered_protocol;
static ULONG      h_delivered_length;

UINT _nx_packet_release(NX_PACKET *packet_ptr)
{
    NX_PARAMETER_NOT_USED(packet_ptr);

    h_released++;
    return NX_SUCCESS;
}

UINT _nx_ip_dispatch_process(NX_IP *ip_ptr, NX_PACKET *packet_ptr, UINT protocol)
{
    NX_PARAMETER_NOT_USED(ip_ptr);

    h_deliveries++;
    h_delivered = packet_ptr;
    h_delivered_protocol = protocol;
    h_delivered_length = packet_ptr -> nx_packet_length;

    /* Consumed: a zero return tells the caller not to release it. */
    return 0;
}

static UINT h_icmp_errors;

VOID _nx_icmpv6_send_error_message(NX_IP *ip_ptr, NX_PACKET *offending_packet,
                                   ULONG word1, ULONG error_pointer)
{
    NX_PARAMETER_NOT_USED(ip_ptr);
    NX_PARAMETER_NOT_USED(offending_packet);
    NX_PARAMETER_NOT_USED(word1);
    NX_PARAMETER_NOT_USED(error_pointer);

    h_icmp_errors++;
}

VOID _nx_icmpv4_send_error_message(NX_IP *ip_ptr, NX_PACKET *offending_packet,
                                   ULONG word1, ULONG error_pointer)
{
    NX_PARAMETER_NOT_USED(ip_ptr);
    NX_PARAMETER_NOT_USED(offending_packet);
    NX_PARAMETER_NOT_USED(word1);
    NX_PARAMETER_NOT_USED(error_pointer);

    h_icmp_errors++;
}


/* ----------------------------------------------------------- the fixture -- */

/*
 * The pool is AMI_POOL_MIN_PACKETS, the floor src/netstack/ clamps to and the
 * only size at which the reserve is worth arguing about: half of sixteen is
 * eight, and eight packets is what has to be left for ARP, ND and TCP while
 * somebody is holding the other eight.
 */
#define H_POOL_TOTAL    16
#define H_POOL_RESERVE  (H_POOL_TOTAL / 2)

/* More packets than any one check needs, so a leak shows up as a shortage. */
#define H_PACKETS       24
#define H_PAYLOAD       256

static NX_IP          h_ip;
static NX_PACKET_POOL h_pool;
static NX_PACKET      h_packet[H_PACKETS];
static UCHAR          h_body[H_PACKETS][H_PAYLOAD];
static NX_IPV6_HEADER h_v6_header[H_PACKETS];
static NX_IPV4_HEADER h_v4_header[H_PACKETS];
static UINT           h_packets_used;

/* 2001:db8::2, the peer that is sending us a fragmented datagram. */
#define H_PEER_0        0x20010DB8UL
#define H_PEER_3        2UL

/* 2001:db8::1, us. */
#define H_LOCAL_0       0x20010DB8UL
#define H_LOCAL_3       1UL

#define H_FRAGMENT_ID   0x11223344UL
#define H_NEXT_HEADER   NX_PROTOCOL_UDP

static VOID h_reset(VOID)
{
    memset(&h_ip, 0, sizeof(h_ip));
    memset(&h_pool, 0, sizeof(h_pool));
    memset(h_packet, 0, sizeof(h_packet));
    memset(h_body, 0, sizeof(h_body));
    memset(h_v6_header, 0, sizeof(h_v6_header));
    memset(h_v4_header, 0, sizeof(h_v4_header));

    h_packets_used = 0;
    h_released = 0;
    h_delivered = NX_NULL;
    h_deliveries = 0;
    h_delivered_protocol = 0;
    h_delivered_length = 0;
    h_icmp_errors = 0;

    h_pool.nx_packet_pool_total = H_POOL_TOTAL;
    h_pool.nx_packet_pool_available = H_POOL_TOTAL;

    /* Reassembly is on, which is what the netstack now does at start-up. */
    h_ip.nx_ip_fragment_assembly = _nx_ip_fragment_assembly;
    h_ip.nx_ip_fragment_timeout_check = _nx_ip_fragment_timeout_check;
}

static NX_PACKET *h_next_packet(VOID)
{
NX_PACKET *packet_ptr;

    if (h_packets_used == H_PACKETS)
    {
        printf("FAIL the fixture ran out of packets\n");
        exit(1);
    }

    packet_ptr = &h_packet[h_packets_used];
    packet_ptr -> nx_packet_pool_owner = &h_pool;
    packet_ptr -> nx_packet_data_start = h_body[h_packets_used];
    packet_ptr -> nx_packet_data_end   = h_body[h_packets_used] + H_PAYLOAD;

    /* Held by reassembly is held out of the pool. */
    h_pool.nx_packet_pool_available--;
    h_packets_used++;

    return packet_ptr;
}

/*
 * One IPv6 fragment, offered to the receive path exactly as
 * _nx_ip_dispatch_process() offers it: prepend_ptr on the fragment header, the
 * outer IPv6 header byte-swapped into host order already, the fragment header
 * still in network order because the option handler is what swaps it.
 *
 * `offset` is in bytes and a multiple of eight, which is what the wire field
 * holds once its three flag bits are masked off.
 */
static UINT h_deliver_v6(ULONG id, ULONG offset, UINT more, ULONG data_length)
{
NX_PACKET                      *packet_ptr = h_next_packet();
NX_IPV6_HEADER                 *ip_header  = &h_v6_header[h_packets_used - 1];
NX_IPV6_HEADER_FRAGMENT_OPTION *option;
ULONG                           payload_length;

    payload_length = (ULONG)sizeof(NX_IPV6_HEADER_FRAGMENT_OPTION) + data_length;

    ip_header -> nx_ip_header_word_0 = 0x60000000UL;
    ip_header -> nx_ip_header_word_1 = (payload_length << 16) |
                                       ((ULONG)NX_PROTOCOL_NEXT_HEADER_FRAGMENT << 8) | 64UL;
    ip_header -> nx_ip_header_source_ip[0] = H_PEER_0;
    ip_header -> nx_ip_header_source_ip[1] = 0UL;
    ip_header -> nx_ip_header_source_ip[2] = 0UL;
    ip_header -> nx_ip_header_source_ip[3] = H_PEER_3;
    ip_header -> nx_ip_header_destination_ip[0] = H_LOCAL_0;
    ip_header -> nx_ip_header_destination_ip[1] = 0UL;
    ip_header -> nx_ip_header_destination_ip[2] = 0UL;
    ip_header -> nx_ip_header_destination_ip[3] = H_LOCAL_3;

    option = (NX_IPV6_HEADER_FRAGMENT_OPTION *)packet_ptr -> nx_packet_data_start;

    option -> nx_ipv6_header_fragment_option_next_header = H_NEXT_HEADER;
    option -> nx_ipv6_header_fragment_option_reserved    = 0;
    option -> nx_ipv6_header_fragment_option_offset_flag =
        (USHORT)((offset & 0xFFF8UL) | (more ? 1UL : 0UL));
    option -> nx_ipv6_header_fragment_option_packet_id   = id;

    /* Network order, which is how it comes off the wire. */
    NX_CHANGE_USHORT_ENDIAN(option -> nx_ipv6_header_fragment_option_offset_flag);

    packet_ptr -> nx_packet_ip_version  = NX_IP_VERSION_V6;
    packet_ptr -> nx_packet_ip_header   = (UCHAR *)ip_header;
    packet_ptr -> nx_packet_prepend_ptr = packet_ptr -> nx_packet_data_start;
    packet_ptr -> nx_packet_append_ptr  = packet_ptr -> nx_packet_data_start + payload_length;
    packet_ptr -> nx_packet_length      = payload_length;
    packet_ptr -> nx_packet_next        = NX_NULL;
    packet_ptr -> nx_packet_last        = NX_NULL;

    return _nx_ipv6_process_fragment_option(&h_ip, packet_ptr);
}

/*
 * One IPv4 fragment, queued the way _nx_ipv4_packet_receive() queues it:
 * prepend_ptr on the IP header, header in host order, no option handler in
 * between.  `offset` is in eight-byte units, as the wire field holds it.
 */
static VOID h_deliver_v4(ULONG id, ULONG ttl, ULONG offset, UINT more, ULONG data_length)
{
NX_PACKET      *packet_ptr = h_next_packet();
NX_IPV4_HEADER *ip_header  = &h_v4_header[h_packets_used - 1];
ULONG           total_length;

    total_length = (ULONG)sizeof(NX_IPV4_HEADER) + data_length;

    memcpy(packet_ptr -> nx_packet_data_start, ip_header, sizeof(NX_IPV4_HEADER));
    ip_header = (NX_IPV4_HEADER *)packet_ptr -> nx_packet_data_start;

    ip_header -> nx_ip_header_word_0 = (0x45UL << 24) | total_length;
    ip_header -> nx_ip_header_word_1 = (id << 16) | (more ? NX_IP_MORE_FRAGMENT : 0UL) | offset;
    ip_header -> nx_ip_header_word_2 = (ttl << NX_IP_TIME_TO_LIVE_SHIFT) |
                                       ((ULONG)NX_IP_UDP);
    ip_header -> nx_ip_header_source_ip      = 0xC0000201UL;
    ip_header -> nx_ip_header_destination_ip = 0xC0000202UL;

    packet_ptr -> nx_packet_ip_version  = NX_IP_VERSION_V4;
    packet_ptr -> nx_packet_ip_header   = packet_ptr -> nx_packet_data_start;
    packet_ptr -> nx_packet_prepend_ptr = packet_ptr -> nx_packet_data_start;
    packet_ptr -> nx_packet_append_ptr  = packet_ptr -> nx_packet_data_start + total_length;
    packet_ptr -> nx_packet_length      = total_length;
    packet_ptr -> nx_packet_next        = NX_NULL;
    packet_ptr -> nx_packet_last        = NX_NULL;
    packet_ptr -> nx_packet_queue_next  = NX_NULL;

    /* Straight onto the received-fragment queue, as the receive path does. */
    if (h_ip.nx_ip_received_fragment_head)
    {
        h_ip.nx_ip_received_fragment_tail -> nx_packet_queue_next = packet_ptr;
        h_ip.nx_ip_received_fragment_tail = packet_ptr;
    }
    else
    {
        h_ip.nx_ip_received_fragment_head = packet_ptr;
        h_ip.nx_ip_received_fragment_tail = packet_ptr;
    }
}

/*
 * Leave the pool with `free` packets at the moment the next fragment is
 * examined.  The fragment itself is already out of the pool by then -- it had
 * to be allocated to be received -- so this is one more than the level the
 * guard reads.
 */
static VOID h_pool_free_after_next(ULONG free)
{
    h_pool.nx_packet_pool_available = free + 1;
}

/* Run the once-a-second sweep for `seconds`. */
static VOID h_age(ULONG seconds)
{
ULONG i;

    for (i = 0; i < seconds; i++)
    {
        _nx_ip_fragment_timeout_check(&h_ip);
    }
}

/* How many datagrams are on the assembly list. */
static ULONG h_assembling(VOID)
{
NX_PACKET *fragment = h_ip.nx_ip_fragment_assembly_head;
ULONG      count = 0;

    while (fragment)
    {
        count++;
        fragment = fragment -> nx_packet_queue_next;
    }

    return count;
}


/* ---------------------------------------------------------------- tests --- */

/* Three fragments in order. */
static VOID test_reassembles_in_order(VOID)
{
    h_reset();

    h_deliver_v6(H_FRAGMENT_ID, 0,   1, 64);
    h_deliver_v6(H_FRAGMENT_ID, 64,  1, 64);
    h_deliver_v6(H_FRAGMENT_ID, 128, 0, 32);

    h_check(h_deliveries == 0, "nothing is delivered before the assembly runs");

    _nx_ip_fragment_assembly(&h_ip);

    h_check(h_deliveries == 1, "a complete datagram is delivered once");
    h_check(h_delivered_length == 160,
            "the delivered length is the sum of the fragment payloads");
    h_check(h_delivered_protocol == H_NEXT_HEADER,
            "the protocol comes from the fragment header's next header");
    h_check(h_assembling() == 0, "the assembly list is empty afterwards");
    h_check(h_ip.nx_ip_packets_reassembled == 1, "the reassembly is counted");
}

/* The same three, arriving last-first. */
static VOID test_reassembles_out_of_order(VOID)
{
    h_reset();

    h_deliver_v6(H_FRAGMENT_ID, 128, 0, 32);
    h_deliver_v6(H_FRAGMENT_ID, 64,  1, 64);
    h_deliver_v6(H_FRAGMENT_ID, 0,   1, 64);

    _nx_ip_fragment_assembly(&h_ip);

    h_check(h_deliveries == 1, "fragments arriving in reverse still reassemble");
    h_check(h_delivered_length == 160,
            "and to the same length as the in-order case");
    h_check(h_assembling() == 0, "and leave nothing on the assembly list");
}

/* The reserve. */
static VOID test_pool_reserve(VOID)
{
UINT status;

    h_reset();

    /* One packet above the reserve: still accepted. */
    h_pool_free_after_next(H_POOL_RESERVE + 1);

    status = h_deliver_v6(H_FRAGMENT_ID, 0, 1, 64);

    h_check(status == NX_SUCCESS,
            "a fragment one packet above the reserve is accepted");
    h_check(h_ip.nx_ip_received_fragment_head != NX_NULL,
            "and is queued for assembly");

    /* At the reserve: refused, and not queued anywhere. */
    h_reset();
    h_pool_free_after_next(H_POOL_RESERVE);

    status = h_deliver_v6(H_FRAGMENT_ID, 0, 1, 64);

    h_check(status != NX_SUCCESS, "a fragment at the reserve is refused");
    h_check(h_ip.nx_ip_received_fragment_head == NX_NULL,
            "and is queued nowhere");
    h_check(h_assembling() == 0, "and starts no reassembly");

    /* Below it too, which is where a lossy link leaves the pool. */
    h_reset();
    h_pool_free_after_next(0);

    status = h_deliver_v6(H_FRAGMENT_ID, 0, 1, 64);

    h_check(status != NX_SUCCESS, "a fragment below the reserve is refused");
    h_check(h_ip.nx_ip_received_fragment_head == NX_NULL,
            "and is queued nowhere either");
}

/* A tail that never arrives. */
static VOID test_incomplete_times_out(VOID)
{
    h_reset();

    h_deliver_v6(H_FRAGMENT_ID, 0,  1, 64);
    h_deliver_v6(H_FRAGMENT_ID, 64, 1, 64);

    _nx_ip_fragment_assembly(&h_ip);

    h_check(h_deliveries == 0, "an incomplete datagram is not delivered");
    h_check(h_assembling() == 1, "and is held on the assembly list");

    /* One second short of the timeout it is still held. */
    h_age(NX_IPV6_MAX_REASSEMBLY_TIME);

    h_check(h_assembling() == 1,
            "the datagram is held for the whole reassembly timeout");
    h_check(h_released == 0, "and nothing is released before it expires");

    h_age(1);

    h_check(h_assembling() == 0, "the sweep drops it when the timeout expires");
    h_check(h_released == 2, "and releases every fragment it was holding");
    h_check(h_ip.nx_ip_reassembly_failures == 1, "and counts the failure");
}

/* The IPv4 hold time does not follow the sender's TTL. */
static VOID test_ipv4_hold_time_is_fixed(VOID)
{
    h_reset();

    /* TTL 255, the longest hold RFC 791's MAX() rule would have granted. */
    h_deliver_v4(0x1234UL, 255UL, 0UL, 1, 64UL);

    _nx_ip_fragment_assembly(&h_ip);

    h_check(h_assembling() == 1, "the first fragment starts a reassembly");
    h_check(h_ip.nx_ip_fragment_assembly_head -> nx_packet_reassembly_time ==
            NX_IPV4_MAX_REASSEMBLY_TIME,
            "the hold time is the fixed timeout and not the TTL");

    h_age(NX_IPV4_MAX_REASSEMBLY_TIME);

    h_check(h_assembling() == 1, "it is held for the whole fixed timeout");

    h_age(1);

    h_check(h_assembling() == 0,
            "and dropped there rather than 240 seconds later");
    h_check(h_released == 1, "and its one fragment is released");
}


int main(void)
{
    test_reassembles_in_order();
    test_reassembles_out_of_order();
    test_pool_reserve();
    test_incomplete_times_out();
    test_ipv4_hold_time_is_fixed();

    printf("%lu checks, %lu failures\n", h_checks, h_failures);

    return h_failures ? 1 : 0;
}
