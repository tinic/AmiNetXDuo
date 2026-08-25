/*
 * AmiNetXDuo, whether a TCP connection gives up after
 * NX_TCP_MAXIMUM_RETRIES.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nx_api.h"
#include "nx_tcp.h"
#include "nx_packet.h"

/* The library's own verdict on a dead connect, so this tests it and not a
   copy of it.  src/bsdsocket/select.c is its other caller. */
#include "connfail.h"

#include <stdio.h>
#include <string.h>


static ULONG h_now;

#define H_SECONDS(ticks)    ((ticks) / NX_IP_PERIODIC_RATE)

/* The output path reads the clock to stamp the segment it is timing for the
   RFC 6298 estimator.  Nothing here is ever acknowledged, so no sample is ever
   taken; the stub exists so the same simulated time drives both. */
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

#define H_MAX_EVENTS    24

typedef struct
{
    ULONG   sent_at[H_MAX_EVENTS];      /* elapsed seconds of each datagram  */
    UINT    sent;
    ULONG   reset_at;
    UINT    reset;                      /* _nx_tcp_socket_connection_reset   */
    UINT    disconnected;               /* the create-time callback          */
    UINT    disconnect_complete;        /* the extended-notify callback      */
    UINT    ladder_spent;               /* what select.c asked, at the time  */
    ULONG   retries_at_reset;
} h_run;

static h_run h_r;

static void h_record_send(void)
{
    if (h_r.sent < H_MAX_EVENTS)
    {
        h_r.sent_at[h_r.sent] = H_SECONDS(h_now);
    }

    h_r.sent++;
}

static void h_print_ladder(const char *what)
{
    UINT i;

    printf("  %-34s", what);

    for (i = 0; i < h_r.sent && i < H_MAX_EVENTS; i++)
    {
        printf(" +%lus", (unsigned long)h_r.sent_at[i]);
    }

    if (h_r.reset)
    {
        printf("  reset at %lus", (unsigned long)h_r.reset_at);
    }
    else
    {
        printf("  NO RESET");
    }

    printf("\n");
}


ULONG _nx_tcp_fast_timer_rate;
ULONG _nx_tcp_ack_timer_rate;
ULONG _nx_tcp_transmit_timer_rate;

/* send_internal reads this to find out whether it is on the IP thread.  It
   never is here: nothing in this test suspends. */
TX_THREAD *_tx_thread_current_ptr;

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
                        ULONG fragment, ULONG next_protocol,
                        ULONG destination_port)
{
    (void)ip_ptr; (void)destination_ip; (void)type_of_service;
    (void)fragment; (void)next_protocol; (void)destination_port;

    h_record_send();

    /*lint -e{923} suppress cast of ULONG to pointer.  */
    packet_ptr -> nx_packet_queue_next = (NX_PACKET *)NX_DRIVER_TX_DONE;
}

VOID _nx_tcp_packet_send_probe(NX_TCP_SOCKET *socket_ptr, ULONG tx_sequence,
                               UCHAR data)
{
    (void)socket_ptr; (void)tx_sequence; (void)data;
    h_record_send();
}

VOID _nx_tcp_packet_send_ack(NX_TCP_SOCKET *socket_ptr, ULONG tx_sequence)
{
    (void)socket_ptr; (void)tx_sequence;
}

VOID _nx_tcp_packet_send_syn(NX_TCP_SOCKET *socket_ptr, ULONG tx_sequence)
{
    (void)socket_ptr; (void)tx_sequence;
    h_record_send();
}

VOID _nx_tcp_packet_send_fin(NX_TCP_SOCKET *socket_ptr, ULONG tx_sequence)
{
    (void)socket_ptr; (void)tx_sequence;
    h_record_send();
}

/* Reached from the real _nx_tcp_socket_connection_reset() with one segment
   still queued.  Releasing packets needs a pool; unlinking them does not. */
VOID _nx_tcp_socket_transmit_queue_flush(NX_TCP_SOCKET *socket_ptr)
{
    socket_ptr -> nx_tcp_socket_transmit_sent_head  = NX_NULL;
    socket_ptr -> nx_tcp_socket_transmit_sent_tail  = NX_NULL;
    socket_ptr -> nx_tcp_socket_transmit_sent_count = 0;
}

