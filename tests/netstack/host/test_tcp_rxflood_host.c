/*
 * AmiNetXDuo, receive-queue exhaustion by a sub-MSS peer: what bounds the
 * number of packets a chatty peer can pin, against the real enqueue path.
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

TX_THREAD *_tx_thread_current_ptr;

ULONG _nx_tcp_fast_timer_rate;
ULONG _nx_tcp_ack_timer_rate;
ULONG _nx_tcp_transmit_timer_rate;
ULONG _nx_tcp_2MSL_timer_rate;

UINT _nx_tcp_mss_option_get(UCHAR *option_ptr, ULONG option_area_size, ULONG *mss)
{
    (void)option_ptr; (void)option_area_size; (void)mss;
    return NX_TRUE;
}

VOID _nx_tcp_packet_send_rst(NX_TCP_SOCKET *socket_ptr, NX_TCP_HEADER *header_ptr)
{
    (void)socket_ptr; (void)header_ptr;
}

VOID _nx_tcp_sack_option_get(NX_TCP_SOCKET *socket_ptr, UCHAR *option_ptr,
                             ULONG option_area_size)
{
    (void)socket_ptr; (void)option_ptr; (void)option_area_size;
}

VOID _nx_tcp_socket_connection_reset(NX_TCP_SOCKET *socket_ptr)
{
    (void)socket_ptr;
}

UINT _nx_tcp_socket_state_ack_check(NX_TCP_SOCKET *socket_ptr,
                                    NX_TCP_HEADER *header_ptr)
{
    (void)socket_ptr; (void)header_ptr;
    return NX_TRUE;
}

VOID _nx_tcp_socket_state_closing(NX_TCP_SOCKET *socket_ptr,
                                  NX_TCP_HEADER *header_ptr)
{
    (void)socket_ptr; (void)header_ptr;
}

VOID _nx_tcp_socket_state_established(NX_TCP_SOCKET *socket_ptr)
{
    (void)socket_ptr;
}

VOID _nx_tcp_socket_state_fin_wait1(NX_TCP_SOCKET *socket_ptr)
{
    (void)socket_ptr;
}

VOID _nx_tcp_socket_state_fin_wait2(NX_TCP_SOCKET *socket_ptr)
{
    (void)socket_ptr;
}

VOID _nx_tcp_socket_state_last_ack(NX_TCP_SOCKET *socket_ptr,
                                   NX_TCP_HEADER *header_ptr)
{
    (void)socket_ptr; (void)header_ptr;
}

VOID _nx_tcp_socket_state_syn_received(NX_TCP_SOCKET *socket_ptr,
                                       NX_TCP_HEADER *header_ptr)
{
    (void)socket_ptr; (void)header_ptr;
}

VOID _nx_tcp_socket_state_syn_sent(NX_TCP_SOCKET *socket_ptr,
                                   NX_TCP_HEADER *header_ptr,
                                   NX_PACKET *packet_ptr)
{
    (void)socket_ptr; (void)header_ptr; (void)packet_ptr;
}

VOID _nx_tcp_socket_state_transmit_check(NX_TCP_SOCKET *socket_ptr)
{
    (void)socket_ptr;
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

/* Every released packet is one returned to the pool.  The reproduction never
   releases (there is no reader and the flood is in order), but the fix's
   tail-drop does, and the count has to come back. */
static ULONG h_pool_available;

UINT _nx_packet_release(NX_PACKET *packet_ptr)
{
    (void)packet_ptr;
    h_pool_available++;
    return NX_SUCCESS;
}

VOID _nx_tcp_packet_send_ack(NX_TCP_SOCKET *socket_ptr, ULONG tx_sequence)
{
    (void)socket_ptr; (void)tx_sequence;
}

ULONG _nx_tcp_socket_window_update_step(NX_TCP_SOCKET *socket_ptr)
{
    /* Half the buffer, the same threshold data_check's byte-ACK path wants.
       Its value does not steer this test; it only needs to be callable. */
    return socket_ptr -> nx_tcp_socket_rx_window_default / 2;
}

/* The lab's 8 MB A1200: 368 packets, an eighth share, a 72,128-byte window. */
#define H_POOL_TOTAL        368
#define H_WINDOW            72128UL
#define H_MSS               1460UL
#ifndef H_ISN_RX
#define H_ISN_RX            0x20000000UL
#endif

/* Enough backing packets to let the defect run past the pool without the test
   itself running out.  On the fixed build the queue caps long before this. */
#define H_MAX_SEG           2048
#define H_BUF               64

static NX_IP          h_ip;
static NX_INTERFACE   h_iface;
static NX_TCP_SOCKET  h_sock;
static NX_PACKET_POOL h_pool;   /* low_watermark stays 0: the global guard is
                                   inert, exactly as the shipped build leaves it */

