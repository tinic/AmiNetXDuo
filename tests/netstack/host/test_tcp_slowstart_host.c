/*
 * AmiNetXDuo, the slow-start threshold a connection starts with, RFC 5681
 * 3.1: arbitrarily high, so that slow start runs until the first loss.
 *
 * The A1200 on 2026-09-18 (AmiSpeedTest upload, 23 ms to the Ookla server,
 * scratchpad astup.pcap): the peer's SYN-ACK carried a window of 42,340 --
 * unscaled, as a SYN's window always is -- and the threshold was set to it.
 * Slow start ended after two round trips at 43 KB in flight, and congestion
 * avoidance grew the flight one segment a round trip from there: 415 KB
 * after five seconds, 86 Mbit/s, against a peer window of 660 KB, no loss,
 * on a 300 Mbit/s link.
 *
 * Drives the real handshake handlers (client and accepted side), then the
 * real send and acknowledgment-check code round trip by round trip.
 *
 * SPDX-License-Identifier: MIT
 */
#include "nx_api.h"
#include "nx_tcp.h"
#include "nx_packet.h"
#include "nx_ip.h"

#include <stdio.h>
#include <string.h>

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
static UINT h_acks_sent;
static UINT h_syns_sent;
static UINT h_rsts_sent;

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
#define H_PACKETS       512
#define H_ISN           0x10000000UL

/* The capture: the peer's SYN-ACK window, its scale, and the window its
   acknowledgments grew to once the transfer ran. */
#define H_SYN_WINDOW    42340UL
#define H_PEER_SHIFT    8
#define H_PEER_WINDOW   (2087UL << H_PEER_SHIFT)        /* 534,272 */

static NX_IP          h_ip;
static NX_INTERFACE   h_iface;
static NX_TCP_SOCKET  h_sock;
static NX_PACKET      h_pkt[H_PACKETS];
static UCHAR          h_pkt_buf[H_PACKETS][H_BUF];
static UINT           h_pkt_next;

VOID _nx_ip_packet_send(NX_IP *ip_ptr, NX_PACKET *packet_ptr,
                        ULONG destination_ip, ULONG type_of_service,
                        ULONG time_to_live, ULONG protocol, ULONG fragment,
                        ULONG next_hop_address)
{
    (void)ip_ptr; (void)destination_ip; (void)type_of_service;
    (void)time_to_live; (void)protocol; (void)fragment; (void)next_hop_address;

    h_datagrams++;

    /* What a SANA-II transmit completion leaves behind. */
    packet_ptr -> nx_packet_queue_next = (NX_PACKET *)NX_DRIVER_TX_DONE;
}

VOID _nx_tcp_packet_send_ack(NX_TCP_SOCKET *socket_ptr, ULONG tx_sequence)
{
    (void)socket_ptr;
    (void)tx_sequence;
    h_acks_sent++;
}

VOID _nx_tcp_packet_send_syn(NX_TCP_SOCKET *socket_ptr, ULONG tx_sequence)
{
    (void)socket_ptr;
    (void)tx_sequence;
    h_syns_sent++;
}

