/*
 * AmiNetXDuo, the two receiver-side acknowledgment holes that used to leave a
 * sender in persist backoff.  Both are properties of the vendored TCP state
 * machine, so they hold on every card and every machine, and both are asserted
 * here against the real receive path and the real fast periodic timer rather
 * than against a throughput figure.
 *
 * WHAT THIS GUARDS, and why it is a signature and not a rate.
 *
 *   1. A SEGMENT TRIMMED TO NOTHING IS A WINDOW PROBE AND GETS AN ANSWER.
 *      A sender parked on a closed window puts one byte past the advertised
 *      edge (RFC 1122 4.2.2.17).  The window trims cut that byte away, the
 *      segment reaches the no-payload block carrying nothing, and the block's
 *      invalid-sequence conditions -- written for bare acknowledgments --
 *      exclude exactly a probe's sequence numbers.  Nothing else in the
 *      function answers it: it advances no sequence and reopens no window,
 *      which are the two things that let an acknowledgment out.  An unanswered
 *      probe is a persist timer doubling its interval, and doubling it again.
 *
 *   2. A RUNT WINDOW ESCAPES FROM THE DELAYED-ACK ARM.
 *      A last advertisement below one MSS parks the sender, and nothing
 *      regrows the announcement until the application has drained RCV.BUFF/2
 *      -- half a buffer away.  The escape announces from the fast periodic's
 *      delayed-ACK arm once two full segments fit, and from THERE ONLY: the
 *      receive-path form is a stable degraded attractor and was measured and
 *      rejected, so the negative case below is as load-bearing as the
 *      positive one.
 *
 * Both landed in 0.25.3 as submodule commits (tinic/netxduo bfedf066 and
 * 0a00c482) with no test in this tree, which is what this file is for.
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

/* ------------------------------------------------------------ the shim --- */

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

