/*
 * AmiNetXDuo, whether a TCP connection gives up after
 * NX_TCP_MAXIMUM_RETRIES.
 *
 * Over an impaired link, loss, a 576-byte path MTU, and a router silently
 * dropping the oversized datagrams, a socket retransmitted the same sequence
 * number at +1, +2, +4, +8, +16, +32, +64 and +128 seconds.  The doubling is
 * NX_TCP_RETRY_SHIFT 1, so port/netxduo-amiga/inc/nx_user.h had plainly been
 * read; the +128 is a retry that NX_TCP_MAXIMUM_RETRIES 6 should never have
 * allowed, and 600 s later curl was still blocked with an empty stderr.
 *
 * Two explanations fit that from the outside, the macro did not reach the
 * vendored translation unit, or the limit is tested against the wrong counter
 * and they need opposite fixes.  A packet capture cannot separate them,
 * because the interval ladder is driven by nx_tcp_socket_timeout_retries and
 * looks identical either way.  So this prints what the socket holds after
 * nx_tcp_socket_create() and then drives the timer that has to act on it.
 *
 * Real, compiled from third_party/netxduo/common/src into this binary:
 * nx_tcp_socket_create.c, nx_tcp_socket_send_internal.c,
 * nx_tcp_socket_retransmit.c, nx_tcp_fast_periodic_processing.c,
 * nx_tcp_socket_connection_reset.c and nx_tcp_socket_block_cleanup.c, the
 * whole decision path from "the application could not queue a segment" to
 * "the connection is reset and the application is told".
 *
 * Stubbed: everything that would touch a driver, a packet pool or another
 * thread.  _nx_ip_packet_send() counts the datagram and hands the packet back
 * marked NX_DRIVER_TX_DONE, which is what a SANA-II transmit completion does
 * and what the retransmit path needs to see to send again.
 *
 * The clock is a loop rather than a timer: one call to
 * _nx_tcp_fast_periodic_processing() is one NX_TCP_FAST_TIMER_RATE tick, so
 * 600 simulated seconds cost microseconds and the emulator is not involved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nx_api.h"
#include "nx_tcp.h"
#include "nx_packet.h"

#include <stdio.h>
#include <string.h>


/* ---------------------------------------------------------- the clock ----- */

/*
 * Simulated time, in ThreadX ticks, advanced by the tick loop below.  The
 * stubs read it to stamp every datagram with the moment it went out, so the
 * checks can test the interval ladder rather than the count alone.
 */
static ULONG h_now;

#define H_SECONDS(ticks)    ((ticks) / NX_IP_PERIODIC_RATE)

/* The output path reads the clock to stamp the segment it is timing for the
   RFC 6298 estimator.  Nothing here is ever acknowledged, so no sample is ever
   taken; the stub exists so the same simulated time drives both. */
ULONG _tx_time_get(VOID)
{
    return h_now;
}


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

#define H_MAX_EVENTS    24

