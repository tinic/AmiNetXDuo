/*
 * AmiNetXDuo, SACK-based loss recovery, RFC 6675: what a sender does with a
 * hole the peer has described, and what it must not do with the
 * acknowledgments that then fill it.
 *
 * The A1200 on 2026-09-17 (playhouse3:~/anxd-caps/loss0.0001.pcap): one lost
 * segment in a flight of 130, and the sender filled the 59-segment hole one
 * segment per partial acknowledgment -- 2.25 ms each behind the receive
 * coalescing -- then retransmitted, on every acknowledgment of a batch, the
 * segments the next acknowledgment in the same batch already covered.  0.01 %
 * loss was 63 Mbit/s out of 650.
 *
 * SPDX-License-Identifier: MIT
 */
#include "nx_api.h"
#include "nx_tcp.h"
#include "nx_packet.h"
#include "nx_ip.h"

#include <stdio.h>
#include <string.h>

#ifndef H_ISN_RX
#define H_ISN_RX        0x20000000UL
#endif

static ULONG h_now = 1000;

ULONG _tx_time_get(VOID)
{
    return h_now;
}

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

static void h_check_eq(ULONG got, ULONG want, const char *what)
{
    h_checks++;

    if (got != want)
    {
        h_failures++;
        printf("FAIL %s: got %lu, wanted %lu\n", what,
               (unsigned long)got, (unsigned long)want);
    }
}

TX_THREAD *_tx_thread_current_ptr;

ULONG _nx_tcp_fast_timer_rate;
ULONG _nx_tcp_ack_timer_rate;
ULONG _nx_tcp_transmit_timer_rate;

static UINT h_datagrams;