/* Suspension lists are empty throughout, so none of these is called. */
VOID _nx_tcp_receive_cleanup(TX_THREAD *thread_ptr NX_CLEANUP_PARAMETER)
{
    (void)thread_ptr;
    NX_CLEANUP_EXTENSION
}

VOID _nx_tcp_transmit_cleanup(TX_THREAD *thread_ptr NX_CLEANUP_PARAMETER)
{
    (void)thread_ptr;
    NX_CLEANUP_EXTENSION
}

VOID _nx_tcp_connect_cleanup(TX_THREAD *thread_ptr NX_CLEANUP_PARAMETER)
{
    (void)thread_ptr;
    NX_CLEANUP_EXTENSION
}

VOID _nx_tcp_disconnect_cleanup(TX_THREAD *thread_ptr NX_CLEANUP_PARAMETER)
{
    (void)thread_ptr;
    NX_CLEANUP_EXTENSION
}

VOID _nx_tcp_socket_thread_suspend(TX_THREAD **suspension_list_head,
                                   VOID (*suspend_cleanup)(TX_THREAD * NX_CLEANUP_PARAMETER),
                                   NX_TCP_SOCKET *socket_ptr,
                                   TX_MUTEX *mutex_ptr, ULONG wait_option)
{
    (void)suspension_list_head; (void)suspend_cleanup; (void)socket_ptr;
    (void)mutex_ptr; (void)wait_option;
}

UINT _nx_tcp_socket_state_wait(NX_TCP_SOCKET *socket_ptr, UINT desired_state,
                               ULONG wait_option)
{
    (void)socket_ptr; (void)desired_state; (void)wait_option;
    return NX_SUCCESS;
}

UINT _nx_packet_allocate(NX_PACKET_POOL *pool_ptr, NX_PACKET **packet_ptr,
                         ULONG packet_type, ULONG wait_option)
{
    (void)pool_ptr; (void)packet_type; (void)wait_option;
    *packet_ptr = NX_NULL;
    return NX_NO_PACKET;
}

UINT _nx_packet_data_append(NX_PACKET *packet_ptr, VOID *data_start,
                            ULONG data_size, NX_PACKET_POOL *pool_ptr,
                            ULONG wait_option)
{
    (void)packet_ptr; (void)data_start; (void)data_size; (void)pool_ptr;
    (void)wait_option;
    return NX_NO_PACKET;
}

UINT _nx_packet_release(NX_PACKET *packet_ptr)
{
    (void)packet_ptr;
    return NX_SUCCESS;
}


#define H_SEG_BYTES     1460
#define H_BUF           2048

static NX_IP          h_ip;
static NX_INTERFACE   h_iface;
static NX_TCP_SOCKET  h_sock;

static NX_PACKET      h_seg;
static UCHAR          h_seg_buf[H_BUF];

static NX_PACKET      h_out;
static UCHAR          h_out_buf[H_BUF];

static VOID h_disconnect_callback(NX_TCP_SOCKET *socket_ptr)
{
    (void)socket_ptr;
    h_r.disconnected++;
}

static VOID h_disconnect_complete(NX_TCP_SOCKET *socket_ptr)
{
    h_r.disconnect_complete++;
    h_r.reset_at = H_SECONDS(h_now);
    h_r.reset++;

    h_r.ladder_spent     = bsd_connect_ladder_spent(socket_ptr);
    h_r.retries_at_reset = socket_ptr -> nx_tcp_socket_timeout_retries;
}