typedef struct
{
    ULONG   sent_at[H_MAX_EVENTS];      /* elapsed seconds of each datagram  */
    UINT    sent;
    ULONG   reset_at;
    UINT    reset;                      /* _nx_tcp_socket_connection_reset   */
    UINT    disconnected;               /* the create-time callback          */
    UINT    disconnect_complete;        /* the extended-notify callback      */
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


/* --------------------------------------------------------------- stubs ---- */

/*
 * NetX Duo's timer rates.  nx_tcp_enable.c computes them from
 * NX_IP_PERIODIC_RATE and would drag the whole IP enable path in with it, so
 * the three lines that matter are reproduced here (nx_tcp_enable.c:111-116).
 */
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

/*
 * NX_ASSERT parks the calling thread here.  Nothing below trips one, and a
 * stub that returned would let the test carry on inside a state NetX Duo has
 * already called impossible.
 */
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

/*
 * A retransmission clears nx_packet_queue_next before sending
 * (nx_tcp_socket_retransmit.c:386) and only sends again once the driver has
 * put NX_DRIVER_TX_DONE back, so this stub does what the SANA-II transmit
 * completion does.  Without it the ladder stops after one rung for a reason
 * that has nothing to do with the retry limit.
 */
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


/* ------------------------------------------------------------- fixture ---- */

/*
 * A socket in the middle of the failure: ESTABLISHED, one full segment sent
 * and unacknowledged, the peer advertising a healthy window, and a retransmit
 * timeout armed.  Nothing from the peer ever arrives again, because the
 * datagrams are being dropped by a router that says nothing.
 */

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

/*
 * src/bsdsocket/select.c:141 installs bsd_tcp_disconnect_complete_notify()
 * here through nx_tcp_socket_disconnect_complete_notify(), and that callback
 * is the only thing that sets ASF_EOF on a socket nobody has FIN'd:
 * bsd_writable() (select.c:504) answers FALSE for every state but
 * NX_TCP_ESTABLISHED, so without this a reset socket is neither readable nor
 * writable and WaitSelect() never returns.  Whether it fires for a reset that
 * came from the retransmit timer rather than from a peer's FIN or RST was
 * untested, so the reset path is run for real and this counts it.
 */
static VOID h_disconnect_complete(NX_TCP_SOCKET *socket_ptr)
{
    (void)socket_ptr;
    h_r.disconnect_complete++;
    h_r.reset_at = H_SECONDS(h_now);
    h_r.reset++;
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

    /* Bound, connected, and talking to 192.168.1.1. */
    h_sock.nx_tcp_socket_bound_next = &h_sock;
    h_sock.nx_tcp_socket_client_type = NX_TRUE;
    h_sock.nx_tcp_socket_state = NX_TCP_ESTABLISHED;
    h_sock.nx_tcp_socket_port = 40000;
    h_sock.nx_tcp_socket_connect_port = 80;
    h_sock.nx_tcp_socket_connect_ip.nxd_ip_version = NX_IP_VERSION_V4;
    h_sock.nx_tcp_socket_connect_ip.nxd_ip_address.v4 = 0xC0A80101UL;
    h_sock.nx_tcp_socket_connect_interface = &h_iface;
    h_sock.nx_tcp_socket_connect_mss = H_SEG_BYTES;

    /*
     * The peer's window is wide open: nothing here is a zero-window
     * condition.  The only reason a further send cannot proceed is that one
     * segment is already in flight and the congestion window is one segment,
     * which is where a retransmit timeout leaves it
     * (nx_tcp_socket_retransmit.c:182).
     */
    h_sock.nx_tcp_socket_tx_window_advertised = 8192;
    h_sock.nx_tcp_socket_tx_window_congestion = H_SEG_BYTES;
    h_sock.nx_tcp_socket_tx_outstanding_bytes = H_SEG_BYTES;
    h_sock.nx_tcp_socket_tx_sequence = 0x10000000UL + H_SEG_BYTES;
    h_sock.nx_tcp_socket_rx_sequence = 0x20000000UL;

    /* The segment that is not getting through. */
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

    /*
     * And the transmit queue is full.  An application with more to say than
     * the connection will take fills the queue to
     * nx_tcp_socket_transmit_queue_maximum (NX_TCP_MAXIMUM_TX_QUEUE, twenty
     * segments) and stays there for as long as nothing is acknowledged, which
     * is the state any stalled upload, POST or TLS handshake is in.  One
     * segment and a maximum of one is the same state with less bookkeeping:
     * no further send can be queued no matter what the windows do.
     */
    h_sock.nx_tcp_socket_transmit_queue_maximum = 1;

    /* Armed, as nx_tcp_socket_send_internal.c:877 arms it. */
    h_sock.nx_tcp_socket_timeout         = h_sock.nx_tcp_socket_timeout_rate;
    h_sock.nx_tcp_socket_timeout_retries = 0;
}

/* One more segment the application wants to send while the first is stuck. */
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

/*
 * H_APP_WRITES models a caller stuck in send().  bsd_wait_sliced()
 * (src/bsdsocket/select.c:374) re-enters nx_tcp_socket_send() every
 * BSD_BREAK_SLICE_TICKS, 200 ms, for as long as the status is
 * NX_WINDOW_OVERFLOW or NX_TX_QUEUE_DEPTH, so that Ctrl-C still works; a
 * non-blocking caller such as curl comes back on its select loop just as
 * often, and for the same two statuses.  Either way the application offers
 * data several times a second throughout the ladder below, which decides
 * whether the ladder ends.
 */
#define H_APP_WRITES        0x01u   /* the caller keeps offering data       */
#define H_PEER_ACKS_PROBES  0x02u   /* the peer answers every probe         */

#define H_SLICE_TICKS       10      /* BSD_BREAK_SLICE_TICKS, 200 ms        */

/*
 * Run the fast periodic timer for at most `seconds` of simulated time, or
 * until the connection is reset.  One iteration is one NX_TCP_FAST_TIMER_RATE
 * tick, which is what the IP thread does.
 */
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

        /* nx_tcp_socket_state_ack_check.c:555: an ACK that covers the probe
           sequence clears the probe failure count.  A peer that is still
           there does this; one that has gone does not. */
        if ((behaviour & H_PEER_ACKS_PROBES) != 0)
        {
            h_sock.nx_tcp_socket_zero_window_probe_failure = 0;
        }
    }
}