UINT _tx_mutex_get(TX_MUTEX *mutex_ptr, ULONG wait_option)
{
    (void)mutex_ptr;
    (void)wait_option;
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

UINT _tx_thread_info_get(TX_THREAD *thread_ptr, CHAR **name, UINT *state,
                         ULONG *run_count, UINT *priority,
                         UINT *preemption_threshold, ULONG *time_slice,
                         TX_THREAD **next_thread, TX_THREAD **suspended_thread)
{
    (void)thread_ptr; (void)name; (void)state; (void)run_count;
    (void)preemption_threshold; (void)time_slice;
    (void)next_thread; (void)suspended_thread;

    if (priority)
    {
        *priority = 0;
    }

    return TX_SUCCESS;
}

UINT _tx_thread_preemption_change(TX_THREAD *thread_ptr, UINT new_threshold,
                                  UINT *old_threshold)
{
    (void)thread_ptr;
    (void)new_threshold;

    if (old_threshold)
    {
        *old_threshold = 0;
    }

    return TX_SUCCESS;
}

UINT _tx_thread_sleep(ULONG timer_ticks)
{
    (void)timer_ticks;
    printf("FAIL NX_ASSERT fired\n");
    h_failures++;
    return TX_SUCCESS;
}

VOID _nx_tcp_socket_thread_suspend(TX_THREAD **suspension_list_head,
                                   VOID (*suspend_cleanup)(TX_THREAD * NX_CLEANUP_PARAMETER),
                                   NX_TCP_SOCKET *socket_ptr, TX_MUTEX *mutex_ptr,
                                   ULONG wait_option)
{
    (void)suspension_list_head; (void)suspend_cleanup; (void)socket_ptr;
    (void)mutex_ptr; (void)wait_option;
}

VOID _nx_tcp_socket_thread_resume(TX_THREAD **suspension_list_head, UINT status)
{
    (void)suspension_list_head;
    (void)status;
}

VOID _nx_tcp_transmit_cleanup(TX_THREAD *thread_ptr NX_CLEANUP_PARAMETER)
{
    NX_CLEANUP_EXTENSION
    (void)thread_ptr;
}

#ifdef NX_ENABLE_TCP_SACK
static NX_PACKET_POOL h_control_pool;
static NX_PACKET h_control_packet;
static union { ULONG align; UCHAR bytes[1600]; } h_control_data;
static UINT h_control_used;
static UINT h_control_deny;
static UINT h_control_releases;
static NX_PACKET h_slice_packet;
static union { ULONG align; UCHAR bytes[1600]; } h_slice_data;
static UINT h_slice_used;
static UINT h_slice_deny;
static UINT h_spend_credit_in_append;
static NX_TCP_SOCKET *h_credit_socket;
#endif

UINT _nx_packet_release(NX_PACKET *packet_ptr)
{
#ifdef NX_ENABLE_TCP_SACK
    if (packet_ptr == &h_slice_packet)
    {
        h_check(h_slice_used, "transient slice is released only once");
        h_slice_used = NX_FALSE;
    }
    if (packet_ptr == &h_control_packet)
    {
        h_check(h_control_used, "control packet is released only once");
        h_control_used = NX_FALSE;
        h_control_releases++;
    }
#else
    (void)packet_ptr;
#endif
    return NX_SUCCESS;
}

UINT _nx_packet_allocate(NX_PACKET_POOL *pool_ptr, NX_PACKET **packet_ptr,
                         ULONG packet_type, ULONG wait_option)
{
    (void)packet_type; (void)wait_option;
#ifdef NX_ENABLE_TCP_SACK
    if (pool_ptr == &h_control_pool && packet_type != 0 && !h_control_deny && !h_slice_deny && !h_slice_used)
    {
        memset(&h_slice_packet, 0, sizeof(h_slice_packet));
        h_slice_packet.nx_packet_data_start = h_slice_data.bytes;
        h_slice_packet.nx_packet_data_end = h_slice_data.bytes + sizeof(h_slice_data.bytes);
        h_slice_packet.nx_packet_prepend_ptr = h_slice_data.bytes + packet_type;
        h_slice_packet.nx_packet_append_ptr = h_slice_packet.nx_packet_prepend_ptr;
        h_slice_packet.nx_packet_pool_owner = pool_ptr;
        h_slice_packet.nx_packet_union_next.nx_packet_tcp_queue_next = (NX_PACKET *)NX_PACKET_ALLOCATED;
        h_slice_used = NX_TRUE;
        *packet_ptr = &h_slice_packet;
        return NX_SUCCESS;
    }
    if (pool_ptr == &h_control_pool && packet_type == 0 && !h_control_deny && !h_control_used)
    {
        memset(&h_control_packet, 0, sizeof(h_control_packet));
        h_control_packet.nx_packet_data_start = h_control_data.bytes;
        h_control_packet.nx_packet_data_end = h_control_data.bytes + sizeof(h_control_data.bytes);
        h_control_packet.nx_packet_prepend_ptr = h_control_data.bytes;
        h_control_packet.nx_packet_append_ptr = h_control_data.bytes;
        h_control_packet.nx_packet_pool_owner = pool_ptr;
        h_control_used = NX_TRUE;
        *packet_ptr = &h_control_packet;
        return NX_SUCCESS;
    }
#else
    (void)pool_ptr; (void)packet_ptr;
#endif
    return NX_NO_PACKET;
}

UINT _nx_packet_data_append(NX_PACKET *packet_ptr, VOID *data_start, ULONG data_size,
                            NX_PACKET_POOL *pool_ptr, ULONG wait_option)
{
    (void)pool_ptr; (void)wait_option;
#ifdef NX_ENABLE_TCP_SACK
    if (packet_ptr == &h_slice_packet)
    {
        if ((ULONG)(packet_ptr -> nx_packet_data_end - packet_ptr -> nx_packet_append_ptr) < data_size)
        {
            return NX_NO_PACKET;
        }
        memcpy(packet_ptr -> nx_packet_append_ptr, data_start, data_size);
        packet_ptr -> nx_packet_append_ptr += data_size;
        packet_ptr -> nx_packet_length += data_size;
        if (h_spend_credit_in_append)
        {
            if (h_spend_credit_in_append == 2)
            {
                h_credit_socket -> nx_tcp_socket_state = NX_TCP_CLOSED;
            }
            else
            {
                h_credit_socket -> nx_tcp_socket_prr_out += h_credit_socket -> nx_tcp_socket_prr_credit;
                h_credit_socket -> nx_tcp_socket_prr_credit = 0;
            }
            h_spend_credit_in_append = NX_FALSE;
        }
    }
#else
    (void)packet_ptr; (void)data_start; (void)data_size;
#endif
    return NX_SUCCESS;
}

UINT _nx_packet_copy(NX_PACKET *packet_ptr, NX_PACKET **new_packet_ptr,
                     NX_PACKET_POOL *pool_ptr, ULONG wait_option)
{
    (void)packet_ptr; (void)new_packet_ptr; (void)pool_ptr; (void)wait_option;
    return NX_NO_PACKET;
}

USHORT _nx_ip_checksum_compute(NX_PACKET *packet_ptr, ULONG protocol,
                               UINT data_length, ULONG *src_ip_addr,
                               ULONG *dest_ip_addr)
{
    (void)packet_ptr; (void)protocol; (void)data_length;
    (void)src_ip_addr; (void)dest_ip_addr;
    return 0;
}


#define H_MSS           1460UL
#define H_BUF           1600
#define H_PACKETS       200
#ifndef H_ISN
#define H_ISN           0x10000000UL
#endif
#define H_LOG           4096

static NX_IP          h_ip;
static NX_INTERFACE   h_iface;
static NX_TCP_SOCKET  h_sock;
static NX_PACKET      h_pkt[H_PACKETS];
static UCHAR          h_pkt_buf[H_PACKETS][H_BUF];
static UINT           h_pkt_next;

/* Every datagram the socket handed the IP layer, by the sequence number of
   its segment: the first H_PACKETS are the original sends, the rest are
   retransmissions. */
static ULONG          h_log[H_LOG];
static ULONG          h_log_bytes[H_LOG];
static NX_INTERFACE  *h_log_interface[H_LOG];
static UCHAR          h_log_first[H_LOG];
static UINT           h_logged;

VOID _nx_ip_packet_send(NX_IP *ip_ptr, NX_PACKET *packet_ptr,
                        ULONG destination_ip, ULONG type_of_service,
                        ULONG time_to_live, ULONG protocol, ULONG fragment,
                        ULONG next_hop_address)
{
    ULONG seq;

    (void)ip_ptr; (void)destination_ip; (void)type_of_service;
    (void)time_to_live; (void)protocol; (void)fragment; (void)next_hop_address;

    h_datagrams++;
    seq = ((NX_TCP_HEADER *)packet_ptr -> nx_packet_prepend_ptr) -> nx_tcp_sequence_number;
    NX_CHANGE_ULONG_ENDIAN(seq);
    if (h_logged < H_LOG)
    {
        ULONG word = ((NX_TCP_HEADER *)packet_ptr -> nx_packet_prepend_ptr) -> nx_tcp_header_word_3;
        NX_CHANGE_ULONG_ENDIAN(word);
        h_log[h_logged] = seq;
        h_log_interface[h_logged] = packet_ptr -> nx_packet_address.nx_packet_interface_ptr;
        h_log_bytes[h_logged] = packet_ptr -> nx_packet_length - (word >> NX_TCP_HEADER_SHIFT) * (ULONG)sizeof(ULONG);
        h_log_first[h_logged] = packet_ptr -> nx_packet_prepend_ptr[(word >> NX_TCP_HEADER_SHIFT) * (ULONG)sizeof(ULONG)];
        h_logged++;
    }

    /* What a SANA-II transmit completion leaves behind, and what the
       retransmit path needs to see before it will send a queued segment
       again.  */
    packet_ptr -> nx_packet_queue_next = (NX_PACKET *)NX_DRIVER_TX_DONE;
#ifdef NX_ENABLE_TCP_SACK
    if (packet_ptr == &h_slice_packet)
    {
        _nx_packet_release(packet_ptr);
    }
#endif
}

VOID _nx_tcp_packet_send_ack(NX_TCP_SOCKET *socket_ptr, ULONG tx_sequence)
{
    (void)socket_ptr;
    (void)tx_sequence;
}

VOID _nx_tcp_packet_send_probe(NX_TCP_SOCKET *socket_ptr, ULONG tx_sequence, UCHAR data)
{
    (void)socket_ptr;
    (void)tx_sequence;
    (void)data;
}

VOID _nx_tcp_packet_send_syn(NX_TCP_SOCKET *socket_ptr, ULONG tx_sequence)
{
    (void)socket_ptr; (void)tx_sequence;
    h_check(0, "RTO data recovery must not send SYN");
}

VOID _nx_tcp_packet_send_fin(NX_TCP_SOCKET *socket_ptr, ULONG tx_sequence)
{
    (void)socket_ptr; (void)tx_sequence;
    h_check(0, "RTO data recovery must not send FIN");
}

VOID _nx_tcp_socket_connection_reset(NX_TCP_SOCKET *socket_ptr)
{
    (void)socket_ptr;
    printf("FAIL the connection was reset\n");
    h_failures++;
}

UINT _nx_tcp_socket_state_wait(NX_TCP_SOCKET *socket_ptr, UINT desired_state,
                               ULONG wait_option)
{
    (void)socket_ptr; (void)desired_state; (void)wait_option;
    return NX_SUCCESS;
}

VOID _nx_tcp_socket_retransmit_queue_flush(NX_TCP_SOCKET *socket_ptr)
{
    (void)socket_ptr;
}

static ULONG h_seq(UINT segment)
{
    return H_ISN + (ULONG)segment * H_MSS;
}

static void h_fixture(void)
{
#ifdef NX_ENABLE_TCP_SACK
    _nx_tcp_sack_recovery_reset(&h_sock);
    h_control_deny = NX_FALSE;
    h_slice_deny = NX_FALSE;
    h_spend_credit_in_append = NX_FALSE;
    h_credit_socket = &h_sock;
    h_check(!h_slice_used, "driver returned the prior transient slice");
    h_control_releases = 0;
#endif
    memset(&h_ip, 0, sizeof(h_ip));
    memset(&h_iface, 0, sizeof(h_iface));

    h_now       = 1000;
    h_datagrams = 0;
    h_pkt_next  = 0;
    h_logged    = 0;

#ifdef NX_ENABLE_TCP_SACK
    h_ip.nx_ip_default_packet_pool = &h_control_pool;
#endif
    h_iface.nx_interface_ip_address = 0xC0A80102UL;

    _nx_tcp_socket_create(&h_ip, &h_sock, "host loss recovery", NX_IP_NORMAL,
                          NX_FRAGMENT_OKAY, 0x80, 8192, NX_NULL, NX_NULL);

    h_sock.nx_tcp_socket_bound_next   = &h_sock;
    h_sock.nx_tcp_socket_client_type  = NX_TRUE;
    h_sock.nx_tcp_socket_state        = NX_TCP_ESTABLISHED;
    h_sock.nx_tcp_socket_port         = 40000;
    h_sock.nx_tcp_socket_connect_port = 5001;
    h_sock.nx_tcp_socket_connect_ip.nxd_ip_version    = NX_IP_VERSION_V4;
    h_sock.nx_tcp_socket_connect_ip.nxd_ip_address.v4 = 0xC0A80101UL;
    h_sock.nx_tcp_socket_connect_interface = &h_iface;
    h_sock.nx_tcp_socket_connect_mss  = H_MSS;
    h_sock.nx_tcp_socket_connect_mss2 = H_MSS * H_MSS;

    /* The peer's window and our congestion window both hold the flight. */
#ifdef NX_ENABLE_TCP_WINDOW_SCALING
    /* h_ack sends 65535 wire units; this flight deliberately has a 1 MiB rwnd. */
    h_sock.nx_tcp_snd_win_scale_value = 4;
#endif
    h_sock.nx_tcp_socket_tx_window_advertised    = 1048576UL;
    h_sock.nx_tcp_socket_tx_window_advertised_max = 1048576UL;
    h_sock.nx_tcp_socket_tx_window_congestion    = 400000UL;
    h_sock.nx_tcp_socket_tx_slow_start_threshold = 400000UL;
    h_sock.nx_tcp_socket_tx_outstanding_bytes    = 0;
    h_sock.nx_tcp_socket_tx_sequence             = H_ISN;
    /* What the SYN leaves behind (nx_tcp_packet_send_syn.c): both
       relative to the ISN, so a flight straddling 2^31 compares right. */
    h_sock.nx_tcp_socket_tx_sequence_recover  = H_ISN - 1;
    h_sock.nx_tcp_socket_previous_highest_ack = H_ISN - 1;
    h_sock.nx_tcp_socket_rx_sequence             = H_ISN_RX;
    h_sock.nx_tcp_socket_transmit_queue_maximum  = H_PACKETS;
#ifdef NX_ENABLE_TCP_SACK
    h_sock.nx_tcp_socket_sack_permitted          = NX_TRUE;
#endif
}

static UINT h_send(void)
{
    NX_PACKET *p;

    if (h_pkt_next >= H_PACKETS)
    {
        printf("FAIL out of test packets\n");
        h_failures++;
        return NX_NO_PACKET;
    }

    p = &h_pkt[h_pkt_next];
    memset(p, 0, sizeof(*p));
    memset(h_pkt_buf[h_pkt_next], 'x', H_BUF);

    p -> nx_packet_data_start  = h_pkt_buf[h_pkt_next];
    p -> nx_packet_data_end    = h_pkt_buf[h_pkt_next] + H_BUF;
    p -> nx_packet_prepend_ptr = h_pkt_buf[h_pkt_next] + NX_PHYSICAL_HEADER +
                                 20 + sizeof(NX_TCP_HEADER);
    p -> nx_packet_append_ptr  = p -> nx_packet_prepend_ptr + H_MSS;
    p -> nx_packet_length      = H_MSS;

    h_pkt_next++;

    return _nx_tcp_socket_send_internal(&h_sock, p, 0);
}

/* One acknowledgment from the peer: the cumulative number, and up to two SACK
   blocks in segment numbers ([from, to) exclusive), the way
   _nx_tcp_packet_process leaves them in the socket before the check. */
static UINT h_ack_with_current_sack(UINT acked_segments)
{
    NX_TCP_HEADER hdr;

    memset(&hdr, 0, sizeof(hdr));
    hdr.nx_tcp_header_word_0         = (5001UL << NX_SHIFT_BY_16) | 40000UL;
    hdr.nx_tcp_sequence_number       = h_sock.nx_tcp_socket_rx_sequence;
    hdr.nx_tcp_acknowledgment_number = h_seq(acked_segments);
    hdr.nx_tcp_header_word_3         = NX_TCP_HEADER_SIZE | NX_TCP_ACK_BIT | 65535UL;

    return _nx_tcp_socket_state_ack_check(&h_sock, &hdr);
}

static UINT h_ack(UINT acked_segments, UINT b1_from, UINT b1_to, UINT b2_from, UINT b2_to)
{
#ifdef NX_ENABLE_TCP_SACK
    h_sock.nx_tcp_socket_sack_block_count = 0;
    if (b1_to > b1_from)
    {
        h_sock.nx_tcp_socket_sack_left[0]  = h_seq(b1_from);
        h_sock.nx_tcp_socket_sack_right[0] = h_seq(b1_to);
        h_sock.nx_tcp_socket_sack_block_count = 1;
    }
    if (b2_to > b2_from)
    {
        h_sock.nx_tcp_socket_sack_left[1]  = h_seq(b2_from);
        h_sock.nx_tcp_socket_sack_right[1] = h_seq(b2_to);
        h_sock.nx_tcp_socket_sack_block_count = 2;
    }
#else
    (void)b1_from; (void)b1_to; (void)b2_from; (void)b2_to;
#endif

    return h_ack_with_current_sack(acked_segments);
}

/* How many times segment `s` was sent after the first H_PACKETS sends, and
   how many datagrams since `from` fell inside [lo, hi). */
static UINT h_resent(UINT s)
{
    UINT i, n = 0;

    for (i = h_pkt_next; i < h_logged; i++)
    {
        if (h_log[i] == h_seq(s))
        {
            n++;
        }
    }
    return n;
}

static UINT h_sent_in(UINT from, UINT lo, UINT hi)
{
    UINT i, n = 0;

    for (i = from; i < h_logged; i++)
    {
        if (((INT)(h_log[i] - h_seq(lo))) >= 0 && ((INT)(h_seq(hi) - h_log[i])) > 0)
        {
            n++;
        }
    }
    return n;
}

/*
 * The capture: 130 in flight, segments 10..68 lost, 69..129 delivered.
 */
#define H_FLIGHT        130
#define H_HOLE_FROM     10
#define H_HOLE_TO       69

static void burst_hole(void)
{
    UINT i, before, first_walk, spurious, s;

    printf("a 59-segment hole in a flight of 130\n");

    h_fixture();
    for (i = 0; i < H_FLIGHT; i++)
    {
        (VOID)h_send();
    }
    h_check_eq(h_sock.nx_tcp_socket_transmit_sent_count, H_FLIGHT, "the flight is on the queue");
    h_check_eq(h_datagrams, H_FLIGHT, "and went out once each");

    /* The first ten arrive and are acknowledged. */
    (VOID)h_ack(H_HOLE_FROM, 0, 0, 0, 0);
    h_check_eq(h_sock.nx_tcp_socket_transmit_sent_count, H_FLIGHT - H_HOLE_FROM, "ten released");

    /* Three segments past the hole arrive: three duplicate acknowledgments,
       each with the block the peer holds. */
    before = h_datagrams;
    (VOID)h_ack(H_HOLE_FROM, H_HOLE_TO, H_HOLE_TO + 1, 0, 0);
    (VOID)h_ack(H_HOLE_FROM, H_HOLE_TO, H_HOLE_TO + 2, 0, 0);
    h_check_eq(h_datagrams - before, 0, "two duplicates retransmit nothing");
    (VOID)h_ack(H_HOLE_FROM, H_HOLE_TO, H_HOLE_TO + 3, 0, 0);
    first_walk = h_datagrams - before;
    printf("  third duplicate     %u segment(s) retransmitted, %u in the hole\n",
           first_walk, h_sent_in(H_FLIGHT, H_HOLE_FROM, H_HOLE_TO));
    h_check(h_sock.nx_tcp_socket_fast_recovery == NX_TRUE, "fast recovery entered");
    h_check(h_resent(H_HOLE_FROM) == 1, "the first lost segment went out again");
    h_check_eq(h_sent_in(H_FLIGHT, H_HOLE_TO, H_FLIGHT), 0, "nothing the peer holds was resent");

    /*
     * RFC 6675 section 5: the peer has said, three segments over, that
     * everything below its block is missing, and what is missing is not in
     * the network.  With 58 segments still legitimately in flight beyond the
     * block the pipe leaves room for a few of the hole now; each further
     * duplicate acknowledgment takes one out of the pipe and adds one to the
     * window, so the hole drains as the blocks grow -- all of it before a
     * single partial acknowledgment, where the old rule sent one segment per
     * partial acknowledgment and a round trip each.
     */
    h_check(first_walk >= 2,
            "the third duplicate acknowledgment retransmits more than one segment");

    /* The retransmitted copies are still in the network. The next SACK
       removes one original segment from the pipe and grants bounded PRR-SSRB
       credit, so it permits at most two more copies, not another whole window.
       No retransmitted byte has been cumulatively acknowledged yet. */
    before = h_datagrams;
    (VOID)h_ack(H_HOLE_FROM, H_HOLE_TO, H_HOLE_TO + 4, 0, 0);
    h_check(h_datagrams - before <= 2,
            "the next SACK counts retransmissions already in flight");
    h_check((H_FLIGHT - H_HOLE_TO - 4 +
             h_sent_in(H_FLIGHT, H_HOLE_FROM, H_HOLE_TO)) * H_MSS <=
                h_sock.nx_tcp_socket_tx_window_congestion,
            "original and retransmitted copies together stay within cwnd");

    /* The rest of the delivered flight is reported, one block growing. */
    for (i = H_HOLE_TO + 5; i <= H_FLIGHT; i++)
    {
        (VOID)h_ack(H_HOLE_FROM, H_HOLE_TO, i, 0, 0);
    }
    printf("  growing block       %u of the hole resent by the last duplicate acknowledgment\n",
           h_sent_in(H_FLIGHT, H_HOLE_FROM, H_HOLE_TO));
    h_check_eq(h_sent_in(H_FLIGHT, H_HOLE_FROM, H_HOLE_TO), H_HOLE_TO - H_HOLE_FROM,
               "the whole hole has left by the time the peer has reported all it holds");
    h_check_eq(h_sent_in(H_FLIGHT, H_HOLE_TO, H_FLIGHT), 0, "and nothing the peer holds");

    /* The retransmissions land, in order: one cumulative acknowledgment per
       segment, then the jump over what was held. */
    before = h_datagrams;
    for (s = H_HOLE_FROM + 1; s < H_HOLE_TO; s++)
    {
        (VOID)h_ack(s, H_HOLE_TO, H_FLIGHT, 0, 0);
    }
    spurious = h_datagrams - before;
    printf("  partial acks        %u segment(s) resent during %u partial acknowledgments\n",
           spurious, H_HOLE_TO - H_HOLE_FROM - 1);
    h_check_eq(spurious, 0, "a partial acknowledgment of a hole already resent resends nothing");

    (VOID)h_ack(H_FLIGHT, 0, 0, 0, 0);
    h_check_eq(h_sock.nx_tcp_socket_transmit_sent_count, 0, "the queue is empty");
    h_check_eq(h_sock.nx_tcp_socket_tx_outstanding_bytes, 0, "nothing outstanding");
    h_check(h_sock.nx_tcp_socket_fast_recovery == NX_FALSE, "fast recovery left");
    for (s = H_HOLE_FROM; s < H_HOLE_TO; s++)
    {
        if (h_resent(s) != 1)
        {
            printf("  segment %u resent %u times\n", s, h_resent(s));
            h_failures++;
            break;
        }
    }
    h_checks++;
    printf("  total               %u datagrams for %u segments and a hole of %u\n",
           h_datagrams, H_FLIGHT, H_HOLE_TO - H_HOLE_FROM);
}

/*
 * One lost segment, the common case: exactly one retransmission, and a
 * batch of acknowledgments arriving together does not resend what its later
 * members cover.
 */
static void single_loss(void)
{
    UINT i, before;

    printf("one lost segment in a flight of 130\n");

    h_fixture();
    for (i = 0; i < H_FLIGHT; i++)
    {
        (VOID)h_send();
    }
    (VOID)h_ack(10, 0, 0, 0, 0);
    before = h_datagrams;
    for (i = 12; i <= 40; i++)
    {
        (VOID)h_ack(10, 11, i, 0, 0);
    }
    h_check_eq(h_datagrams - before, 1, "one lost segment is retransmitted exactly once");
    h_check(h_resent(10) == 1, "and it is the lost one");

    /* It lands: the acknowledgment jumps over everything held. */
    before = h_datagrams;
    (VOID)h_ack(41, 0, 0, 0, 0);
    h_check_eq(h_datagrams - before, 0, "the jump resends nothing");
    h_check_eq(h_sock.nx_tcp_socket_transmit_sent_count, H_FLIGHT - 41, "and releases the run");

    /* The rest arrives in order, acknowledged in a coalesced batch. */
    before = h_datagrams;
    for (i = 42; i <= H_FLIGHT; i++)
    {
        (VOID)h_ack(i, 0, 0, 0, 0);
    }
    h_check_eq(h_datagrams - before, 0, "in-order acknowledgments resend nothing");
    h_check_eq(h_sock.nx_tcp_socket_transmit_sent_count, 0, "the queue is empty");
    h_check(h_sock.nx_tcp_socket_fast_recovery == NX_FALSE, "fast recovery left");
}

/*
 * Two losses with the second one at the tail.  Once the retransmitted
 * interior segment arrives, the cumulative acknowledgment consumes the SACK
 * block for everything beyond it.  No block remains to describe the tail --
 * but the sole outstanding segment is still the recovery point, and leaving
 * it alone means waiting for the RTO.
 */
static void second_loss_at_the_tail(void)
{
    UINT i, before;

    printf("two losses, the second at the tail\n");

    h_fixture();
    for (i = 0; i < 10; i++)
    {
        (VOID)h_send();
    }

    /* Segment 2 is lost; 3..8 arrive, while segment 9 is lost at the tail. */
    (VOID)h_ack(2, 0, 0, 0, 0);
    (VOID)h_ack(2, 3, 4, 0, 0);
    (VOID)h_ack(2, 3, 5, 0, 0);
    (VOID)h_ack(2, 3, 9, 0, 0);
    h_check(h_resent(2) == 1, "the interior loss enters fast recovery");

    /* Filling it advances through segment 8.  The tail is now the only byte
       range outstanding and there is no SACK block left to report. */
    before = h_datagrams;
    (VOID)h_ack(9, 0, 0, 0, 0);
    h_check_eq(h_datagrams - before, 1,
               "the partial acknowledgment retransmits the sole tail segment");
    h_check(h_resent(9) == 1, "the tail does not wait for the RTO");
#ifdef NX_ENABLE_TCP_SACK
    h_check_eq(h_sock.nx_tcp_socket_tx_high_rxt, h_seq(3), "tail rescue does not advance HighRxt");
    h_check_eq(h_sock.nx_tcp_socket_sack_rescue_rxt, h_seq(10), "tail rescue is bounded by RecoveryPoint");
#endif
}

/*
 * Audit N-031: two holes, the second far into the flight.  The
 * retransmission of the first lands and the cumulative acknowledgment jumps
 * to the second: a partial acknowledgment of more data than the recovery
 * window holds.  The window is deflated by what was acknowledged and must
 * stop at zero, not wrap; RFC 6582 then adds one segment back.
 */
static void two_holes_partial_ack(UINT dups)
{
    UINT  i;
    ULONG before, after;

    printf("two holes, segments 1 and 16 of 20, %u duplicate acknowledgment(s)\n", dups);

    h_fixture();
    for (i = 0; i < 20; i++)
    {
        (VOID)h_send();
    }
    (VOID)h_ack(1, 0, 0, 0, 0);
    for (i = 0; i < dups; i++)
    {
        (VOID)h_ack(1, 2, 3 + i, 0, 0);
    }
    h_check(h_sock.nx_tcp_socket_fast_recovery == NX_TRUE, "fast recovery entered");

    before = h_sock.nx_tcp_socket_tx_window_congestion;
    (VOID)h_ack(16, 17, 20, 0, 0);
    after = h_sock.nx_tcp_socket_tx_window_congestion;
    printf("  window %lu before the partial acknowledgment of %lu, %lu after\n",
           (unsigned long)before, (unsigned long)(15 * H_MSS), (unsigned long)after);
    h_check(h_sock.nx_tcp_socket_fast_recovery == NX_TRUE, "still in recovery: 16 is below the recovery point");
    h_check(after <= before + H_MSS, "a partial acknowledgment does not grow the window past one segment");
}

/* #354: a timeout in fast recovery must resend SND.UNA even when HighRxt
   already covers it.  The remaining hole follows the advancing ACK clock,
   with no additional timer ticks, no repeated batch sends and no retry
   inflation.  The old path repaired one packet per RTO. */
static void timeout_hole(void)
{
    UINT i, before;
    ULONG timer;

    printf("timeout in fast recovery, then advancing ACKs\n");
    h_fixture();
    for (i = 0; i < 8; i++)
    {
        (VOID)h_send();
    }
    h_sock.nx_tcp_socket_fast_recovery = NX_TRUE;
#ifdef NX_ENABLE_TCP_SACK
    h_sock.nx_tcp_socket_sack_block_count = 1;
    h_sock.nx_tcp_socket_sack_left[0] = h_seq(1);
    h_sock.nx_tcp_socket_sack_right[0] = h_seq(8);
    h_sock.nx_tcp_socket_tx_high_rxt = h_seq(8);
#endif
    h_sock.nx_tcp_socket_timeout = _nx_tcp_fast_timer_rate;
    before = h_datagrams;
    _nx_tcp_fast_periodic_processing(&h_ip);
    h_check_eq(h_datagrams - before, 1, "RTO resends one segment");
    h_check_eq(h_resent(0), 1, "RTO ignores obsolete HighRxt/SACK and resends SND.UNA");
    h_check_eq(h_sock.nx_tcp_socket_tx_window_congestion, H_MSS, "RTO leaves one MSS congestion window");
    h_check_eq(h_sock.nx_tcp_socket_timeout_retries, 1, "RTO counts one retry");
#ifdef NX_ENABLE_TCP_SACK
    h_check_eq(h_sock.nx_tcp_socket_sack_block_count, 0, "RTO discards stale SACK state");
#endif
    before = h_datagrams;
    (VOID)h_ack(1, 0, 0, 0, 0);
    h_check_eq(h_datagrams - before, 2, "advancing ACK immediately resends two segments in slow start");
    h_check_eq(h_sock.nx_tcp_socket_timeout_retries, 0, "ACK resends do not increase retries");
    timer = h_sock.nx_tcp_socket_timeout;
    before = h_datagrams;
    (VOID)h_ack(1, 0, 0, 0, 0);
    h_check_eq(h_datagrams, before, "duplicate ACK does not repeat cursor's resends");
    h_check_eq(h_sock.nx_tcp_socket_timeout, timer, "duplicate ACK leaves timer alone");
    for (i = 2; i <= 8; i++)
    {
        (VOID)h_ack(i, 0, 0, 0, 0);
    }
    for (i = 0; i < 8; i++)
    {
        h_check_eq(h_resent(i), 1, "each old segment resent exactly once without another timer tick");
    }
    h_check_eq(h_sock.nx_tcp_socket_transmit_sent_count, 0, "RTO old flight completely acknowledged");

    h_fixture();
    for (i = 0; i < 8; i++)
    {
        (VOID)h_send();
    }
    h_sock.nx_tcp_socket_state = NX_TCP_FIN_WAIT_1;
    h_sock.nx_tcp_socket_tx_sequence++;
    h_sock.nx_tcp_socket_timeout = _nx_tcp_fast_timer_rate;
    _nx_tcp_fast_periodic_processing(&h_ip);
    before = h_datagrams;
    (VOID)h_ack(1, 0, 0, 0, 0);
    h_check((h_datagrams - before) * H_MSS <= h_sock.nx_tcp_socket_tx_window_congestion &&
            (h_datagrams - before + 1) * H_MSS > h_sock.nx_tcp_socket_tx_window_congestion,
            "FIN recovery fills, and does not exceed, the current congestion window");
    h_check_eq(h_resent(1), 1, "FIN does not make the cursor skip the first unacknowledged segment");

    h_fixture();
    for (i = 0; i < 8; i++)
    {
        (VOID)h_send();
    }
    h_sock.nx_tcp_socket_tx_window_advertised = H_MSS / 2;
    h_sock.nx_tcp_socket_timeout = _nx_tcp_fast_timer_rate;
    before = h_datagrams;
    _nx_tcp_fast_periodic_processing(&h_ip);
    h_check_eq(h_datagrams - before, 1, "RTO resends old data despite a shrunken nonzero window");
    h_sock.nx_tcp_socket_timeout = _nx_tcp_fast_timer_rate;
    _nx_tcp_fast_periodic_processing(&h_ip);
    h_check_eq(h_datagrams - before, 2, "small receiver window cannot suppress subsequent RTOs");

    h_fixture();
    for (i = 0; i < 8; i++)
    {
        (VOID)h_send();
    }
    h_sock.nx_tcp_socket_timeout = _nx_tcp_fast_timer_rate;
    _nx_tcp_fast_periodic_processing(&h_ip);
    (VOID)h_ack(1, 0, 0, 0, 0);
    (VOID)h_ack(1, 0, 0, 0, 0);
    (VOID)h_ack(1, 0, 0, 0, 0);
    (VOID)h_ack(1, 0, 0, 0, 0);
    h_check_eq(h_sock.nx_tcp_socket_fast_recovery, NX_TRUE, "duplicate ACKs enter new fast recovery after RTO");
    h_check_eq(h_sock.nx_tcp_socket_tx_rto_next, h_sock.nx_tcp_socket_tx_rto_end,
               "fast recovery supersedes timeout mode");
    before = h_datagrams;
    (VOID)h_ack(8, 0, 0, 0, 0);
    h_check_eq(h_datagrams, before, "fast recovery exit cannot revive stale timeout sends");

    h_fixture();
    for (i = 0; i < 8; i++)
    {
        (VOID)h_send();
    }
    h_sock.nx_tcp_socket_timeout = _nx_tcp_fast_timer_rate;
    _nx_tcp_fast_periodic_processing(&h_ip);
    h_sock.nx_tcp_socket_tx_rto_end = h_seq(10);
    before = h_datagrams;
    (VOID)h_ack(1, 0, 0, 0, 0);
    h_check_eq(h_datagrams, before, "invalid old-flight bound cannot underflow send credit");
    h_check_eq(h_sock.nx_tcp_socket_tx_rto_next, h_sock.nx_tcp_socket_tx_rto_end,
               "invalid timeout cursor is retired");
}

#ifdef NX_ENABLE_TCP_SACK
static void timeout_fresh_sack_tail(void)
{
    UINT scenario, i, before;
    ULONG timer;

    printf("fresh SACK tail after timeout, withdrawal and next timeout\n");
    for (scenario = 0; scenario < 3; scenario++)
    {
        h_fixture();
        for (i = 0; i < 8; i++) { (VOID)h_send(); }
        h_sock.nx_tcp_socket_sack_block_count = 1;
        h_sock.nx_tcp_socket_sack_left[0] = h_seq(4);
        h_sock.nx_tcp_socket_sack_right[0] = h_seq(8);
        h_sock.nx_tcp_socket_timeout = _nx_tcp_fast_timer_rate;
        _nx_tcp_fast_periodic_processing(&h_ip);
        h_check_eq(h_sock.nx_tcp_socket_sack_block_count, 0, "timeout forgets old tail SACK");
        (VOID)h_ack(1, 4, 8, 0, 0);
        (VOID)h_ack_with_current_sack(2);
        (VOID)h_ack_with_current_sack(3);
        h_check_eq(h_sock.nx_tcp_socket_sack_block_count, 1, "plain ACK below cached left edge keeps tail evidence");
        h_check_eq(h_datagrams - h_pkt_next, 4, "fresh tail SACK avoids all three duplicate copies");
        for (i = 0; i < 4; i++)
        {
            h_check_eq(h_resent(i), 1, "fresh SACK never hides the missing prefix");
        }
        h_check_eq(h_sock.nx_tcp_socket_tx_rto_next, h_seq(4), "SACKed tail keeps its recovery cursor");
        before = h_datagrams;
        timer = h_sock.nx_tcp_socket_timeout;
        _nx_tcp_socket_retransmit_after_timeout(&h_ip, &h_sock);
        h_check_eq(h_datagrams, before, "repeated fresh SACK check sends nothing");
        h_check_eq(h_sock.nx_tcp_socket_timeout, timer, "fresh SACK check does not change timer");
        if (scenario == 0)
        {
            (VOID)h_ack(8, 0, 0, 0, 0);
            h_check_eq(h_datagrams, before, "cumulative ACK completes SACKed tail without copies");
        }
        else
        {
            if (scenario == 2)
            {
                h_sock.nx_tcp_socket_timeout = _nx_tcp_fast_timer_rate;
                _nx_tcp_fast_periodic_processing(&h_ip);
                h_check_eq(h_datagrams, before + 1, "next RTO still resends unacknowledged head");
                h_check_eq(h_sock.nx_tcp_socket_sack_block_count, 0, "next RTO discards fresh tail SACK too");
            }
            before = h_datagrams;
            /* A plain wire ACK does not invoke the option parser.  Keep
               the old SACK here and require the ACK path to reject it. */
            (VOID)h_ack_with_current_sack(4);
            h_check(h_datagrams > before && h_resent(4) == 1, "withdrawn tail SACK resumes queued recovery");
            h_check_eq(h_sock.nx_tcp_socket_sack_block_count, 0, "ACK entering cached SACK block invalidates that claim");
            for (i = 5; i <= 8; i++) { (VOID)h_ack(i, 0, 0, 0, 0); }
        }
        h_check_eq(h_sock.nx_tcp_socket_transmit_sent_count, 0, "tail path releases all acknowledged packets");
    }

    h_fixture();
    for (i = 0; i < 8; i++) { (VOID)h_send(); }
    h_sock.nx_tcp_socket_timeout = _nx_tcp_fast_timer_rate;
    _nx_tcp_fast_periodic_processing(&h_ip);
    for (i = 1; i <= 8; i++) { (VOID)h_ack(i, 4, 6, 0, 0); }
    h_check_eq(h_resent(6), 1, "interior SACK cannot hide a missing later segment");
    h_check_eq(h_resent(7), 1, "interior SACK cannot stop before the missing tail");
    h_check_eq(h_sock.nx_tcp_socket_transmit_sent_count, 0, "interior SACK path completes without another timer");
}
#endif /* NX_ENABLE_TCP_SACK */

static void fin_ack_sequence(void)
{
    UINT states[] = {NX_TCP_FIN_WAIT_1, NX_TCP_CLOSING, NX_TCP_LAST_ACK};
    UINT state, i;
    ULONG before;
    NX_TCP_HEADER header;

    for (state = 0; state < sizeof(states) / sizeof(states[0]); state++)
    {
        h_fixture();
        for (i = 0; i < 8; i++) { (VOID)h_send(); }
        h_sock.nx_tcp_socket_state = states[state];
        h_sock.nx_tcp_socket_tx_sequence++;
        for (i = 0; i < NX_TCP_FAST_RETRANSMIT_THRESHOLD; i++) { (VOID)h_ack(0, 0, 0, 0, 0); }
        h_check_eq(h_sock.nx_tcp_socket_duplicated_ack_received, NX_TCP_FAST_RETRANSMIT_THRESHOLD,
                   "pending FIN preserves duplicate ACK sequence");
        h_check(h_sock.nx_tcp_socket_fast_recovery == NX_TRUE && h_resent(0) == 1,
                "pending FIN permits fast retransmit of missing DATA");

        h_fixture();
        for (i = 0; i < 8; i++) { (VOID)h_send(); }
        h_sock.nx_tcp_socket_state = states[state];
        h_sock.nx_tcp_socket_tx_sequence++;
        h_sock.nx_tcp_socket_tx_window_congestion = H_MSS;
        (VOID)h_ack(1, 0, 0, 0, 0);
        h_check_eq(h_sock.nx_tcp_socket_tx_window_congestion, 2 * H_MSS,
                   "pending FIN does not reduce DATA ACK congestion credit");
        h_check_eq(h_sock.nx_tcp_socket_previous_highest_ack, H_ISN,
                   "previous ACK records DATA sequence before FIN");
        (VOID)h_ack(8, 0, 0, 0, 0);
        h_check(h_sock.nx_tcp_socket_tx_outstanding_bytes == 0 &&
                h_sock.nx_tcp_socket_transmit_sent_count == 0 && h_sock.nx_tcp_socket_fin_acked == NX_FALSE,
                "DATA-only completion leaves FIN outstanding");
        h_check(h_sock.nx_tcp_socket_timeout != 0, "pending FIN retains retry timer");
        before = h_sock.nx_tcp_socket_tx_window_congestion;
        memset(&header, 0, sizeof(header));
        header.nx_tcp_sequence_number = h_sock.nx_tcp_socket_rx_sequence;
        header.nx_tcp_acknowledgment_number = h_seq(8) + 1;
        header.nx_tcp_header_word_3 = NX_TCP_HEADER_SIZE | NX_TCP_ACK_BIT | 65535UL;
        (VOID)_nx_tcp_socket_state_ack_check(&h_sock, &header);
        h_check(h_sock.nx_tcp_socket_fin_acked == NX_TRUE &&
                h_sock.nx_tcp_socket_tx_window_congestion == before,
                "FIN-only ACK does not earn DATA congestion credit");

        h_fixture();
        for (i = 0; i < 8; i++) { (VOID)h_send(); }
        h_sock.nx_tcp_socket_state = states[state];
        h_sock.nx_tcp_socket_tx_sequence++;
        h_sock.nx_tcp_socket_tx_window_congestion = 8 * H_MSS + 1;
        h_sock.nx_tcp_socket_tx_slow_start_threshold = H_MSS;
        header.nx_tcp_sequence_number = h_sock.nx_tcp_socket_rx_sequence;
        (VOID)_nx_tcp_socket_state_ack_check(&h_sock, &header);
        h_check(h_sock.nx_tcp_socket_tx_cwnd_acked == 8 * H_MSS &&
                h_sock.nx_tcp_socket_tx_window_congestion == 8 * H_MSS + 1,
                "combined DATA and FIN ACK credits only DATA bytes");
    }
}

#ifdef NX_ENABLE_TCP_SACK
static void fin_sack_option_sequence(void)
{
    UINT states[] = {NX_TCP_ESTABLISHED, NX_TCP_CLOSE_WAIT, NX_TCP_FIN_WAIT_1, NX_TCP_CLOSING, NX_TCP_LAST_ACK};
    UINT state, i;
    UCHAR option[10];
    ULONG left = H_ISN + 1;
    ULONG right = h_seq(4);

    for (state = 0; state < sizeof(states) / sizeof(states[0]); state++)
    {
        h_fixture();
        h_sock.nx_tcp_socket_state = states[state];
        h_sock.nx_tcp_socket_tx_outstanding_bytes = 8 * H_MSS;
        h_sock.nx_tcp_socket_tx_sequence = h_seq(8);
        if (states[state] >= NX_TCP_FIN_WAIT_1) { h_sock.nx_tcp_socket_tx_sequence++; }
        option[0] = NX_TCP_SACK_KIND;
        option[1] = sizeof(option);
        for (i = 0; i < 4; i++)
        {
            option[2 + i] = (UCHAR)(left >> (24 - i * 8));
            option[6 + i] = (UCHAR)(right >> (24 - i * 8));
        }
        _nx_tcp_sack_option_get(&h_sock, option, sizeof(option));
        h_check(h_sock.nx_tcp_socket_sack_block_count == 1 &&
                h_sock.nx_tcp_socket_sack_left[0] == left && h_sock.nx_tcp_socket_sack_right[0] == right,
                "wire SACK beyond one-byte hole remains valid after FIN");
        for (i = 0; i < 4; i++) { option[2 + i] = (UCHAR)(H_ISN >> (24 - i * 8)); }
        _nx_tcp_sack_option_get(&h_sock, option, sizeof(option));
        h_check_eq(h_sock.nx_tcp_socket_sack_block_count, 0,
                   "wire SACK at cumulative ACK remains invalid after FIN");
    }
}

static void fin_partial_ack_sequence(void)
{
    UINT states[] = {NX_TCP_ESTABLISHED, NX_TCP_CLOSE_WAIT, NX_TCP_FIN_WAIT_1, NX_TCP_CLOSING, NX_TCP_LAST_ACK};
    UINT state, i, before;

    for (state = 0; state < sizeof(states) / sizeof(states[0]); state++)
    {
        h_fixture();
        for (i = 0; i < 8; i++) { (VOID)h_send(); }
        h_sock.nx_tcp_socket_state = states[state];
        if (states[state] >= NX_TCP_FIN_WAIT_1) { h_sock.nx_tcp_socket_tx_sequence++; }
        h_sock.nx_tcp_socket_fast_recovery = NX_TRUE;
        h_sock.nx_tcp_socket_tx_sequence_recover = h_sock.nx_tcp_socket_tx_sequence - 1;
        h_sock.nx_tcp_socket_tx_high_rxt = h_seq(1);
        before = h_datagrams;
        (VOID)h_ack(6, 0, 0, 0, 0);
        h_check_eq(h_datagrams, before, "partial ACK with two segments left does not resend DATA");
        (VOID)h_ack(7, 0, 0, 0, 0);
        h_check_eq(h_datagrams, before, "one-MSS partial ACK does not spuriously resend FIN recovery tail");
        (VOID)h_ack(8, 0, 0, 0, 0);
        h_check_eq(h_sock.nx_tcp_socket_transmit_sent_count, 0,
                   "partial ACK tail completes without a retransmission");
    }
}
#endif /* NX_ENABLE_TCP_SACK */

#ifdef NX_ENABLE_TCP_SACK
static void prr_burst_example(void)
{
    UINT i, before;
    h_fixture();
    for (i = 0; i < 20; i++) { (VOID)h_send(); }
    (VOID)h_ack(0, 15, 16, 0, 0);
    (VOID)h_ack(0, 15, 17, 0, 0);
    before = h_datagrams;
    (VOID)h_ack(0, 15, 18, 0, 0);
    h_check_eq(h_datagrams - before, 2, "RFC 6937 20-flight/15-loss example sends two repairs on entry");
    before = h_datagrams;
    (VOID)h_ack(0, 15, 19, 0, 0);
    h_check_eq(h_datagrams - before, 2, "PRR-SSRB clocks two repairs on the fourth ACK");
    before = h_datagrams;
    (VOID)h_ack(0, 15, 20, 0, 0);
    h_check_eq(h_datagrams - before, 2, "PRR-SSRB clocks two repairs on the fifth ACK");
    for (i = 1; i < 15; i++)
    {
        (VOID)h_ack(i, 15, 20, 0, 0);
        h_check_eq(h_sock.nx_tcp_socket_tx_window_congestion, 10 * H_MSS,
                   "partial ACKs preserve the recovery target instead of NewReno deflation");
    }
    for (i = 0; i < 15; i++)
    {
        h_check(h_resent(i) >= 1, "every burst-loss hole is repaired without another timeout");
    }
    h_check_eq(h_sock.nx_tcp_socket_timeout_retries, 0, "ACK-clocked repairs are not RTO retries");
    (VOID)h_ack(20, 0, 0, 0, 0);
    h_check(!NX_TCP_SACK_RECOVERY_ACTIVE(&h_sock), "RecoveryPoint ACK exits PRR");
    h_check_eq(h_control_used, NX_FALSE, "completed flight returns its scoreboard packet");
}

static void prr_receive_window_bound(void)
{
    UINT i, before;
    h_fixture();
    for (i = 0; i < 20; i++) { (VOID)h_send(); }
    (VOID)h_ack(0, 10, 11, 0, 0);
    (VOID)h_ack(0, 10, 12, 0, 0);
    h_sock.nx_tcp_socket_tx_window_advertised = 2 * H_MSS;
    before = h_logged;
    (VOID)h_ack(0, 10, 13, 0, 0);
    for (i = before; i < h_logged; i++)
    {
        h_check((ULONG)(h_log[i] - H_ISN) + h_log_bytes[i] <= 2 * H_MSS,
                "retransmission honors the current peer window edge");
    }
    _nx_tcp_sack_recovery_reset(&h_sock);
}

static void newreno_peer_unchanged(void)
{
    UINT i;
    ULONG window;
    h_fixture();
    h_sock.nx_tcp_socket_sack_permitted = NX_FALSE;
    for (i = 0; i < 10; i++) { (VOID)h_send(); }
    for (i = 0; i < 3; i++) { (VOID)h_ack(0, 0, 0, 0, 0); }
    h_check(h_sock.nx_tcp_socket_fast_recovery, "non-SACK peer still enters NewReno");
    window = h_sock.nx_tcp_socket_tx_window_congestion;
    (VOID)h_ack(0, 0, 0, 0, 0);
    h_check_eq(h_sock.nx_tcp_socket_tx_window_congestion, window + H_MSS,
               "non-SACK duplicate ACK retains NewReno inflation");
    window += H_MSS;
    (VOID)h_ack(3, 0, 0, 0, 0);
    h_check_eq(h_sock.nx_tcp_socket_tx_window_congestion, window - 2 * H_MSS,
               "non-SACK partial ACK retains NewReno deflation");
    h_check_eq(h_control_used, NX_FALSE, "non-SACK peer allocates no scoreboard");
}

static void prr_credit_across_allocation(void)
{
    NX_PACKET source;
    UCHAR data[3200];
    UINT i, before;
    UCHAR *prepend;
    h_fixture();
    for (i = 0; i < 10; i++) { (VOID)h_send(); }
    h_sock.nx_tcp_socket_sack_block_count = 1;
    h_sock.nx_tcp_socket_sack_left[0] = h_seq(4);
    h_sock.nx_tcp_socket_sack_right[0] = h_seq(10);
    _nx_tcp_sack_recovery_ack(&h_sock, H_ISN);
    _nx_tcp_sack_recovery_begin(&h_sock);
    memset(&source, 0, sizeof(source));
    memset(data, 'd', sizeof(data));
    source.nx_packet_data_start = data;
    source.nx_packet_data_end = data + sizeof(data);
    source.nx_packet_prepend_ptr = data + 80;
    source.nx_packet_append_ptr = source.nx_packet_prepend_ptr + 2 * H_MSS;
    source.nx_packet_length = 2 * H_MSS;
    source.nx_packet_pool_owner = &h_control_pool;
    prepend = source.nx_packet_prepend_ptr;
    h_sock.nx_tcp_socket_prr_credit = H_MSS;
    h_spend_credit_in_append = NX_TRUE;
    before = h_logged;
    h_check_eq(_nx_tcp_socket_send_internal(&h_sock, &source, NX_NO_WAIT), NX_WINDOW_OVERFLOW,
               "credit spent across allocation yield returns normal nonblocking backpressure");
    h_check_eq(h_logged, before, "sender cannot overspend credit consumed while IP mutex was released");
    h_check_eq(h_sock.nx_tcp_socket_prr_out, H_MSS, "concurrent repair is charged once without a second DATA debit");
    h_check(source.nx_packet_length == 2 * H_MSS && source.nx_packet_prepend_ptr == prepend,
            "failed revalidation preserves caller packet ownership and DATA");
    h_check_eq(h_slice_used, NX_FALSE, "failed revalidation releases only the prepared copy");
    h_sock.nx_tcp_socket_prr_credit = H_MSS;
    h_spend_credit_in_append = 2;
    h_check_eq(_nx_tcp_socket_send_internal(&h_sock, &source, NX_NO_WAIT), NX_NOT_CONNECTED,
               "teardown during allocation cannot enqueue prepared DATA");
    h_check_eq(h_logged, before, "connection teardown sends no prepared DATA");
    h_check(source.nx_packet_length == 2 * H_MSS && source.nx_packet_prepend_ptr == prepend && !h_slice_used,
            "teardown preserves caller DATA and reclaims its transient copy");
    _nx_tcp_sack_recovery_reset(&h_sock);
}

static void prr_partial_credit_slice(void)
{
    UINT i, before;
    ULONG sent, sequence;
    NX_PACKET extra;
    UCHAR extra_data[1000];
    UCHAR *original_prepend;
    h_fixture();
    for (i = 0; i < 10; i++) { (VOID)h_send(); }
    h_sock.nx_tcp_socket_sack_block_count = 1;
    h_sock.nx_tcp_socket_sack_left[0] = h_seq(4);
    h_sock.nx_tcp_socket_sack_right[0] = h_seq(10);
    _nx_tcp_sack_recovery_ack(&h_sock, H_ISN);
    _nx_tcp_sack_recovery_begin(&h_sock);
    original_prepend = h_pkt[0].nx_packet_prepend_ptr;
    for (i = 0; i < 2; i++)
    {
        h_sock.nx_tcp_socket_prr_credit = 333;
        before = h_logged;
        sent = h_sock.nx_tcp_socket_prr_out;
        _nx_tcp_socket_retransmit(&h_ip, &h_sock, NX_FALSE);
        h_check_eq(h_logged - before, 1, "partial PRR credit produces one bounded slice");
        h_check_eq(h_log[before], H_ISN + 333 * i, "successive slices advance through the same queued segment");
        h_check_eq(h_log_bytes[before], 333, "slice never exceeds actual PRR byte credit");
        h_check(h_log_interface[before] == &h_iface,
                "partial repair retains the original IPv4 egress interface");
        h_check_eq(h_sock.nx_tcp_socket_prr_out - sent, 333, "actual slice bytes debit shared credit");
        h_check_eq(h_sock.nx_tcp_socket_prr_credit, 0, "partial slice consumes its exact budget");
        h_check(h_pkt[0].nx_packet_prepend_ptr == original_prepend &&
                h_pkt[0].nx_packet_length == H_MSS + sizeof(NX_TCP_HEADER),
                "transient slice preserves queued original bytes and headers");
    }
    h_slice_deny = NX_TRUE;
    h_sock.nx_tcp_socket_prr_credit = 333;
    before = h_logged;
    sent = h_sock.nx_tcp_socket_prr_out;
    sequence = h_sock.nx_tcp_socket_tx_high_rxt;
    _nx_tcp_socket_retransmit(&h_ip, &h_sock, NX_FALSE);
    h_check_eq(h_logged, before, "slice allocation failure does not send or wait");
    h_check_eq(h_sock.nx_tcp_socket_prr_out, sent, "allocation failure spends no output credit");
    h_check_eq(h_sock.nx_tcp_socket_tx_high_rxt, sequence, "allocation failure preserves retransmission progress");
    h_slice_deny = NX_FALSE;
    memset(&extra, 0, sizeof(extra));
    memset(extra_data, 'c', sizeof(extra_data));
    extra.nx_packet_prepend_ptr = extra_data;
    extra.nx_packet_append_ptr = extra_data + (H_MSS - 500);
    h_pkt[0].nx_packet_append_ptr = original_prepend + sizeof(NX_TCP_HEADER) + 500;
    h_pkt[0].nx_packet_next = &extra;
    _nx_tcp_socket_retransmit(&h_ip, &h_sock, NX_FALSE);
    h_check_eq(h_logged - before, 1, "retry with available storage resumes the unspent slice");
    h_check_eq(h_log[before], H_ISN + 666, "chained source uses correct byte offset");
    h_check_eq(h_log_first[before], 'c', "slice copies DATA from the chained buffer");
    h_check_eq(h_log_bytes[before], 333, "chained slice preserves exact credit bound");
    h_check(h_log_interface[before] == &h_iface,
            "chained repair retains the original IPv4 egress interface");
    h_pkt[0].nx_packet_next = NX_NULL;
    _nx_tcp_sack_recovery_reset(&h_sock);
}

static void prr_delivery_conservation(UINT deny)
{
    UINT i;
    ULONG delivered = 0, failures;
    h_fixture();
    h_control_deny = deny;
    for (i = 0; i < 12; i++) { (VOID)h_send(); }
    failures = _nx_tcp_sack_scoreboard_allocation_failures;
    _nx_tcp_sack_recovery_ack(&h_sock, H_ISN);
    h_sock.nx_tcp_socket_sack_block_count = 1;
    h_sock.nx_tcp_socket_sack_left[0] = h_seq(1) + 100;
    h_sock.nx_tcp_socket_sack_right[0] = h_seq(1) + 500;
    _nx_tcp_sack_recovery_ack(&h_sock, H_ISN);
    delivered += h_sock.nx_tcp_socket_sack_delivered_data;
    h_check_eq(delivered, 400, "partial-byte SACK credits exactly its byte range");
    h_check_eq(h_control_used, !deny, "scoreboard uses one packet, or safe exhausted-pool fallback");
    _nx_tcp_sack_recovery_ack(&h_sock, H_ISN);
    h_check_eq(h_sock.nx_tcp_socket_sack_delivered_data, 0, "duplicate SACK earns no repeated delivery");
    h_sock.nx_tcp_socket_sack_left[0] = h_seq(3);
    h_sock.nx_tcp_socket_sack_right[0] = h_seq(4);
    _nx_tcp_sack_recovery_ack(&h_sock, H_ISN);
    delivered += h_sock.nx_tcp_socket_sack_delivered_data;
    h_check_eq(delivered, deny ? H_MSS : H_MSS + 400,
               "replaced SACK blocks retain exact history or conservative fallback credit");
    h_sock.nx_tcp_socket_sack_block_count = 0;
    _nx_tcp_sack_recovery_ack(&h_sock, h_seq(2));
    delivered += h_sock.nx_tcp_socket_sack_delivered_data;
    h_check(delivered <= 4 * H_MSS, "cumulative and selective delivery do not double count");
    _nx_tcp_sack_recovery_ack(&h_sock, h_seq(12));
    delivered += h_sock.nx_tcp_socket_sack_delivered_data;
    h_check_eq(delivered, 12 * H_MSS, "final cumulative ACK exactly conserves DATA delivery");
    h_check_eq(_nx_tcp_sack_scoreboard_allocation_failures - failures, deny ? 1 : 0,
               "exhausted pool is counted once per flight, without allocation retries per ACK");
    _nx_tcp_socket_block_cleanup(&h_sock);
    h_check_eq(h_control_used, NX_FALSE, "teardown reclaims scoreboard storage");
    h_check_eq(h_control_releases, deny ? 0 : 1, "teardown releases one owned control packet");
}

static void prr_bounded_scoreboard(void)
{
    UINT i;
    ULONG delivered = 0;
    h_fixture();
    for (i = 0; i < 160; i++) { (VOID)h_send(); }
    _nx_tcp_sack_recovery_ack(&h_sock, H_ISN);
    for (i = 0; i < 75; i++)
    {
        h_sock.nx_tcp_socket_sack_block_count = 1;
        h_sock.nx_tcp_socket_sack_left[0] = h_seq(2 * i + 1);
        h_sock.nx_tcp_socket_sack_right[0] = h_seq(2 * i + 2);
        _nx_tcp_sack_recovery_ack(&h_sock, H_ISN);
        delivered += h_sock.nx_tcp_socket_sack_delivered_data;
        h_check(h_sock.nx_tcp_socket_sack_hole_count <= h_sock.nx_tcp_socket_sack_hole_capacity,
                "fragmented SACK history stays inside bounded storage");
        h_check(delivered <= (i + 1) * H_MSS, "overflow cannot invent delivery credit");
    }
    h_check_eq(h_sock.nx_tcp_socket_sack_hole_capacity, 64, "scoreboard cap is independent of TX queue depth");
    h_sock.nx_tcp_socket_sack_block_count = 0;
    _nx_tcp_sack_recovery_ack(&h_sock, h_seq(160));
    delivered += h_sock.nx_tcp_socket_sack_delivered_data;
    h_check_eq(delivered, 160 * H_MSS, "capacity overflow still conserves final cumulative delivery");
    _nx_tcp_sack_recovery_reset(&h_sock);
    h_check_eq(h_control_used, NX_FALSE, "reset reclaims bounded scoreboard");
}

static void prr_repeated_ack_and_shared_credit(UINT deny)
{
    UINT i, before;
    ULONG credit, sent, threshold;
    h_fixture();
    h_control_deny = deny;
    for (i = 0; i < 20; i++) { (VOID)h_send(); }
    (VOID)h_ack(0, 1, 2, 0, 0);
    (VOID)h_ack(0, 1, 3, 0, 0);
    (VOID)h_ack(0, 1, 4, 0, 0);
    h_check(NX_TCP_SACK_RECOVERY_ACTIVE(&h_sock), "SACK enters the shared PRR controller");
    before = h_datagrams;
    credit = h_sock.nx_tcp_socket_prr_credit;
    threshold = h_sock.nx_tcp_socket_tx_slow_start_threshold;
    for (i = 0; i < 20; i++) { (VOID)h_ack(0, 1, 4, 0, 0); }
    h_check_eq(h_datagrams, before, "identical duplicate ACKs cannot clock more retransmissions");
    h_check_eq(h_sock.nx_tcp_socket_prr_credit, credit, "identical ACKs cannot mint PRR credit");
    h_check_eq(h_sock.nx_tcp_socket_tx_window_congestion, threshold, "duplicate ACKs do not inflate SACK cwnd");
    /* A sender with no eligible retransmission can use the same budget for
       new DATA even when SACKed bytes keep total outstanding above cwnd. */
    h_sock.nx_tcp_socket_prr_credit = H_MSS;
    sent = h_sock.nx_tcp_socket_prr_out;
    h_check_eq(h_send(), NX_SUCCESS, "new DATA uses PRR/rwnd instead of cwnd minus SACKed outstanding");
    h_check_eq(h_sock.nx_tcp_socket_prr_out - sent, H_MSS, "new DATA spends aggregate PRR output");
    h_check_eq(h_sock.nx_tcp_socket_prr_credit, 0, "new DATA consumes the retransmission-shared credit");
    h_check(h_send() != NX_SUCCESS, "no sender bypasses the exhausted PRR budget");
    h_sock.nx_tcp_socket_timeout = _nx_tcp_fast_timer_rate;
    _nx_tcp_fast_periodic_processing(&h_ip);
    h_check_eq(h_sock.nx_tcp_socket_prr_recover_fs, 0, "RTO terminates PRR recovery");
    h_check_eq(h_control_used, NX_FALSE, "RTO releases advisory SACK storage");
}
#endif

/* The queue owns buffers, while SND.UNA and cwnd count cumulative DATA.
   A large immutable buffer must make progress on each wire-segment ACK. */
static UINT h_ack_bytes(ULONG bytes)
{
    NX_TCP_HEADER hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.nx_tcp_sequence_number = h_sock.nx_tcp_socket_rx_sequence;
    hdr.nx_tcp_acknowledgment_number = H_ISN + bytes;
    hdr.nx_tcp_header_word_3 = NX_TCP_HEADER_SIZE | NX_TCP_ACK_BIT | 65535UL;
    return _nx_tcp_socket_state_ack_check(&h_sock, &hdr);
}

static void cumulative_bytes_before_retirement(UINT driver_owned)
{
    UCHAR data[4 * H_MSS + 128];
    NX_PACKET *p;
    ULONG window;
    UCHAR *prepend;
    h_fixture();
    (VOID)h_send();
    p = &h_pkt[0];
    memset(data, 'a', sizeof(data));
    memcpy(data + 80, p -> nx_packet_prepend_ptr, sizeof(NX_TCP_HEADER));
    p -> nx_packet_data_start = data;
    p -> nx_packet_data_end = data + sizeof(data);
    p -> nx_packet_prepend_ptr = data + 80;
    p -> nx_packet_append_ptr = p -> nx_packet_prepend_ptr + sizeof(NX_TCP_HEADER) + 4 * H_MSS;
    p -> nx_packet_length = sizeof(NX_TCP_HEADER) + 4 * H_MSS;
    if (driver_owned)
    {
        p -> nx_packet_ip_header = p -> nx_packet_prepend_ptr - 20;
        p -> nx_packet_ip_header_length = 20;
        p -> nx_packet_prepend_ptr -= 20;
        p -> nx_packet_length += 20;
        p -> nx_packet_queue_next = NX_NULL;
    }
    prepend = p -> nx_packet_prepend_ptr;
    h_sock.nx_tcp_socket_tx_sequence = H_ISN + 4 * H_MSS;
    h_sock.nx_tcp_socket_tx_outstanding_bytes = 4 * H_MSS;
    h_sock.nx_tcp_socket_tx_slow_start_threshold = 800000;
    h_sock.nx_tcp_socket_timeout_retries = 3;
    h_sock.nx_tcp_socket_stall_ticks = 20;
    window = h_sock.nx_tcp_socket_tx_window_congestion;
    h_check_eq(h_ack_bytes(333), NX_TRUE, "ACK inside a buffer is valid");
    h_check_eq(h_sock.nx_tcp_socket_tx_outstanding_bytes, 4 * H_MSS - 333,
               "partial cumulative ACK immediately releases DATA byte credit");
    h_check_eq(h_sock.nx_tcp_socket_tx_window_congestion, window + 333,
               "partial cumulative ACK earns only its DATA congestion credit");
    h_check_eq(h_sock.nx_tcp_socket_timeout_retries, 0, "partial ACK restarts retry progress");
    h_check_eq(h_sock.nx_tcp_socket_stall_ticks, 0, "partial ACK restarts stall progress");
    h_check_eq(h_sock.nx_tcp_socket_transmit_sent_count, 1, "partial ACK retains immutable buffer");
    h_check(p -> nx_packet_prepend_ptr == prepend, "partial ACK never trims driver-visible storage");
    (VOID)h_ack_bytes(333);
    (VOID)h_ack_bytes(100);
    h_check_eq(h_sock.nx_tcp_socket_tx_outstanding_bytes, 4 * H_MSS - 333,
               "duplicate and old ACKs cannot release credit again");
    h_check_eq(h_ack_bytes(4 * H_MSS + 1), NX_FALSE, "future ACK is rejected");
    h_check_eq(h_sock.nx_tcp_socket_tx_outstanding_bytes, 4 * H_MSS - 333,
               "future ACK grants no DATA credit");
    (VOID)h_ack_bytes(2 * H_MSS);
    h_check_eq(h_sock.nx_tcp_socket_tx_outstanding_bytes, 2 * H_MSS,
               "successive partial ACKs measure from the advanced SND.UNA");
    h_check_eq(h_sock.nx_tcp_socket_tx_window_congestion, window + 333 + H_MSS,
               "one delayed ACK grows slow-start cwnd by at most one MSS");
    h_sock.nx_tcp_socket_duplicated_ack_received = 0;
    (VOID)h_ack_bytes(4 * H_MSS);
    h_check_eq(h_sock.nx_tcp_socket_tx_outstanding_bytes, 0,
               "final retirement does not subtract acknowledged bytes twice");
    h_check_eq(h_sock.nx_tcp_socket_transmit_sent_count, 0, "final ACK retires the buffer");
    h_check(p -> nx_packet_union_next.nx_packet_tcp_queue_next == (NX_PACKET *)NX_PACKET_ALLOCATED,
            "driver can release its buffer after full ACK without a queue owner");
#ifdef NX_ENABLE_TCP_SACK
    _nx_tcp_sack_recovery_reset(&h_sock);
#endif
}

int main(void)
{
    _nx_tcp_fast_timer_rate     = (NX_IP_PERIODIC_RATE + (NX_TCP_FAST_TIMER_RATE - 1)) / NX_TCP_FAST_TIMER_RATE;
    _nx_tcp_ack_timer_rate      = (NX_IP_PERIODIC_RATE + (NX_TCP_ACK_TIMER_RATE - 1)) / NX_TCP_ACK_TIMER_RATE;
    _nx_tcp_transmit_timer_rate = (NX_IP_PERIODIC_RATE + (NX_TCP_TRANSMIT_TIMER_RATE - 1)) / NX_TCP_TRANSMIT_TIMER_RATE;

    printf("RFC 6675 loss recovery, against a socket rather than a network\n");

    cumulative_bytes_before_retirement(NX_FALSE);
    cumulative_bytes_before_retirement(NX_TRUE);
    single_loss();
    second_loss_at_the_tail();
    burst_hole();
    two_holes_partial_ack(3);
    two_holes_partial_ack(14);
    timeout_hole();
    fin_ack_sequence();
#ifdef NX_ENABLE_TCP_SACK
    fin_sack_option_sequence();
    fin_partial_ack_sequence();
    timeout_fresh_sack_tail();
    prr_burst_example();
    prr_receive_window_bound();
    newreno_peer_unchanged();
    prr_credit_across_allocation();
    prr_partial_credit_slice();
    prr_delivery_conservation(NX_FALSE);
    prr_delivery_conservation(NX_TRUE);
    prr_bounded_scoreboard();
    prr_repeated_ack_and_shared_credit(NX_FALSE);
    prr_repeated_ack_and_shared_credit(NX_TRUE);
    _nx_tcp_sack_recovery_reset(&h_sock);
#endif /* NX_ENABLE_TCP_SACK */

    printf("%lu checks, %lu failures, %s\n",
           h_checks, h_failures, (h_failures == 0UL) ? "PASS" : "FAIL");

    return (h_failures == 0UL) ? 0 : 1;
}