static void h_fixture(void)
{
    memset(&h_ip, 0, sizeof(h_ip));
    memset(&h_iface, 0, sizeof(h_iface));
    memset(&h_r, 0, sizeof(h_r));
    memset(h_seg_buf, 0, sizeof(h_seg_buf));

    h_now = 0;

    h_iface.nx_interface_ip_address = 0xC0A80102UL;

    _nx_tcp_socket_create(&h_ip, &h_sock, "host retry", NX_IP_NORMAL,
                          NX_FRAGMENT_OKAY, 0x80, 8192, NX_NULL,
                          h_disconnect_callback);

    h_sock.nx_tcp_disconnect_complete_notify = h_disconnect_complete;

    h_sock.nx_tcp_socket_bound_next = &h_sock;
    h_sock.nx_tcp_socket_client_type = NX_TRUE;
    h_sock.nx_tcp_socket_state = NX_TCP_ESTABLISHED;
    h_sock.nx_tcp_socket_port = 40000;
    h_sock.nx_tcp_socket_connect_port = 80;
    h_sock.nx_tcp_socket_connect_ip.nxd_ip_version = NX_IP_VERSION_V4;
    h_sock.nx_tcp_socket_connect_ip.nxd_ip_address.v4 = 0xC0A80101UL;
    h_sock.nx_tcp_socket_connect_interface = &h_iface;
    h_sock.nx_tcp_socket_connect_mss = H_SEG_BYTES;

    h_sock.nx_tcp_socket_tx_window_advertised = 8192;
    h_sock.nx_tcp_socket_tx_window_congestion = H_SEG_BYTES;
    h_sock.nx_tcp_socket_tx_outstanding_bytes = H_SEG_BYTES;
    h_sock.nx_tcp_socket_tx_sequence = 0x10000000UL + H_SEG_BYTES;
    h_sock.nx_tcp_socket_rx_sequence = 0x20000000UL;

    memset(&h_seg, 0, sizeof(h_seg));
    h_seg.nx_packet_data_start  = h_seg_buf;
    h_seg.nx_packet_data_end    = h_seg_buf + H_BUF;
    h_seg.nx_packet_prepend_ptr = h_seg_buf + NX_PHYSICAL_HEADER;
    h_seg.nx_packet_append_ptr  = h_seg_buf + NX_PHYSICAL_HEADER +
                                  sizeof(NX_TCP_HEADER) + H_SEG_BYTES;
    h_seg.nx_packet_length      = sizeof(NX_TCP_HEADER) + H_SEG_BYTES;
    /*lint -e{923} suppress cast of ULONG to pointer.  */
    h_seg.nx_packet_queue_next  = (NX_PACKET *)NX_DRIVER_TX_DONE;
    /*lint -e{923} suppress cast of ULONG to pointer.  */
    h_seg.nx_packet_union_next.nx_packet_tcp_queue_next = (NX_PACKET *)NX_PACKET_ENQUEUED;

    h_sock.nx_tcp_socket_transmit_sent_head  = &h_seg;
    h_sock.nx_tcp_socket_transmit_sent_tail  = &h_seg;
    h_sock.nx_tcp_socket_transmit_sent_count = 1;

    h_sock.nx_tcp_socket_transmit_queue_maximum = 1;

    /* Armed, as nx_tcp_socket_send_internal.c:1054 arms it. */
    h_sock.nx_tcp_socket_timeout         = h_sock.nx_tcp_socket_timeout_rate;
    h_sock.nx_tcp_socket_timeout_retries = 0;
}

static NX_PACKET *h_app_packet(void)
{
    memset(&h_out, 0, sizeof(h_out));
    memset(h_out_buf, 'x', sizeof(h_out_buf));

    h_out.nx_packet_data_start  = h_out_buf;
    h_out.nx_packet_data_end    = h_out_buf + H_BUF;
    h_out.nx_packet_prepend_ptr = h_out_buf + NX_PHYSICAL_HEADER +
                                  20 +                  /* IPv4 header      */
                                  sizeof(NX_TCP_HEADER);
    h_out.nx_packet_append_ptr  = h_out.nx_packet_prepend_ptr + 512;
    h_out.nx_packet_length      = 512;

    return &h_out;
}

#define H_APP_WRITES        0x01u   /* the caller keeps offering data       */
#define H_PEER_ACKS_PROBES  0x02u   /* the peer answers every probe         */

#define H_SLICE_TICKS       10      /* BSD_BREAK_SLICE_TICKS, 200 ms        */

static void h_run_timer(ULONG seconds, UINT behaviour)
{
    ULONG limit = seconds * NX_IP_PERIODIC_RATE;
    ULONG next_slice = H_SLICE_TICKS;

    while ((h_now < limit) && (h_r.reset == 0))
    {
        h_now += _nx_tcp_fast_timer_rate;
        _nx_tcp_fast_periodic_processing(&h_ip);

        if (h_now < next_slice)
        {
            continue;
        }

        next_slice = h_now + H_SLICE_TICKS;

        if ((behaviour & H_APP_WRITES) != 0)
        {
            (VOID)_nx_tcp_socket_send_internal(&h_sock, h_app_packet(), 0);
        }

        /* nx_tcp_socket_state_ack_check.c:616: an ACK that covers the probe
           sequence clears the probe failure count.  A peer that is still
           there does this; one that has gone does not. */
        if ((behaviour & H_PEER_ACKS_PROBES) != 0)
        {
            h_sock.nx_tcp_socket_zero_window_probe_failure = 0;
        }
    }
}