UINT _tx_thread_preemption_change(TX_THREAD *thread_ptr, UINT new_threshold,
                                  UINT *old_threshold)
{
    (void)thread_ptr; (void)new_threshold;
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

VOID _nx_tcp_socket_thread_resume(TX_THREAD **suspension_list_head, UINT status)
{
    (void)suspension_list_head; (void)status;
}

UINT _nx_packet_release(NX_PACKET *packet_ptr)
{
    (void)packet_ptr;
    return NX_SUCCESS;
}

/* The retransmission ladder is not what this file is about; every socket it
   drives has nx_tcp_socket_timeout at zero, so none of these is reached. */
VOID _nx_tcp_socket_retransmit_tail(NX_IP *ip_ptr, NX_TCP_SOCKET *socket_ptr)
{
    (void)ip_ptr; (void)socket_ptr;
}

VOID _nx_tcp_socket_retransmit(NX_IP *ip_ptr, NX_TCP_SOCKET *socket_ptr,
                               UINT need_fast_retransmit)
{
    (void)ip_ptr; (void)socket_ptr; (void)need_fast_retransmit;
}

VOID _nx_tcp_socket_connection_reset(NX_TCP_SOCKET *socket_ptr)
{
    (void)socket_ptr;
}

VOID _nx_tcp_packet_send_syn(NX_TCP_SOCKET *socket_ptr, ULONG tx_sequence)
{
    (void)socket_ptr; (void)tx_sequence;
}

VOID _nx_tcp_packet_send_fin(NX_TCP_SOCKET *socket_ptr, ULONG tx_sequence)
{
    (void)socket_ptr; (void)tx_sequence;
}

VOID _nx_tcp_socket_block_cleanup(NX_TCP_SOCKET *socket_ptr)
{
    (void)socket_ptr;
}

#if defined(NX_ENABLE_TCP_LOSS_PROBE) && defined(NX_ENABLE_TCP_RTT_ESTIMATOR)
VOID _nx_tcp_socket_loss_probe_check(NX_IP *ip_ptr, NX_TCP_SOCKET *socket_ptr)
{
    (void)ip_ptr; (void)socket_ptr;
}
#endif

/* ------------------------------------------------------- what went out --- */

static NX_PACKET h_ack_pkt;
static UCHAR     h_ack_buf[256];

/* Every acknowledgment the real control path put on the wire, and the header
   fields of the last one.  Counted at _nx_ip_packet_send, not at
   _nx_tcp_packet_send_ack: the question is what a peer would have seen. */
static UINT  h_acks;
static ULONG h_last_window;
static ULONG h_last_ack_seq;

UINT _nx_packet_allocate(NX_PACKET_POOL *pool_ptr, NX_PACKET **packet_ptr,
                         ULONG packet_type, ULONG wait_option)
{
    (void)pool_ptr; (void)packet_type; (void)wait_option;

    memset(&h_ack_pkt, 0, sizeof(h_ack_pkt));
    memset(h_ack_buf, 0, sizeof(h_ack_buf));

    h_ack_pkt.nx_packet_data_start  = h_ack_buf;
    h_ack_pkt.nx_packet_data_end    = h_ack_buf + sizeof(h_ack_buf);
    h_ack_pkt.nx_packet_prepend_ptr = h_ack_buf + 64;
    h_ack_pkt.nx_packet_append_ptr  = h_ack_buf + 64;

    *packet_ptr = &h_ack_pkt;
    return NX_SUCCESS;
}

UINT _nx_packet_data_append(NX_PACKET *packet_ptr, VOID *data_start,
                            ULONG data_size, NX_PACKET_POOL *pool_ptr,
                            ULONG wait_option)
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

VOID _nx_ip_packet_send(NX_IP *ip_ptr, NX_PACKET *packet_ptr,
                        ULONG destination_ip, ULONG type_of_service,
                        ULONG time_to_live, ULONG protocol, ULONG fragment,
                        ULONG next_hop_address)
{
NX_TCP_HEADER *hdr;
ULONG          word_3;
ULONG          ack_seq;

    (void)ip_ptr; (void)destination_ip; (void)type_of_service;
    (void)time_to_live; (void)protocol; (void)fragment; (void)next_hop_address;

    hdr     = (NX_TCP_HEADER *)packet_ptr -> nx_packet_prepend_ptr;
    word_3  = hdr -> nx_tcp_header_word_3;
    ack_seq = hdr -> nx_tcp_acknowledgment_number;

    NX_CHANGE_ULONG_ENDIAN(word_3);
    NX_CHANGE_ULONG_ENDIAN(ack_seq);

    h_last_window  = word_3 & 0xFFFFUL;
    h_last_ack_seq = ack_seq;
    h_acks++;

    packet_ptr -> nx_packet_queue_next = (NX_PACKET *)NX_DRIVER_TX_DONE;
}

/* --------------------------------------------------------- the fixture --- */

#define H_MSS       1460UL
#define H_WINDOW    8192UL          /* RCV.BUFF/2 is 4096, above 2*MSS */
#define H_ISN       0x10000000UL
#define H_ISN_RX    0x20000000UL
#define H_BUF       256

static NX_IP          h_ip;
static NX_INTERFACE   h_iface;
static NX_TCP_SOCKET  h_sock;
static NX_PACKET_POOL h_pool;

static NX_PACKET      h_pkt;
static UCHAR          h_pkt_buf[H_BUF];

static void h_fixture(void)
{
    memset(&h_ip, 0, sizeof(h_ip));
    memset(&h_iface, 0, sizeof(h_iface));
    memset(&h_pool, 0, sizeof(h_pool));

    h_now          = 1000;
    h_acks         = 0;
    h_last_window  = 0xFFFFFFFFUL;
    h_last_ack_seq = 0;

    h_pool.nx_packet_pool_total     = 64;
    h_pool.nx_packet_pool_available = 64;

    h_iface.nx_interface_ip_address = 0xC0A80102UL;

    _nx_tcp_socket_create(&h_ip, &h_sock, "host persist", NX_IP_NORMAL,
                          NX_FRAGMENT_OKAY, 0x80, H_WINDOW, NX_NULL, NX_NULL);

    h_sock.nx_tcp_socket_bound_next   = &h_sock;
    h_sock.nx_tcp_socket_client_type  = NX_TRUE;
    h_sock.nx_tcp_socket_state        = NX_TCP_ESTABLISHED;
    h_sock.nx_tcp_socket_port         = 40000;
    h_sock.nx_tcp_socket_connect_port = 80;
    h_sock.nx_tcp_socket_connect_ip.nxd_ip_version    = NX_IP_VERSION_V4;
    h_sock.nx_tcp_socket_connect_ip.nxd_ip_address.v4 = 0xC0A80101UL;
    h_sock.nx_tcp_socket_connect_interface = &h_iface;
    h_sock.nx_tcp_socket_connect_mss  = H_MSS;
    h_sock.nx_tcp_socket_connect_mss2 = H_MSS * H_MSS;

    h_sock.nx_tcp_socket_tx_sequence         = H_ISN;
    h_sock.nx_tcp_socket_rx_sequence         = H_ISN_RX;
    h_sock.nx_tcp_socket_rx_sequence_acked   = H_ISN_RX;
    h_sock.nx_tcp_socket_rx_window_default   = H_WINDOW;
    h_sock.nx_tcp_socket_rx_window_current   = H_WINDOW;
    h_sock.nx_tcp_socket_rx_window_last_sent = H_WINDOW;

    h_sock.nx_tcp_socket_receive_queue_head  = NX_NULL;
    h_sock.nx_tcp_socket_receive_queue_tail  = NX_NULL;
    h_sock.nx_tcp_socket_receive_queue_count = 0;
    h_sock.nx_tcp_socket_receive_suspension_list = NX_NULL;
    h_sock.nx_tcp_receive_callback           = NX_NULL;
    h_sock.nx_tcp_socket_timeout             = 0;

    h_ip.nx_ip_default_packet_pool = &h_pool;
}

/* One segment of `bytes` at `seq`, through the real receive path. */
static void h_feed(ULONG seq, ULONG bytes)
{
NX_TCP_HEADER *hdr;

    memset(&h_pkt, 0, sizeof(h_pkt));
    memset(h_pkt_buf, 'x', sizeof(h_pkt_buf));

    h_pkt.nx_packet_data_start  = h_pkt_buf;
    h_pkt.nx_packet_data_end    = h_pkt_buf + sizeof(h_pkt_buf);
    h_pkt.nx_packet_prepend_ptr = h_pkt_buf;
    h_pkt.nx_packet_append_ptr  = h_pkt_buf + sizeof(NX_TCP_HEADER) + bytes;
    h_pkt.nx_packet_length      = sizeof(NX_TCP_HEADER) + bytes;
    h_pkt.nx_packet_pool_owner  = &h_pool;

    hdr = (NX_TCP_HEADER *)h_pkt.nx_packet_prepend_ptr;
    memset(hdr, 0, sizeof(*hdr));
    hdr -> nx_tcp_header_word_0   = (80UL << NX_SHIFT_BY_16) | 40000UL;
    hdr -> nx_tcp_sequence_number = seq;
    hdr -> nx_tcp_header_word_3   = NX_TCP_HEADER_SIZE | NX_TCP_ACK_BIT |
                                    65535UL;

    (VOID)_nx_tcp_socket_state_data_check(&h_sock, &h_pkt);
}

/* One turn of the fast periodic with the delayed acknowledgment already due. */
static void h_tick(void)
{
    h_sock.nx_tcp_socket_created_next     = &h_sock;
    h_sock.nx_tcp_socket_delayed_ack_timeout = 1;

    h_ip.nx_ip_tcp_created_sockets_count = 1;
    h_ip.nx_ip_tcp_created_sockets_ptr   = &h_sock;

    _nx_tcp_fast_periodic_processing(&h_ip);
}

/* ------------------------------- 1. the window probe gets an answer ------ */

/*
 * The zero-window probe.  RCV.WND is 0, the peer's persist timer sends one
 * byte at RCV.NXT, the right-hand trim takes it, and the segment must still be
 * answered.  Before tinic/netxduo bfedf066 nothing answered it, because the
 * invalid-sequence test the byte fell through to asks for a sequence that is
 * NOT the one a probe carries.
 */
static void a_zero_window_probe_is_answered(void)
{
    h_fixture();

    h_sock.nx_tcp_socket_rx_window_current   = 0;
    h_sock.nx_tcp_socket_rx_window_last_sent = 0;

    h_feed(H_ISN_RX, 1);

    h_check_eq(h_acks, 1,
               "a one-byte zero-window probe went unanswered, which is a "
               "sender left in persist backoff");
    h_check_eq(h_last_ack_seq, H_ISN_RX,
               "the probe answer did not carry the receive sequence");
    h_check_eq(h_last_window, 0,
               "the probe answer did not carry the current (closed) window");

    printf("  zero-window probe   %u ack(s), window %lu\n",
           (unsigned int)h_acks, (unsigned long)h_last_window);
}

/*
 * The same shape on a runt window rather than a closed one: the probe byte
 * lies one past the advertised edge, so the trim still takes all of it.
 *
 * This one WAS answered before the fix, by accident: the probe sits at a
 * sequence the old invalid-sequence test happened to call invalid.  It is here
 * so that the boundary is written down -- the defect was never "probes are
 * ignored", it was "probes at RCV.NXT are ignored", which is the only place a
 * sender parked on a CLOSED window can put one.
 */
static void b_runt_window_probe_is_answered(void)
{
    h_fixture();

    h_sock.nx_tcp_socket_rx_window_current   = 100;
    h_sock.nx_tcp_socket_rx_window_last_sent = 100;

    h_feed(H_ISN_RX + 100UL, 1);

    h_check_eq(h_acks, 1,
               "a probe one byte past a runt window went unanswered");

    printf("  runt-window probe   %u ack(s)\n", (unsigned int)h_acks);
}

/*
 * RFC 793's other shape that lands in the same block: a segment every byte of
 * which has already been received.  The left-hand trim takes all of it AND
 * fixes the header sequence up to RCV.NXT, so the old invalid-sequence test
 * could not fire on this either.
 */
static void c_fully_duplicate_segment_is_answered(void)
{
    h_fixture();

    h_feed(H_ISN_RX - 100UL, 100);

    h_check_eq(h_acks, 1,
               "a fully duplicate segment went unanswered");

    printf("  duplicate segment   %u ack(s)\n", (unsigned int)h_acks);
}

/*
 * AND THE COUNTERWEIGHT.  A bare acknowledgment -- no payload when it arrived,
 * at the expected sequence -- must NOT be answered.  Answering it is an
 * acknowledgment of an acknowledgment, which is a loop the two ends run until
 * one of them is loaded.  This is the check that keeps the fix from being
 * "answer everything".
 */
static void d_a_bare_ack_is_not_answered(void)
{
    h_fixture();

    h_feed(H_ISN_RX, 0);

    h_check_eq(h_acks, 0,
               "a bare acknowledgment at the expected sequence was answered "
               "with another acknowledgment");

    printf("  bare ack            %u ack(s)\n", (unsigned int)h_acks);
}

/* ------------------------------- 2. the runt-window escape --------------- */

/*
 * The application has drained: RCV.WND is two full segments again, but the
 * last thing announced was below one MSS, so the sender is parked.  Half the
 * buffer (4096) is more than two segments (2920), so the ordinary
 * window-update step CANNOT fire here and the escape is the only thing that
 * can put an announcement on the wire.
 */
static void e_two_segments_escape_a_runt_advertisement(void)
{
    h_fixture();

    h_sock.nx_tcp_socket_rx_window_last_sent = 0;
    h_sock.nx_tcp_socket_rx_window_current   = H_MSS * 2UL;

    h_tick();

    h_check_eq(h_acks, 1,
               "a window that had reopened to two full segments was never "
               "announced, so the sender stays in persist backoff until the "
               "application drains half the buffer");
    h_check_eq(h_last_window, H_MSS * 2UL,
               "the escape announced a window other than what had reopened");

    printf("  runt escape         %u ack(s), window %lu (step would need "
           "%lu)\n", (unsigned int)h_acks, (unsigned long)h_last_window,
           (unsigned long)(H_WINDOW / 2UL));
}

/*
 * ONE segment is not the escape.  Announcing at the moment one segment fits is
 * the silly-window advertisement the floor exists to suppress.
 */
static void f_one_segment_does_not_escape(void)
{
    h_fixture();

    h_sock.nx_tcp_socket_rx_window_last_sent = 0;
    h_sock.nx_tcp_socket_rx_window_current   = H_MSS;

    h_tick();

    h_check_eq(h_acks, 0,
               "one segment of reopened window was announced, which is the "
               "silly-window advertisement the two-segment floor suppresses");

    printf("  one segment         %u ack(s)\n", (unsigned int)h_acks);
}

/*
 * And the escape is for a RUNT advertisement only.  A last advertisement of a
 * full segment or more is not a parked sender, so a sub-step increment on top
 * of it stays unannounced -- otherwise the escape becomes an announcement at
 * every dequeue, which was measured as a stable degraded attractor.
 */
static void g_a_full_last_advertisement_does_not_escape(void)
{
    h_fixture();

    h_sock.nx_tcp_socket_rx_window_last_sent = H_MSS;
    h_sock.nx_tcp_socket_rx_window_current   = H_MSS * 2UL;

    h_tick();

    h_check_eq(h_acks, 0,
               "a window that had never gone runt was announced below the "
               "half-buffer step, which is an announcement at every dequeue");

    printf("  full last advert    %u ack(s)\n", (unsigned int)h_acks);
}

/*
 * The half-buffer step still works, which is what carries an ordinary
 * transfer: nothing about the escape may replace it.
 */
static void h_the_half_buffer_step_still_fires(void)
{
    h_fixture();

    h_sock.nx_tcp_socket_rx_window_last_sent = H_MSS * 2UL;
    h_sock.nx_tcp_socket_rx_window_current   = H_WINDOW;

    h_tick();

    h_check_eq(h_acks, 1,
               "the half-buffer window update stopped firing");

    printf("  half-buffer step    %u ack(s), window %lu\n",
           (unsigned int)h_acks, (unsigned long)h_last_window);
}

int main(void)
{
    _nx_tcp_fast_timer_rate     = (NX_IP_PERIODIC_RATE + (NX_TCP_FAST_TIMER_RATE - 1)) / NX_TCP_FAST_TIMER_RATE;
    _nx_tcp_ack_timer_rate      = (NX_IP_PERIODIC_RATE + (NX_TCP_ACK_TIMER_RATE - 1)) / NX_TCP_ACK_TIMER_RATE;
    _nx_tcp_transmit_timer_rate = (NX_IP_PERIODIC_RATE + (NX_TCP_TRANSMIT_TIMER_RATE - 1)) / NX_TCP_TRANSMIT_TIMER_RATE;

    printf("RFC 1122 4.2.2.17 window probes, against the real receive path\n");
    printf("  MSS %lu, buffer %lu, half-buffer step %lu\n",
           (unsigned long)H_MSS, (unsigned long)H_WINDOW,
           (unsigned long)(H_WINDOW / 2UL));

    a_zero_window_probe_is_answered();
    b_runt_window_probe_is_answered();
    c_fully_duplicate_segment_is_answered();
    d_a_bare_ack_is_not_answered();

    printf("the runt-window escape, against the real fast periodic timer\n");

    e_two_segments_escape_a_runt_advertisement();
    f_one_segment_does_not_escape();
    g_a_full_last_advertisement_does_not_escape();
    h_the_half_buffer_step_still_fires();

    printf("%lu checks, %lu failures, %s\n",
           h_checks, h_failures, (h_failures == 0UL) ? "PASS" : "FAIL");

    return (h_failures == 0UL) ? 0 : 1;
}
