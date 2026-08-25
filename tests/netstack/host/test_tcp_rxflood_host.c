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
#define H_ISN_RX            0x20000000UL

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

    printf("%lu checks, %lu failures, %s\n",
           h_checks, h_failures, (h_failures == 0UL) ? "PASS" : "FAIL");

    return (h_failures == 0UL) ? 0 : 1;
}