int main(void)
{
    UINT status;

    /* nx_tcp_enable.c:116-121, without the IP instance. */
    _nx_tcp_fast_timer_rate     = (NX_IP_PERIODIC_RATE + (NX_TCP_FAST_TIMER_RATE - 1)) / NX_TCP_FAST_TIMER_RATE;
    _nx_tcp_ack_timer_rate      = (NX_IP_PERIODIC_RATE + (NX_TCP_ACK_TIMER_RATE - 1)) / NX_TCP_ACK_TIMER_RATE;
    _nx_tcp_transmit_timer_rate = (NX_IP_PERIODIC_RATE + (NX_TCP_TRANSMIT_TIMER_RATE - 1)) / NX_TCP_TRANSMIT_TIMER_RATE;

    printf("TCP retransmission limit, against a socket rather than a network\n");

    h_fixture();

    printf("  nx_user.h says      NX_TCP_MAXIMUM_RETRIES=%d NX_TCP_RETRY_SHIFT=%d\n",
           NX_TCP_MAXIMUM_RETRIES, NX_TCP_RETRY_SHIFT);
    printf("  the socket holds    max_retries=%lu shift=%u rate=%lu ticks\n",
           (unsigned long)h_sock.nx_tcp_socket_timeout_max_retries,
           (unsigned int)h_sock.nx_tcp_socket_timeout_shift,
           (unsigned long)h_sock.nx_tcp_socket_timeout_rate);

    h_check(h_sock.nx_tcp_socket_timeout_max_retries == NX_TCP_MAXIMUM_RETRIES,
            "socket_create did not take NX_TCP_MAXIMUM_RETRIES from nx_user.h");
    h_check(h_sock.nx_tcp_socket_timeout_shift == NX_TCP_RETRY_SHIFT,
            "socket_create did not take NX_TCP_RETRY_SHIFT from nx_user.h");
    h_check(h_sock.nx_tcp_socket_timeout_rate == NX_IP_PERIODIC_RATE,
            "the transmit timer is not one second");

    h_fixture();
    h_run_timer(600, 0);
    h_print_ladder("idle socket");

    h_check(h_r.sent == 6, "wrong number of retransmissions before giving up");
    h_check(h_r.reset == 1, "the retransmission limit never reset the connection");
    h_check(h_r.reset_at == 127, "the connection was not abandoned at 127 s");
    h_check(h_sock.nx_tcp_socket_state == NX_TCP_CLOSED,
            "the socket did not end up closed");


    h_check(h_r.disconnect_complete == 1,
            "a retransmission-timeout reset did not run disconnect_complete_notify");
    h_check(h_r.disconnected == 1,
            "a retransmission-timeout reset did not run the disconnect callback");

#ifdef NX_ENABLE_TCP_LOSS_PROBE
    h_fixture();
    h_sock.nx_tcp_socket_rtt_smoothed = 80;
    h_run_timer(600, 0);
    h_print_ladder("measured socket, 200 ms path");

    h_check(h_r.sent == 7,
            "a measured socket did not get its tail loss probe");
    h_check(h_r.sent_at[0] == 0,
            "the tail loss probe did not come before the first rung");
    h_check(h_r.reset_at == 127,
            "the tail loss probe moved when the connection was abandoned");

    h_fixture();
    h_sock.nx_tcp_socket_timeout_rate = 2 * NX_IP_PERIODIC_RATE;
    h_sock.nx_tcp_socket_timeout      = h_sock.nx_tcp_socket_timeout_rate;
    h_run_timer(600, 0);
    h_print_ladder("unmeasured socket, two second timeout");

    h_check(h_r.sent == 7,
            "section 7.2's one second PTO did not probe inside a longer timeout");
    h_check(h_r.sent_at[0] == 1,
            "the one second PTO did not fire at one second");
    h_check(h_r.sent_at[1] == 2,
            "the first rung did not follow the probe at the timeout");
#endif

    h_fixture();

    status = _nx_tcp_socket_send_internal(&h_sock, h_app_packet(), 0);

    printf("  blocked send        status=0x%02x zero_window_probe_has_data=%u"
           " (advertised window %lu)\n",
           status, (unsigned int)h_sock.nx_tcp_socket_zero_window_probe_has_data,
           (unsigned long)h_sock.nx_tcp_socket_tx_window_advertised);

    h_check(status == NX_TX_QUEUE_DEPTH,
            "a send with a full transmit queue did not report queue depth");
    h_check(h_sock.nx_tcp_socket_zero_window_probe_has_data == NX_FALSE,
            "a send blocked by a full queue armed the ZERO-window probe");

    h_fixture();
    h_run_timer(600, H_APP_WRITES);
    h_print_ladder("caller retrying its write");

    h_check(h_r.sent == 6, "a blocked caller changed the retransmission budget");
    h_check(h_r.reset == 1, "a blocked caller stopped the connection giving up");
    h_check(h_r.reset_at == 127,
            "a blocked caller moved when the connection was abandoned");
    h_check(h_r.disconnect_complete == 1,
            "the blocked caller was never told the connection had died");

    h_fixture();
    h_sock.nx_tcp_socket_tx_window_advertised = 0;

    h_run_timer(600, 0);
    h_print_ladder("zero window, probes unanswered");

    h_check(h_r.reset == 1,
            "an unanswered zero-window probe never gave up");
    h_check(h_r.reset_at == 127,
            "the zero-window probe budget is not the retransmission budget");

    h_fixture();
    h_sock.nx_tcp_socket_tx_window_advertised = 0;

    h_run_timer(600, H_PEER_ACKS_PROBES);
    h_print_ladder("zero window, probes answered");

    h_check(h_r.reset == 0,
            "a peer that answered every probe was reset anyway");
    h_check(h_r.sent >= 9,
            "the persist timer stopped probing a live peer");

    h_fixture();

    h_sock.nx_tcp_socket_state = NX_TCP_SYN_SENT;

    /* Nothing has been sent but the SYN, so no transmit queue and no
       outstanding bytes: the SYN branch is the only one that can fire. */
    h_sock.nx_tcp_socket_transmit_sent_head  = NX_NULL;
    h_sock.nx_tcp_socket_transmit_sent_tail  = NX_NULL;
    h_sock.nx_tcp_socket_transmit_sent_count = 0;
    h_sock.nx_tcp_socket_tx_outstanding_bytes = 0;

    h_run_timer(600, 0);
    h_print_ladder("connection request, no answer");

    printf("  nx_user.h says      NX_TCP_SYN_MAXIMUM_RETRIES=%d"
           " NX_TCP_SYN_RETRY_SHIFT_MAX=%d\n",
           NX_TCP_SYN_MAXIMUM_RETRIES, NX_TCP_SYN_RETRY_SHIFT_MAX);

    h_check(h_r.sent == NX_TCP_SYN_MAXIMUM_RETRIES,
            "wrong number of SYN retransmissions before giving up");
    h_check(h_r.reset == 1, "the connection request never gave up");
    h_check(h_r.reset_at >= 180,
            "R2 for a SYN is under RFC 1122 4.2.3.5 MUST-23's three minutes");
    h_check(h_r.reset_at == 191,
            "the SYN ladder did not abandon at 191 s");

    /* The cap, stated on its own: no interval between two SYNs may exceed
       2^NX_TCP_SYN_RETRY_SHIFT_MAX seconds. */
    {
        UINT  i;
        ULONG widest = 0;

        for (i = 1; i < h_r.sent && i < H_MAX_EVENTS; i++)
        {
            ULONG gap = h_r.sent_at[i] - h_r.sent_at[i - 1];

            if (gap > widest)
            {
                widest = gap;
            }
        }

        printf("  widest SYN gap      %lus\n", (unsigned long)widest);

        h_check(widest <= (1UL << NX_TCP_SYN_RETRY_SHIFT_MAX),
                "a SYN retransmission interval ran past the shift cap");
    }

    printf("  at the reset        retries=%lu spent=%u\n",
           (unsigned long)h_r.retries_at_reset, h_r.ladder_spent);

    h_check(h_r.ladder_spent == 1,
            "a SYN ladder that ran out does not read as a timeout");

    h_fixture();

    h_sock.nx_tcp_socket_state = NX_TCP_SYN_SENT;
    h_sock.nx_tcp_socket_transmit_sent_head  = NX_NULL;
    h_sock.nx_tcp_socket_transmit_sent_tail  = NX_NULL;
    h_sock.nx_tcp_socket_transmit_sent_count = 0;
    h_sock.nx_tcp_socket_tx_outstanding_bytes = 0;

    h_run_timer(5, 0);

    printf("  RST after %lu SYNs   retries=%lu spent=%u\n",
           (unsigned long)h_r.sent,
           (unsigned long)h_sock.nx_tcp_socket_timeout_retries,
           bsd_connect_ladder_spent(&h_sock));

    h_check(h_r.reset == 0, "the SYN ladder gave up inside five seconds");

    _nx_tcp_socket_connection_reset(&h_sock);

    h_check(h_r.reset == 1, "an RST in SYN_SENT did not reset the connection");
    h_check(h_r.ladder_spent == 0,
            "a refusal that arrived early reads as a timeout");

    h_fixture();

    h_check(h_sock.nx_tcp_socket_user_timeout == 0,
            "socket_create left a deadline on a socket that asked for none");
    h_check(h_sock.nx_tcp_socket_stall_ticks == 0,
            "socket_create left the stall clock running");

    h_run_timer(10, 0);

    printf("  10 s into a stall   stall=%lus retries=%lu rto_left=%lus\n",
           (unsigned long)H_SECONDS(h_sock.nx_tcp_socket_stall_ticks),
           (unsigned long)h_sock.nx_tcp_socket_timeout_retries,
           (unsigned long)H_SECONDS(h_sock.nx_tcp_socket_timeout));

    h_check(h_r.reset == 0, "the ladder gave up early");
    h_check(H_SECONDS(h_sock.nx_tcp_socket_stall_ticks) == 10,
            "the stall clock does not read the time since the peer last ACKed");
    h_check(h_sock.nx_tcp_socket_timeout_retries == 3,
            "the retransmit count is not readable part-way through the ladder");

    /* And it stops the moment there is nothing outstanding, so a healthy
       connection reads zero rather than its age. */
    h_sock.nx_tcp_socket_transmit_sent_head = NX_NULL;
    h_sock.nx_tcp_socket_transmit_sent_tail = NX_NULL;
    h_sock.nx_tcp_socket_timeout            = 0;

    h_now += _nx_tcp_fast_timer_rate;
    _nx_tcp_fast_periodic_processing(&h_ip);

    h_check(h_sock.nx_tcp_socket_stall_ticks == 0,
            "the stall clock kept running with nothing outstanding");

    h_fixture();
    h_sock.nx_tcp_socket_user_timeout = 20UL * NX_IP_PERIODIC_RATE;

    h_run_timer(600, H_APP_WRITES);
    h_print_ladder("20 s deadline, data");

    h_check(h_r.reset == 1, "a socket with a deadline never gave up");
    h_check(h_r.reset_at == 20,
            "a 20 s deadline was not served at 20 s");
    h_check(h_r.disconnect_complete == 1,
            "a deadline expiry did not tell the application");
    h_check(h_sock.nx_tcp_socket_state == NX_TCP_CLOSED,
            "a deadline expiry left the socket open");

    h_fixture();
    h_sock.nx_tcp_socket_state = NX_TCP_SYN_SENT;
    h_sock.nx_tcp_socket_transmit_sent_head  = NX_NULL;
    h_sock.nx_tcp_socket_transmit_sent_tail  = NX_NULL;
    h_sock.nx_tcp_socket_transmit_sent_count = 0;
    h_sock.nx_tcp_socket_tx_outstanding_bytes = 0;
    h_sock.nx_tcp_socket_user_timeout = 15UL * NX_IP_PERIODIC_RATE;

    h_run_timer(600, 0);
    h_print_ladder("15 s deadline, connect");

    h_check(h_r.reset == 1, "a connection request with a deadline never gave up");
    h_check(h_r.reset_at == 15,
            "a 15 s deadline did not bound the 191 s connect ladder");

    printf("%lu checks, %lu failures, %s\n",
           h_checks, h_failures, (h_failures == 0UL) ? "PASS" : "FAIL");

    return (h_failures == 0UL) ? 0 : 1;
}