static NX_PACKET      h_pkt[H_MAX_SEG];
static UCHAR          h_pkt_buf[H_MAX_SEG][H_BUF];
static UINT           h_pkt_next;

static void h_fixture(void)
{
    memset(&h_ip, 0, sizeof(h_ip));
    memset(&h_iface, 0, sizeof(h_iface));

    h_now            = 1000;
    h_pkt_next       = 0;
    h_pool_available = H_POOL_TOTAL;

    memset(&h_pool, 0, sizeof(h_pool));
    h_pool.nx_packet_pool_total     = H_POOL_TOTAL;
    h_pool.nx_packet_pool_available = H_POOL_TOTAL;

    h_iface.nx_interface_ip_address = 0xC0A80102UL;

    _nx_tcp_socket_create(&h_ip, &h_sock, "host rx flood", NX_IP_NORMAL,
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

    h_sock.nx_tcp_socket_rx_sequence         = H_ISN_RX;
    h_sock.nx_tcp_socket_rx_window_default   = H_WINDOW;
    h_sock.nx_tcp_socket_rx_window_current   = H_WINDOW;
    h_sock.nx_tcp_socket_rx_window_last_sent = H_WINDOW;

    h_sock.nx_tcp_socket_receive_queue_head  = NX_NULL;
    h_sock.nx_tcp_socket_receive_queue_tail  = NX_NULL;
    h_sock.nx_tcp_socket_receive_queue_count = 0;
    h_sock.nx_tcp_socket_receive_suspension_list = NX_NULL;
    h_sock.nx_tcp_receive_callback = NX_NULL;

#ifdef NX_ENABLE_LOW_WATERMARK
    h_sock.nx_tcp_socket_receive_queue_maximum = (H_WINDOW / H_MSS) + 4UL;
#endif
}

/* One in-order segment carrying seg_bytes of payload.  Returns the queued
   packet count after data_check has run. */
static ULONG h_feed(ULONG seg_bytes)
{
    NX_PACKET     *p;
    NX_TCP_HEADER *hdr;

    if (h_pkt_next >= H_MAX_SEG)
    {
        printf("FAIL out of test packets\n");
        h_failures++;
        return h_sock.nx_tcp_socket_receive_queue_count;
    }

    p = &h_pkt[h_pkt_next];
    memset(p, 0, sizeof(*p));
    memset(h_pkt_buf[h_pkt_next], 'x', H_BUF);

    p -> nx_packet_data_start  = h_pkt_buf[h_pkt_next];
    p -> nx_packet_data_end    = h_pkt_buf[h_pkt_next] + H_BUF;
    p -> nx_packet_prepend_ptr = h_pkt_buf[h_pkt_next];
    p -> nx_packet_append_ptr  = h_pkt_buf[h_pkt_next] +
                                 sizeof(NX_TCP_HEADER) + seg_bytes;
    p -> nx_packet_length      = sizeof(NX_TCP_HEADER) + seg_bytes;
    p -> nx_packet_pool_owner  = &h_pool;

    hdr = (NX_TCP_HEADER *)p -> nx_packet_prepend_ptr;
    memset(hdr, 0, sizeof(*hdr));
    hdr -> nx_tcp_header_word_0   = (80UL << NX_SHIFT_BY_16) | 40000UL;
    hdr -> nx_tcp_sequence_number = h_sock.nx_tcp_socket_rx_sequence;
    hdr -> nx_tcp_header_word_3   = NX_TCP_HEADER_SIZE | NX_TCP_ACK_BIT | 65535UL;

    /* A segment that data_check keeps pins one packet out of the pool. */
    if (h_pool_available > 0)
    {
        h_pool_available--;
    }

    h_pkt_next++;

    (VOID)_nx_tcp_socket_state_data_check(&h_sock, p);

    return h_sock.nx_tcp_socket_receive_queue_count;
}

static void sub_mss_flood(void)
{
    ULONG i;
    ULONG queued;
    ULONG budget_packets = H_WINDOW / H_MSS;   /* what a full-MSS peer pins */

    h_fixture();

    /* A peer that ignores segment size and sends one byte at a time.  Push far
       past the pool: on the broken build every one of these is admitted. */
    for (i = 0; i < (ULONG)(H_POOL_TOTAL * 3); i++)
    {
        queued = h_feed(1);

        /* Stop early once the queue is clearly bounded, so a fixed build does
           not spend 1100 iterations after the cap has plainly held. */
        if (queued < i && i > (budget_packets + 32UL))
        {
            break;
        }
    }

    queued = h_sock.nx_tcp_socket_receive_queue_count;

    printf("  sub-MSS flood       %lu one-byte segments offered, %lu queued\n",
           (unsigned long)h_pkt_next, (unsigned long)queued);
    printf("  window after        %lu bytes of %lu still advertised (%.1f%% open)\n",
           (unsigned long)h_sock.nx_tcp_socket_rx_window_current,
           (unsigned long)H_WINDOW,
           100.0 * (double)h_sock.nx_tcp_socket_rx_window_current /
               (double)H_WINDOW);
    printf("  pool               %lu of %d packets free\n",
           (unsigned long)h_pool_available, H_POOL_TOTAL);

    h_check(queued <= budget_packets + 8UL,
            "a sub-MSS flood pinned more packets than a full-MSS window's worth "
            "(no per-socket receive-queue bound)");

    /* And the pool must not have drained: the whole defect is packets pinned
       against a window that still says it has room. */
    h_check(h_pool_available > 0,
            "the packet pool drained to empty under a sub-MSS flood");
}

static void full_mss_not_starved(void)
{
    ULONG i;
    ULONG budget_packets = H_WINDOW / H_MSS;
    ULONG queued = 0;

    h_fixture();

    for (i = 0; i < budget_packets; i++)
    {
        queued = h_feed(H_MSS);
    }

    printf("  full-MSS window     %lu MSS segments offered, %lu queued\n",
           (unsigned long)budget_packets, (unsigned long)queued);

    h_check(queued == budget_packets,
            "a full-MSS peer was capped before it filled the byte window "
            "(clean-link read would regress)");
}

/* GRO can represent a run of wire segments as one packet chain.  A
   retransmitted prefix may put the chain's first byte below RCV.NXT while
   later, new segments put its last byte beyond the right edge.  Neither end
   is in the receive window, but the chain covers the whole window and must be
   admitted so data_check can trim both sides. */
static void aggregate_covering_window_is_accepted(void)
{
    NX_PACKET p[3];
    UCHAR      b[3][1100];
    NX_TCP_HEADER *hdr;
    ULONG dropped;

    h_fixture();
    h_sock.nx_tcp_socket_rx_window_default   = 2000;
    h_sock.nx_tcp_socket_rx_window_current   = 2000;
    h_sock.nx_tcp_socket_rx_window_last_sent = 2000;

    memset(p, 0, sizeof(p));
    memset(b, 'x', sizeof(b));

    p[0].nx_packet_data_start  = b[0];
    p[0].nx_packet_data_end    = b[0] + sizeof(b[0]);
    p[0].nx_packet_prepend_ptr = b[0];
    p[0].nx_packet_append_ptr  = b[0] + sizeof(NX_TCP_HEADER) + 1000;
    p[0].nx_packet_length      = sizeof(NX_TCP_HEADER) + 3000;
    p[0].nx_packet_pool_owner  = &h_pool;
    p[0].nx_packet_next        = &p[1];
    p[0].nx_packet_last        = &p[2];

    p[1].nx_packet_data_start  = b[1];
    p[1].nx_packet_data_end    = b[1] + sizeof(b[1]);
    p[1].nx_packet_prepend_ptr = b[1];
    p[1].nx_packet_append_ptr  = b[1] + 1000;
    p[1].nx_packet_pool_owner  = &h_pool;
    p[1].nx_packet_next        = &p[2];

    p[2].nx_packet_data_start  = b[2];
    p[2].nx_packet_data_end    = b[2] + sizeof(b[2]);
    p[2].nx_packet_prepend_ptr = b[2];
    p[2].nx_packet_append_ptr  = b[2] + 1000;
    p[2].nx_packet_pool_owner  = &h_pool;

    hdr = (NX_TCP_HEADER *)p[0].nx_packet_prepend_ptr;
    memset(hdr, 0, sizeof(*hdr));
    hdr->nx_tcp_header_word_0   = (80UL << NX_SHIFT_BY_16) | 40000UL;
    hdr->nx_tcp_sequence_number = H_ISN_RX - 500UL;
    hdr->nx_tcp_header_word_3   = NX_TCP_HEADER_SIZE | NX_TCP_ACK_BIT | 65535UL;

    dropped = h_ip.nx_ip_tcp_receive_packets_dropped;
    _nx_tcp_socket_packet_process(&h_sock, &p[0]);

    h_check(h_ip.nx_ip_tcp_receive_packets_dropped == dropped,
            "a GRO run covering the receive window was dropped");
    h_check(h_sock.nx_tcp_socket_receive_queue_count == 1,
            "a GRO run covering the receive window was not queued");
    h_check(h_sock.nx_tcp_socket_rx_sequence == H_ISN_RX + 2000UL,
            "the covered receive window was not consumed exactly");
    h_check(p[0].nx_packet_length == sizeof(NX_TCP_HEADER) + 2000UL,
            "the GRO run was not trimmed to the receive window");

    /* The overlap test must not turn either adjacent half-open range into an
       accepted segment. */
    h_fixture();
    h_sock.nx_tcp_socket_rx_window_default   = 2000;
    h_sock.nx_tcp_socket_rx_window_current   = 2000;
    h_sock.nx_tcp_socket_rx_window_last_sent = 2000;
    memset(&p[0], 0, sizeof(p[0]));
    p[0].nx_packet_data_start  = b[0];
    p[0].nx_packet_data_end    = b[0] + sizeof(b[0]);
    p[0].nx_packet_prepend_ptr = b[0];
    p[0].nx_packet_append_ptr  = b[0] + sizeof(NX_TCP_HEADER) + 100;
    p[0].nx_packet_length      = sizeof(NX_TCP_HEADER) + 100;
    p[0].nx_packet_pool_owner  = &h_pool;
    hdr = (NX_TCP_HEADER *)p[0].nx_packet_prepend_ptr;
    memset(hdr, 0, sizeof(*hdr));
    hdr->nx_tcp_sequence_number = H_ISN_RX - 200UL;
    hdr->nx_tcp_header_word_3   = NX_TCP_HEADER_SIZE | NX_TCP_ACK_BIT;
    dropped = h_ip.nx_ip_tcp_receive_packets_dropped;
    _nx_tcp_socket_packet_process(&h_sock, &p[0]);
    h_check(h_ip.nx_ip_tcp_receive_packets_dropped == dropped + 1UL &&
                h_sock.nx_tcp_socket_receive_queue_count == 0,
            "a segment wholly below the receive window was accepted");

    h_fixture();
    h_sock.nx_tcp_socket_rx_window_default   = 2000;
    h_sock.nx_tcp_socket_rx_window_current   = 2000;
    h_sock.nx_tcp_socket_rx_window_last_sent = 2000;
    memset(&p[0], 0, sizeof(p[0]));
    p[0].nx_packet_data_start  = b[0];
    p[0].nx_packet_data_end    = b[0] + sizeof(b[0]);
    p[0].nx_packet_prepend_ptr = b[0];
    p[0].nx_packet_append_ptr  = b[0] + sizeof(NX_TCP_HEADER) + 100;
    p[0].nx_packet_length      = sizeof(NX_TCP_HEADER) + 100;
    p[0].nx_packet_pool_owner  = &h_pool;
    hdr = (NX_TCP_HEADER *)p[0].nx_packet_prepend_ptr;
    memset(hdr, 0, sizeof(*hdr));
    hdr->nx_tcp_sequence_number = H_ISN_RX + 2000UL;
    hdr->nx_tcp_header_word_3   = NX_TCP_HEADER_SIZE | NX_TCP_ACK_BIT;
    dropped = h_ip.nx_ip_tcp_receive_packets_dropped;
    _nx_tcp_socket_packet_process(&h_sock, &p[0]);
    h_check(h_ip.nx_ip_tcp_receive_packets_dropped == dropped + 1UL &&
                h_sock.nx_tcp_socket_receive_queue_count == 0,
            "a segment beginning at the receive window's right edge was accepted");
}

int main(void)
{
    _nx_tcp_fast_timer_rate     = (NX_IP_PERIODIC_RATE + (NX_TCP_FAST_TIMER_RATE - 1)) / NX_TCP_FAST_TIMER_RATE;
    _nx_tcp_ack_timer_rate      = (NX_IP_PERIODIC_RATE + (NX_TCP_ACK_TIMER_RATE - 1)) / NX_TCP_ACK_TIMER_RATE;
    _nx_tcp_transmit_timer_rate = (NX_IP_PERIODIC_RATE + (NX_TCP_TRANSMIT_TIMER_RATE - 1)) / NX_TCP_TRANSMIT_TIMER_RATE;

    printf("receive-queue exhaustion by a sub-MSS peer, against the real "
           "enqueue path\n");
#ifdef NX_ENABLE_LOW_WATERMARK
    printf("  bound               receive_queue_maximum = %lu packets\n",
           (unsigned long)((H_WINDOW / H_MSS) + 4UL));
#else
    printf("  bound               none compiled in (NX_ENABLE_LOW_WATERMARK off)\n");
#endif

    sub_mss_flood();
    full_mss_not_starved();
    aggregate_covering_window_is_accepted();

    printf("%lu checks, %lu failures, %s\n",
           h_checks, h_failures, (h_failures == 0UL) ? "PASS" : "FAIL");

    return (h_failures == 0UL) ? 0 : 1;
}