/* --------------------------------------------------------------- cases ---- */

int main(void)
{
    UINT status;

    /* nx_tcp_enable.c:111-116, without the IP instance. */
    _nx_tcp_fast_timer_rate     = (NX_IP_PERIODIC_RATE + (NX_TCP_FAST_TIMER_RATE - 1)) / NX_TCP_FAST_TIMER_RATE;
    _nx_tcp_ack_timer_rate      = (NX_IP_PERIODIC_RATE + (NX_TCP_ACK_TIMER_RATE - 1)) / NX_TCP_ACK_TIMER_RATE;
    _nx_tcp_transmit_timer_rate = (NX_IP_PERIODIC_RATE + (NX_TCP_TRANSMIT_TIMER_RATE - 1)) / NX_TCP_TRANSMIT_TIMER_RATE;

    printf("TCP retransmission limit, against a socket rather than a network\n");

    /* ---- 1. what nx_tcp_socket_create() actually left in the socket ---- */
    /*
     * These numbers decide between "the macro did not arrive" and "the limit
     * is tested against the wrong counter", so they are printed as well as
     * checked.
     */
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

    /* ---- 2. the ladder, with nothing else going on ---------------------- */
    /*
     * Six retries at a shift of one is 1, 2, 4, 8, 16, 32 and 64 seconds of
     * waiting, six datagrams, at +1, +3, +7, +15, +31 and +63, and the
     * expiry of that last 64 s interval, at 127 s, is a reset rather than a
     * seventh datagram.
     */
    h_fixture();
    h_run_timer(600, 0);
    h_print_ladder("idle socket");

    h_check(h_r.sent == 6, "wrong number of retransmissions before giving up");
    h_check(h_r.reset == 1, "the retransmission limit never reset the connection");
    h_check(h_r.reset_at == 127, "the connection was not abandoned at 127 s");
    h_check(h_sock.nx_tcp_socket_state == NX_TCP_CLOSED,
            "the socket did not end up closed");

    /* ---- 3. the caller has to be told --------------------------------- */
    h_check(h_r.disconnect_complete == 1,
            "a retransmission-timeout reset did not run disconnect_complete_notify");
    h_check(h_r.disconnected == 1,
            "a retransmission-timeout reset did not run the disconnect callback");

    /* ---- 4. one send that cannot be queued ----------------------------- */
    /*
     * Nothing can be queued, the transmit queue is full and one segment is
     * already in flight against a one-segment congestion window, so
     * _nx_tcp_socket_send_internal() answers NX_TX_QUEUE_DEPTH, which the BSD
     * layer turns into EWOULDBLOCK and bsd_wait_sliced() retries.
     *
     * That path also used to declare the socket to be probing a zero window,
     * which it is not: the peer is advertising 8 KB and has never asked us to
     * stop.  The flag is what nx_tcp_fast_periodic_processing.c:129 uses to
     * decide which counter the retry limit is tested against, so setting it
     * here aimed the test at nx_tcp_socket_zero_window_probe_failure, which
     * the ordinary data path never advances.  The neighbouring arm of the
     * same else, NX_WINDOW_OVERFLOW, when the queue has room but the window
     * does not, set it in exactly the same way.
     */
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

    /* ---- 5. the reproduction: a caller that keeps trying to write ------ */
    /*
     * One blocked send is not enough to hide the limit, because the next
     * retransmission clears the flag again (nx_tcp_socket_retransmit.c:158).
     * A caller stuck in send() re-arms it every 200 ms, which is faster than
     * the ladder doubles, so from the second rung onwards the flag is set
     * every time the timer looks, and the connection is never abandoned.
     * This is the 600-second stall, reproduced without a network.
     */
    h_fixture();
    h_run_timer(600, H_APP_WRITES);
    h_print_ladder("caller retrying its write");

    h_check(h_r.sent == 6, "a blocked caller changed the retransmission budget");
    h_check(h_r.reset == 1, "a blocked caller stopped the connection giving up");
    h_check(h_r.reset_at == 127,
            "a blocked caller moved when the connection was abandoned");
    h_check(h_r.disconnect_complete == 1,
            "the blocked caller was never told the connection had died");

    /* ---- 6. a genuine zero window, with nobody answering the probes ---- */
    /*
     * Here the flag is right: the peer really has closed its window, so the
     * limit is tested against nx_tcp_socket_zero_window_probe_failure, and
     * that counter has to be allowed to grow.  It was reset on every probe,
     * so it never passed 1 and the second arm of the test could not fire
     * either.
     */
    h_fixture();
    h_sock.nx_tcp_socket_tx_window_advertised = 0;

    h_run_timer(600, 0);
    h_print_ladder("zero window, probes unanswered");

    h_check(h_r.reset == 1,
            "an unanswered zero-window probe never gave up");
    h_check(h_r.reset_at == 127,
            "the zero-window probe budget is not the retransmission budget");

    /* ---- 7. ... and a zero window whose probes are answered ------------ */
    /*
     * What a retry limit on probes must not break: a peer that is still there
     * and still says "not now" is entitled to keep the connection (RFC 1122
     * 4.2.2.17).  It gets probed for the full ten minutes and is never reset.
     */
    h_fixture();
    h_sock.nx_tcp_socket_tx_window_advertised = 0;

    h_run_timer(600, H_PEER_ACKS_PROBES);
    h_print_ladder("zero window, probes answered");

    h_check(h_r.reset == 0,
            "a peer that answered every probe was reset anyway");
    h_check(h_r.sent >= 9,
            "the persist timer stopped probing a live peer");

    /* ---- 8. R2 for a connection request ------------------------------- */
    /*
     * RFC 1122 4.2.3.5 MUST-23: a SYN is retransmitted for at least 3 minutes.
     * The data budget of six gives 127 s, which meets the same section's R2 for
     * data and not MUST-23, so NX_TCP_SYN_MAXIMUM_RETRIES is its own number and
     * NX_TCP_SYN_RETRY_SHIFT_MAX caps the interval so the extra retry is spent
     * at the ceiling rather than doubling past it.
     *
     * Seven retries with the shift capped at 6 is 1, 2, 4, 8, 16, 32, 64 and 64
     * seconds of waiting, seven datagrams at +1, +3, +7, +15, +31, +63 and
     * +127, and the expiry of that last 64 s interval, at 191 s, is the reset.
     * Without the cap the seventh interval is 128 seconds and the reset lands on
     * 255.
     */
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

    printf("%lu checks, %lu failures, %s\n",
           h_checks, h_failures, (h_failures == 0UL) ? "PASS" : "FAIL");

    return (h_failures == 0UL) ? 0 : 1;
}
