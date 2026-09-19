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

UINT _nx_packet_release(NX_PACKET *packet_ptr)
{
    (void)packet_ptr;
    return NX_SUCCESS;
}

UINT _nx_packet_allocate(NX_PACKET_POOL *pool_ptr, NX_PACKET **packet_ptr,
                         ULONG packet_type, ULONG wait_option)
{
    (void)pool_ptr; (void)packet_ptr; (void)packet_type; (void)wait_option;
    return NX_NO_PACKET;
}

UINT _nx_packet_data_append(NX_PACKET *packet_ptr, VOID *data_start, ULONG data_size,
                            NX_PACKET_POOL *pool_ptr, ULONG wait_option)
{
    (void)packet_ptr; (void)data_start; (void)data_size;
    (void)pool_ptr; (void)wait_option;
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
        h_log[h_logged++] = seq;
    }

    /* What a SANA-II transmit completion leaves behind, and what the
       retransmit path needs to see before it will send a queued segment
       again.  */
    packet_ptr -> nx_packet_queue_next = (NX_PACKET *)NX_DRIVER_TX_DONE;
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
    memset(&h_ip, 0, sizeof(h_ip));
    memset(&h_iface, 0, sizeof(h_iface));

    h_now       = 1000;
    h_datagrams = 0;
    h_pkt_next  = 0;
    h_logged    = 0;

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
static UINT h_ack(UINT acked_segments, UINT b1_from, UINT b1_to, UINT b2_from, UINT b2_to)
{
    NX_TCP_HEADER hdr;

    memset(&hdr, 0, sizeof(hdr));
    hdr.nx_tcp_header_word_0         = (5001UL << NX_SHIFT_BY_16) | 40000UL;
    hdr.nx_tcp_sequence_number       = h_sock.nx_tcp_socket_rx_sequence;
    hdr.nx_tcp_acknowledgment_number = h_seq(acked_segments);
    hdr.nx_tcp_header_word_3         = NX_TCP_HEADER_SIZE | NX_TCP_ACK_BIT | 65535UL;

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

    return _nx_tcp_socket_state_ack_check(&h_sock, &hdr);
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

    /* The rest of the delivered flight is reported, one block growing. */
    for (i = H_HOLE_TO + 4; i <= H_FLIGHT; i++)
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

int main(void)
{
    _nx_tcp_fast_timer_rate     = (NX_IP_PERIODIC_RATE + (NX_TCP_FAST_TIMER_RATE - 1)) / NX_TCP_FAST_TIMER_RATE;
    _nx_tcp_ack_timer_rate      = (NX_IP_PERIODIC_RATE + (NX_TCP_ACK_TIMER_RATE - 1)) / NX_TCP_ACK_TIMER_RATE;
    _nx_tcp_transmit_timer_rate = (NX_IP_PERIODIC_RATE + (NX_TCP_TRANSMIT_TIMER_RATE - 1)) / NX_TCP_TRANSMIT_TIMER_RATE;

    printf("RFC 6675 loss recovery, against a socket rather than a network\n");

    single_loss();
    burst_hole();

    printf("%lu checks, %lu failures, %s\n",
           h_checks, h_failures, (h_failures == 0UL) ? "PASS" : "FAIL");

    return (h_failures == 0UL) ? 0 : 1;
}