VOID _nx_tcp_packet_send_rst(NX_TCP_SOCKET *socket_ptr, NX_TCP_HEADER *header_ptr)
{
    (void)socket_ptr;
    (void)header_ptr;
    h_rsts_sent++;
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

static void h_fixture(void)
{
    memset(&h_ip, 0, sizeof(h_ip));
    memset(&h_iface, 0, sizeof(h_iface));

    h_now        = 1000;
    h_datagrams  = 0;
    h_acks_sent  = 0;
    h_syns_sent  = 0;
    h_rsts_sent  = 0;
    h_pkt_next   = 0;

    h_iface.nx_interface_ip_address = 0xC0A80102UL;

    _nx_tcp_socket_create(&h_ip, &h_sock, "host slow start", NX_IP_NORMAL,
                          NX_FRAGMENT_OKAY, 0x80, 8192, NX_NULL, NX_NULL);

    h_sock.nx_tcp_socket_bound_next   = &h_sock;
    h_sock.nx_tcp_socket_port         = 40000;
    h_sock.nx_tcp_socket_connect_port = 8080;
    h_sock.nx_tcp_socket_connect_ip.nxd_ip_version    = NX_IP_VERSION_V4;
    h_sock.nx_tcp_socket_connect_ip.nxd_ip_address.v4 = 0xC0A80101UL;
    h_sock.nx_tcp_socket_connect_interface = &h_iface;
    h_sock.nx_tcp_socket_connect_mss  = H_MSS;
    h_sock.nx_tcp_socket_connect_mss2 = H_MSS * H_MSS;
    h_sock.nx_tcp_socket_transmit_queue_maximum = H_PACKETS;
    h_sock.nx_tcp_socket_rx_sequence  = 0x20000000UL;

    /* What _nx_tcp_socket_packet_process leaves from the peer's options
       before it hands the SYN to the state handler. */
#ifdef NX_ENABLE_TCP_WINDOW_SCALING
    h_sock.nx_tcp_snd_win_scale_value = H_PEER_SHIFT;
    h_sock.nx_tcp_rcv_win_scale_value = 4;
#endif
#ifdef NX_ENABLE_TCP_SACK
    h_sock.nx_tcp_socket_sack_permitted = NX_TRUE;
#endif
}

/* The peer's SYN-ACK to our SYN: the client-side handshake. */
static void h_connect_client(void)
{
    NX_TCP_HEADER hdr;
    NX_PACKET     pkt;

    h_sock.nx_tcp_socket_client_type = NX_TRUE;
    h_sock.nx_tcp_socket_state       = NX_TCP_SYN_SENT;
    h_sock.nx_tcp_socket_tx_sequence = H_ISN;           /* the SYN took ISN - 1 */

    memset(&hdr, 0, sizeof(hdr));
    memset(&pkt, 0, sizeof(pkt));
    hdr.nx_tcp_header_word_0         = (8080UL << NX_SHIFT_BY_16) | 40000UL;
    hdr.nx_tcp_sequence_number       = 0x20000000UL - 1;
    hdr.nx_tcp_acknowledgment_number = H_ISN;
    hdr.nx_tcp_header_word_3         = NX_TCP_HEADER_SIZE | NX_TCP_SYN_BIT |
                                       NX_TCP_ACK_BIT | H_SYN_WINDOW;

    _nx_tcp_socket_state_syn_sent(&h_sock, &hdr, &pkt);
}

/* The peer's ACK to our SYN-ACK: the accepted side of the handshake.  That
   third segment is not a SYN, so its window is scaled. */
static void h_connect_server(void)
{
    NX_TCP_HEADER hdr;

    h_sock.nx_tcp_socket_client_type = NX_FALSE;
    h_sock.nx_tcp_socket_state       = NX_TCP_SYN_RECEIVED;
    h_sock.nx_tcp_socket_tx_sequence = H_ISN;

    memset(&hdr, 0, sizeof(hdr));
    hdr.nx_tcp_header_word_0         = (8080UL << NX_SHIFT_BY_16) | 40000UL;
    hdr.nx_tcp_sequence_number       = 0x20000000UL;
    hdr.nx_tcp_acknowledgment_number = H_ISN;
    hdr.nx_tcp_header_word_3         = NX_TCP_HEADER_SIZE | NX_TCP_ACK_BIT |
                                       (H_SYN_WINDOW >> H_PEER_SHIFT);

    _nx_tcp_socket_state_syn_received(&h_sock, &hdr);
}

static UINT h_send(void)
{
    NX_PACKET *p;

    if (h_pkt_next >= H_PACKETS)
    {
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

/* One acknowledgment from the peer, cumulative to `seq`, carrying the
   window the transfer ran against (scaled on the wire). */
static UINT h_ack(ULONG seq)
{
    NX_TCP_HEADER hdr;

    memset(&hdr, 0, sizeof(hdr));
    hdr.nx_tcp_header_word_0         = (8080UL << NX_SHIFT_BY_16) | 40000UL;
    hdr.nx_tcp_sequence_number       = h_sock.nx_tcp_socket_rx_sequence;
    hdr.nx_tcp_acknowledgment_number = seq;
    hdr.nx_tcp_header_word_3         = NX_TCP_HEADER_SIZE | NX_TCP_ACK_BIT |
                                       (H_PEER_WINDOW >> H_PEER_SHIFT);

    return _nx_tcp_socket_state_ack_check(&h_sock, &hdr);
}

/*
 * One round trip of a transfer that never loses anything: fill the window
 * the send path allows, then the peer acknowledges it, one acknowledgment
 * per two segments (RFC 1122 4.2.3.2, what the capture's peer did within
 * rounding).  Returns the number of segments the round put in flight.
 */
static UINT h_round_trip(void)
{
    UINT  sent = 0;
    ULONG first = h_sock.nx_tcp_socket_tx_sequence;
    ULONG seq;

    while (h_send() == NX_SUCCESS)
    {
        sent++;
    }

    /* Every packet the round sent is in the transmit queue: the sends that
       failed took nothing. */
    h_pkt_next -= (h_pkt_next > sent) ? 1 : 0;   /* the one that did not go */

    for (seq = first + 2 * H_MSS; ((INT)(h_sock.nx_tcp_socket_tx_sequence - seq)) >= 0; seq += 2 * H_MSS)
    {
        (VOID)h_ack(seq);
    }
    if (h_sock.nx_tcp_socket_tx_sequence != seq - 2 * H_MSS)
    {
        (VOID)h_ack(h_sock.nx_tcp_socket_tx_sequence);
    }

    /* The queue is empty again: the packets can be reused. */
    h_pkt_next = 0;

    return sent;
}

static void threshold_after_handshake(void)
{
    printf("the threshold a fresh connection starts with\n");

    h_fixture();
    h_connect_client();
    h_check_eq(h_sock.nx_tcp_socket_state, NX_TCP_ESTABLISHED, "client: established");
    h_check_eq(h_acks_sent, 1, "client: the handshake's ACK went out");
    h_check_eq(h_sock.nx_tcp_socket_tx_window_advertised, H_SYN_WINDOW,
               "client: the SYN-ACK's window is taken unscaled");
    h_check_eq(h_sock.nx_tcp_socket_tx_slow_start_threshold, NX_TCP_INITIAL_SSTHRESH,
               "client: the threshold is arbitrarily high, not that window");
    h_check(h_sock.nx_tcp_socket_tx_slow_start_threshold >= (65535UL << 14),
            "client: at least the largest window a peer can ever advertise");
    h_check_eq(h_sock.nx_tcp_socket_tx_window_congestion, 3 * H_MSS,
               "client: the initial window is three segments (RFC 5681 3.1)");

    h_fixture();
    h_connect_server();
    h_check_eq(h_sock.nx_tcp_socket_state, NX_TCP_ESTABLISHED, "accepted: established");
    h_check_eq(h_sock.nx_tcp_socket_tx_slow_start_threshold, NX_TCP_INITIAL_SSTHRESH,
               "accepted: the threshold is arbitrarily high");
    h_check_eq(h_rsts_sent, 0, "accepted: no reset");
}

/*
 * The upload: the peer's window opens to 534 KB and nothing is lost.  With
 * the threshold at the SYN's 42,340 the flight left slow start at 43 KB and
 * grew one segment a round trip -- 336 round trips to the peer's window.
 * Arbitrarily high, slow start goes on until the peer's window is the limit.
 */
static void slow_start_reaches_the_window(void)
{
    UINT  round, sent = 0, reached = 0;
    ULONG window_segments = H_PEER_WINDOW / H_MSS;

    printf("a transfer with no loss, the peer's window at %lu\n",
           (unsigned long)H_PEER_WINDOW);

    h_fixture();
    h_connect_client();

    for (round = 1; round <= 40; round++)
    {
        sent = h_round_trip();
        if (round <= 3 || (round % 4) == 0)
        {
            printf("  round trip %2u        %3u segments in flight, cwnd %lu\n",
                   round, sent, (unsigned long)h_sock.nx_tcp_socket_tx_window_congestion);
        }
        if (sent >= window_segments && reached == 0)
        {
            reached = round;
        }
    }

    printf("  the peer's window was reached after %u round trip(s)\n", reached);
    h_check(reached != 0, "the flight reaches the peer's window at all");
    h_check(reached != 0 && reached <= 14,
            "and within fourteen round trips, not three hundred");
    h_check_eq(sent, window_segments, "then the peer's window is the limit");
    h_check(h_sock.nx_tcp_socket_tx_window_congestion < NX_TCP_INITIAL_SSTHRESH,
            "and the congestion window has not crossed the threshold");
    h_check(h_sock.nx_tcp_socket_tx_window_congestion >= H_PEER_WINDOW,
            "the congestion window is not what holds the flight");
}

/*
 * A loss sets a real threshold and slow start ends there: the flight
 * after recovery grows one segment a round trip again, as before.
 */
static void loss_sets_a_real_threshold(void)
{
    ULONG before, after;

    printf("a loss, then congestion avoidance\n");

    h_fixture();
    h_connect_client();
    (VOID)h_round_trip();
    (VOID)h_round_trip();
    (VOID)h_round_trip();

    /* A timeout on the next flight. */
    (VOID)h_send();
    (VOID)h_send();
    (VOID)h_send();
    (VOID)h_send();
    h_sock.nx_tcp_socket_tx_sequence_recover = h_sock.nx_tcp_socket_tx_sequence - 1;
    _nx_tcp_socket_retransmit(&h_ip, &h_sock, NX_FALSE);
    h_check(h_sock.nx_tcp_socket_tx_slow_start_threshold < NX_TCP_INITIAL_SSTHRESH,
            "the timeout set a threshold from the flight");
    h_check_eq(h_sock.nx_tcp_socket_tx_slow_start_threshold, 2 * H_MSS,
               "half the flight of four segments");
    h_check_eq(h_sock.nx_tcp_socket_tx_window_congestion, H_MSS,
               "and the window is one segment");

    /* The flight is acknowledged; from here the window is past the
       threshold and grows by the byte count. */
    (VOID)h_ack(h_sock.nx_tcp_socket_tx_sequence);
    h_pkt_next = 0;
    before = h_sock.nx_tcp_socket_tx_window_congestion;
    (VOID)h_round_trip();
    (VOID)h_round_trip();
    after = h_sock.nx_tcp_socket_tx_window_congestion;
    h_check(after > before && after - before <= 3 * H_MSS,
            "congestion avoidance: about one segment a round trip");
}

int main(void)
{
    _nx_tcp_fast_timer_rate     = (NX_IP_PERIODIC_RATE + (NX_TCP_FAST_TIMER_RATE - 1)) / NX_TCP_FAST_TIMER_RATE;
    _nx_tcp_ack_timer_rate      = (NX_IP_PERIODIC_RATE + (NX_TCP_ACK_TIMER_RATE - 1)) / NX_TCP_ACK_TIMER_RATE;
    _nx_tcp_transmit_timer_rate = (NX_IP_PERIODIC_RATE + (NX_TCP_TRANSMIT_TIMER_RATE - 1)) / NX_TCP_TRANSMIT_TIMER_RATE;

    printf("RFC 5681 3.1 initial slow-start threshold, against a socket rather than a network\n");

    threshold_after_handshake();
    slow_start_reaches_the_window();
    loss_sets_a_real_threshold();

    printf("%lu checks, %lu failures, %s\n",
           h_checks, h_failures, (h_failures == 0UL) ? "PASS" : "FAIL");

    return (h_failures == 0UL) ? 0 : 1;
}
