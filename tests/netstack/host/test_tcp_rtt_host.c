/*
 * AmiNetXDuo -- the RFC 6298 round-trip time estimator, as arithmetic.
 *
 * Sections 2.2 and 2.3 are four lines of integer maths, and getting them
 * wrong is quiet: the connection still works, the timeout is merely the wrong
 * length, and no capture shows which of the two weighted averages drifted.
 * The classic way to get it wrong is order -- section 2.3 computes RTTVAR
 * against the SRTT this sample has NOT yet moved, and updating SRTT first
 * gives an answer that is close enough to look right and is not.  So the
 * expected values below are worked out by hand in the comments, and one case
 * exists purely to separate the two orders.
 *
 * Real, compiled from third_party/netxduo/common/src into this binary:
 * nx_tcp_socket_rtt_sample.c, and around it the whole path that feeds it --
 * nx_tcp_socket_create.c, nx_tcp_socket_send_internal.c,
 * nx_tcp_socket_state_ack_check.c and nx_tcp_socket_retransmit.c -- so the
 * second half of this file asserts on the wiring rather than on the formula:
 * which segment gets timed, which acknowledgment ends the measurement, and
 * which one is thrown away because Karn's algorithm says it is ambiguous.
 *
 * Stubbed: everything that would touch a driver, a packet pool or another
 * thread, exactly as test_tcp_retries_host.c stubs them, plus the clock --
 * tx_time_get() reads a variable this file sets, so a round trip of any
 * length costs nothing and is exact.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nx_api.h"
#include "nx_tcp.h"
#include "nx_packet.h"

#include <stdio.h>
#include <string.h>

#ifndef NX_ENABLE_TCP_RTT_ESTIMATOR
#error "this test is about NX_ENABLE_TCP_RTT_ESTIMATOR and needs it defined"
#endif


/* ---------------------------------------------------------- the clock ----- */

static ULONG h_now;

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

static void h_check_eq(ULONG got, ULONG want, const char *what)
{
    h_checks++;

    if (got != want)
    {
        h_failures++;
        printf("FAIL %s: wanted %lu, got %lu\n", what,
               (unsigned long)want, (unsigned long)got);
    }
}


/* --------------------------------------------------------------- stubs ---- */

ULONG _nx_tcp_fast_timer_rate;
ULONG _nx_tcp_ack_timer_rate;
ULONG _nx_tcp_transmit_timer_rate;

TX_THREAD *_tx_thread_current_ptr;

static UINT h_datagrams;
static UINT h_acks_sent;

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

    h_datagrams++;

    /*lint -e{923} suppress cast of ULONG to pointer.  */
    packet_ptr -> nx_packet_queue_next = (NX_PACKET *)NX_DRIVER_TX_DONE;
}

VOID _nx_tcp_packet_send_probe(NX_TCP_SOCKET *socket_ptr, ULONG tx_sequence,
                               UCHAR data)
{
    (void)socket_ptr; (void)tx_sequence; (void)data;
    h_datagrams++;
}

VOID _nx_tcp_packet_send_ack(NX_TCP_SOCKET *socket_ptr, ULONG tx_sequence)
{
    (void)socket_ptr; (void)tx_sequence;
    h_acks_sent++;
}

VOID _nx_tcp_socket_transmit_queue_flush(NX_TCP_SOCKET *socket_ptr)
{
    socket_ptr -> nx_tcp_socket_transmit_sent_head  = NX_NULL;
    socket_ptr -> nx_tcp_socket_transmit_sent_tail  = NX_NULL;
    socket_ptr -> nx_tcp_socket_transmit_sent_count = 0;
}

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

#define H_SEG_BYTES     512
#define H_BUF           1024
#define H_PACKETS       8

static NX_IP          h_ip;
static NX_INTERFACE   h_iface;
static NX_TCP_SOCKET  h_sock;

static NX_PACKET      h_pkt[H_PACKETS];
static UCHAR          h_pkt_buf[H_PACKETS][H_BUF];
static UINT           h_pkt_next;

#define H_ISN           0x10000000UL

static void h_fixture(void)
{
    memset(&h_ip, 0, sizeof(h_ip));
    memset(&h_iface, 0, sizeof(h_iface));

    h_now       = 1000;         /* not zero, so a stale start time shows up  */
    h_datagrams = 0;
    h_acks_sent = 0;
    h_pkt_next  = 0;

    h_iface.nx_interface_ip_address = 0xC0A80102UL;

    _nx_tcp_socket_create(&h_ip, &h_sock, "host rtt", NX_IP_NORMAL,
                          NX_FRAGMENT_OKAY, 0x80, 8192, NX_NULL, NX_NULL);

    h_sock.nx_tcp_socket_bound_next   = &h_sock;
    h_sock.nx_tcp_socket_client_type  = NX_TRUE;
    h_sock.nx_tcp_socket_state        = NX_TCP_ESTABLISHED;
    h_sock.nx_tcp_socket_port         = 40000;
    h_sock.nx_tcp_socket_connect_port = 80;
    h_sock.nx_tcp_socket_connect_ip.nxd_ip_version   = NX_IP_VERSION_V4;
    h_sock.nx_tcp_socket_connect_ip.nxd_ip_address.v4 = 0xC0A80101UL;
    h_sock.nx_tcp_socket_connect_interface = &h_iface;
    h_sock.nx_tcp_socket_connect_mss  = 1460;
    h_sock.nx_tcp_socket_connect_mss2 = 1460 * 1460;

    h_sock.nx_tcp_socket_tx_window_advertised  = 65535;
    h_sock.nx_tcp_socket_tx_window_congestion  = 65535;
    h_sock.nx_tcp_socket_tx_slow_start_threshold = 65535;
    h_sock.nx_tcp_socket_tx_outstanding_bytes  = 0;
    h_sock.nx_tcp_socket_tx_sequence           = H_ISN;
    h_sock.nx_tcp_socket_rx_sequence           = 0x20000000UL;
}

/* One application send of H_SEG_BYTES, through the real output path. */
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
                                 20 +                   /* IPv4 header       */
                                 sizeof(NX_TCP_HEADER);
    p -> nx_packet_append_ptr  = p -> nx_packet_prepend_ptr + H_SEG_BYTES;
    p -> nx_packet_length      = H_SEG_BYTES;

    h_pkt_next++;

    return _nx_tcp_socket_send_internal(&h_sock, p, 0);
}

/* An acknowledgment from the peer, as _nx_tcp_socket_packet_process would hand
   it over: host byte order, already checked against the receive window. */
static UINT h_ack(ULONG ack_number)
{
    NX_TCP_HEADER hdr;

    memset(&hdr, 0, sizeof(hdr));
    hdr.nx_tcp_header_word_0        = (80UL << NX_SHIFT_BY_16) | 40000UL;
    hdr.nx_tcp_sequence_number      = h_sock.nx_tcp_socket_rx_sequence;
    hdr.nx_tcp_acknowledgment_number = ack_number;
    hdr.nx_tcp_header_word_3        = NX_TCP_HEADER_SIZE | NX_TCP_ACK_BIT | 65535UL;

    return _nx_tcp_socket_state_ack_check(&h_sock, &hdr);
}


/* --------------------------------------------------------- the formula ---- */

/*
 * Every expected value below is in ticks.  NX_IP_PERIODIC_RATE is 50 in the
 * shipping nx_user.h, so a tick is 20 ms, the section 2.4 floor is 50 ticks
 * and the section 2.5 ceiling is 3000.  The state is scaled: smoothed by
 * eight, variation by four.
 */

static void h_sample_state(ULONG smoothed, ULONG variation)
{
    h_sock.nx_tcp_socket_rtt_smoothed  = smoothed;
    h_sock.nx_tcp_socket_rtt_variation = variation;
}

static void formula_cases(void)
{
    printf("The section 2.2 and 2.3 update\n");

    /* ---- the first measurement, section 2.2 --------------------------- */
    /*
     * R = 40 ticks (800 ms).  SRTT = R, RTTVAR = R/2, so scaled that is
     * 40 << 3 = 320 and 40 << 1 = 80.  RTO = SRTT + max(G, 4 RTTVAR)
     *         = 40 + 80 = 120 ticks, which is 2.4 seconds and above the floor.
     */
    h_fixture();
    _nx_tcp_socket_rtt_sample(&h_sock, 40);

    h_check_eq(h_sock.nx_tcp_socket_rtt_smoothed, 320, "first sample SRTT");
    h_check_eq(h_sock.nx_tcp_socket_rtt_variation, 80, "first sample RTTVAR");
    h_check_eq(h_sock.nx_tcp_socket_timeout_rate, 120, "first sample RTO");

    /* ---- and the ones after it, section 2.3 --------------------------- */
    /*
     * The same 40 again.  error is 40 - 320/8 = 0, so SRTT does not move and
     * RTTVAR decays by a quarter: 80 - 20 + 0 = 60.  RTO = 40 + 60 = 100.
     * Then 60 - 15 = 45 and RTO = 85; then 45 - 11 = 34 and RTO = 74.
     */
    _nx_tcp_socket_rtt_sample(&h_sock, 40);
    h_check_eq(h_sock.nx_tcp_socket_rtt_smoothed, 320, "steady SRTT holds");
    h_check_eq(h_sock.nx_tcp_socket_rtt_variation, 60, "steady RTTVAR decays");
    h_check_eq(h_sock.nx_tcp_socket_timeout_rate, 100, "steady RTO");

    _nx_tcp_socket_rtt_sample(&h_sock, 40);
    h_check_eq(h_sock.nx_tcp_socket_rtt_variation, 45, "RTTVAR decays again");
    h_check_eq(h_sock.nx_tcp_socket_timeout_rate, 85, "RTO follows it down");

    _nx_tcp_socket_rtt_sample(&h_sock, 40);
    h_check_eq(h_sock.nx_tcp_socket_rtt_variation, 34, "RTTVAR decays again");
    h_check_eq(h_sock.nx_tcp_socket_timeout_rate, 74, "RTO follows it down");

    /* ---- the order of the two updates --------------------------------- */
    /*
     * SRTT 40 (scaled 320), RTTVAR 20 (scaled 80), and a sample of 200 that
     * is nothing like either.
     *
     *   right  error = 200 - 40 = 160, taken against the OLD SRTT
     *          RTTVAR: 80 - 20 + 160 = 220
     *          SRTT:   320 + 160 = 480, so 60
     *          RTO = 60 + 220 = 280
     *
     *   wrong  SRTT first: 480, so 60
     *          then |60 - 200| = 140
     *          RTTVAR: 80 - 20 + 140 = 200
     *          RTO = 60 + 200 = 260
     *
     * Twenty ticks apart, which is 400 ms of retransmission timeout and is
     * the whole reason this case exists.
     */
    h_fixture();
    h_sample_state(320, 80);
    _nx_tcp_socket_rtt_sample(&h_sock, 200);

    h_check_eq(h_sock.nx_tcp_socket_rtt_variation, 220,
               "RTTVAR was not computed against the previous SRTT");
    h_check_eq(h_sock.nx_tcp_socket_rtt_smoothed, 480, "SRTT after the jump");
    h_check_eq(h_sock.nx_tcp_socket_timeout_rate, 280,
               "RTO after a sample far from the estimate");

    /* ---- a sample below the estimate --------------------------------- */
    /*
     * The error is negative and only its magnitude reaches RTTVAR.  From
     * SRTT 40 (320) and RTTVAR 20 (80), a sample of 8:
     *   error = 8 - 40 = -32
     *   RTTVAR: 80 - 20 + 32 = 92
     *   SRTT:   320 - 32 = 288, so 36
     *   RTO = 36 + 92 = 128
     */
    h_fixture();
    h_sample_state(320, 80);
    _nx_tcp_socket_rtt_sample(&h_sock, 8);

    h_check_eq(h_sock.nx_tcp_socket_rtt_variation, 92,
               "a sample below the estimate must still widen RTTVAR");
    h_check_eq(h_sock.nx_tcp_socket_rtt_smoothed, 288, "SRTT after a short sample");
    h_check_eq(h_sock.nx_tcp_socket_timeout_rate, 128, "RTO after a short sample");

    /* ---- section 2.4's floor ----------------------------------------- */
    /*
     * One tick, the shortest the clock can express.  SRTT 8, RTTVAR 2, and
     * RTO would be 1 + 2 = 3 ticks, 60 ms.  The floor makes it a second.
     * The estimate itself is NOT floored: the next sample has to be able to
     * move it.
     */
    h_fixture();
    _nx_tcp_socket_rtt_sample(&h_sock, 1);

    h_check_eq(h_sock.nx_tcp_socket_rtt_smoothed, 8, "a short path still estimates");
    h_check_eq(h_sock.nx_tcp_socket_rtt_variation, 2, "a short path still estimates");
    h_check_eq(h_sock.nx_tcp_socket_timeout_rate, NX_TCP_RTO_MINIMUM,
               "the section 2.4 floor did not hold");

    /* A measurement of nothing is one tick of clock granularity, not zero --
       zero would make the first sample leave SRTT at 0 and every later one
       re-initialise. */
    h_fixture();
    _nx_tcp_socket_rtt_sample(&h_sock, 0);

    h_check_eq(h_sock.nx_tcp_socket_rtt_smoothed, 8,
               "a round trip inside one tick is one tick, not none");

    /* ---- max(G, K RTTVAR) -------------------------------------------- */
    /*
     * G is one tick.  With no variation at all the timeout must still be a
     * tick longer than the estimate, or a path whose delay never varies would
     * arm a timeout for the exact instant the acknowledgment is due.
     */
    h_fixture();
    h_sample_state(800, 0);
    _nx_tcp_socket_rtt_sample(&h_sock, 100);

    h_check_eq(h_sock.nx_tcp_socket_rtt_variation, 0, "RTTVAR stays at nothing");
    h_check_eq(h_sock.nx_tcp_socket_timeout_rate, 101,
               "the granularity term is missing from RTO");

    /* ---- section 2.5's ceiling --------------------------------------- */
    /*
     * A first sample at the ceiling itself gives RTO = 3 R, which is well
     * past it.  And a measurement longer than any timeout may be is clamped
     * on the way in rather than folded in at full size.
     */
    h_fixture();
    _nx_tcp_socket_rtt_sample(&h_sock, NX_TCP_RTO_MAXIMUM);
    h_check_eq(h_sock.nx_tcp_socket_timeout_rate, NX_TCP_RTO_MAXIMUM,
               "the section 2.5 ceiling did not hold");

    h_fixture();
    _nx_tcp_socket_rtt_sample(&h_sock, NX_TCP_RTO_MAXIMUM * 4);
    h_check_eq(h_sock.nx_tcp_socket_rtt_smoothed, NX_TCP_RTO_MAXIMUM << 3,
               "an impossible measurement was folded in at full size");

    /* ---- where a steady path settles --------------------------------- */
    /*
     * Twenty identical samples at 40 ticks.  SRTT never moves, RTTVAR decays
     * towards nothing, and the computed timeout walks down until section
     * 2.4's floor catches it -- so a short path ends up with exactly the one
     * second the stack used to use unconditionally, which is the point: the
     * estimator costs nothing there and earns its keep on a long path.
     */
    h_fixture();
    {
        UINT i;

        for (i = 0; i < 20; i++)
        {
            _nx_tcp_socket_rtt_sample(&h_sock, 40);
        }
    }

    h_check_eq(h_sock.nx_tcp_socket_rtt_smoothed, 320, "a steady path moves SRTT");
    h_check_eq(h_sock.nx_tcp_socket_timeout_rate, NX_TCP_RTO_MINIMUM,
               "a steady short path did not settle on the floor");

    /*
     * And a steady LONG path settles above it.  Twenty samples at 200 ticks
     * (four seconds) leave SRTT at 200 and the timeout above the floor by
     * more than a factor of four -- which a fixed one-second base cannot do
     * and is the case the estimator exists for.
     */
    h_fixture();
    {
        UINT i;

        for (i = 0; i < 20; i++)
        {
            _nx_tcp_socket_rtt_sample(&h_sock, 200);
        }
    }

    h_check_eq(h_sock.nx_tcp_socket_rtt_smoothed, 1600, "a long path moves SRTT");
    h_check(h_sock.nx_tcp_socket_timeout_rate >= 200,
            "a long path settled below its own round trip");
    h_check(h_sock.nx_tcp_socket_timeout_rate > NX_TCP_RTO_MINIMUM,
            "a long path settled on the floor");

    printf("  a steady 4.0 s path settles at   %lu ticks (%lu ms)\n",
           (unsigned long)h_sock.nx_tcp_socket_timeout_rate,
           (unsigned long)(h_sock.nx_tcp_socket_timeout_rate * 1000UL / NX_IP_PERIODIC_RATE));

    /* ---- an application that named its own timeout -------------------- */
    h_fixture();
    (VOID)_nx_tcp_socket_transmit_configure(&h_sock, 20, 7, 4, 1);
    _nx_tcp_socket_rtt_sample(&h_sock, 200);

    h_check_eq(h_sock.nx_tcp_socket_timeout_rate, 7,
               "the estimator wrote over a timeout the application set");

    /* ---- and a socket about to be reused ------------------------------ */
    h_fixture();
    _nx_tcp_socket_rtt_sample(&h_sock, 200);
    h_check(h_sock.nx_tcp_socket_timeout_rate != _nx_tcp_transmit_timer_rate,
            "the sample did not move the timeout at all");

    _nx_tcp_socket_block_cleanup(&h_sock);

    h_check_eq(h_sock.nx_tcp_socket_rtt_smoothed, 0,
               "a closed socket kept the estimate of the path it used");
    h_check_eq(h_sock.nx_tcp_socket_timeout_rate, _nx_tcp_transmit_timer_rate,
               "a closed socket kept the timeout of the path it used");
}


/* ---------------------------------------------------------- the wiring ---- */

static void wiring_cases(void)
{
    UINT status;

    printf("Which segment is timed, and which acknowledgment ends it\n");

    /* ---- a send arms the measurement ---------------------------------- */
    h_fixture();
    h_now = 1000;
    status = h_send();

    h_check_eq(status, NX_SUCCESS, "the send did not go out");
    h_check_eq(h_sock.nx_tcp_socket_rtt_timing, NX_TRUE, "the send armed nothing");
    h_check_eq(h_sock.nx_tcp_socket_rtt_sequence, H_ISN + H_SEG_BYTES,
               "the timed sequence is not the end of the segment");
    h_check_eq(h_sock.nx_tcp_socket_rtt_start, 1000, "the send time was not recorded");

    /* ---- a second send does not re-arm it ----------------------------- */
    /*
     * One segment per window is what an implementation without the timestamp
     * option can do, so the second send leaves the first one being timed.
     */
    h_now = 1010;
    (VOID)h_send();

    h_check_eq(h_sock.nx_tcp_socket_rtt_sequence, H_ISN + H_SEG_BYTES,
               "a later segment took over the measurement");
    h_check_eq(h_sock.nx_tcp_socket_rtt_start, 1000,
               "a later segment moved the start of the measurement");

    /* ---- and the acknowledgment ends it ------------------------------- */
    /*
     * Forty ticks after the first segment left, the peer acknowledges both.
     * That is the 800 ms first measurement from the formula cases, so the
     * timeout must come out at the same 120 ticks.
     */
    h_now = 1040;
    (VOID)h_ack(H_ISN + (2 * H_SEG_BYTES));

    h_check_eq(h_sock.nx_tcp_socket_rtt_timing, NX_FALSE,
               "the acknowledgment did not end the measurement");
    h_check_eq(h_sock.nx_tcp_socket_rtt_smoothed, 320, "the sample was not 40 ticks");
    h_check_eq(h_sock.nx_tcp_socket_timeout_rate, 120, "the timeout was not recomputed");

    /* ---- an acknowledgment short of the timed segment is not a sample -- */
    h_fixture();
    h_now = 2000;
    (VOID)h_send();
    (VOID)h_send();

    /* Retime on the second segment by hand, so a partial acknowledgment can
       fall between the two. */
    h_sock.nx_tcp_socket_rtt_sequence = H_ISN + (2 * H_SEG_BYTES);

    h_now = 2050;
    (VOID)h_ack(H_ISN + H_SEG_BYTES);

    h_check_eq(h_sock.nx_tcp_socket_rtt_timing, NX_TRUE,
               "a partial acknowledgment ended the measurement");
    h_check_eq(h_sock.nx_tcp_socket_rtt_smoothed, 0,
               "a partial acknowledgment produced a sample");

    /* ...and the one that does cover it, later, still does. */
    h_now = 2080;
    (VOID)h_ack(H_ISN + (2 * H_SEG_BYTES));

    h_check_eq(h_sock.nx_tcp_socket_rtt_timing, NX_FALSE,
               "the covering acknowledgment did not end the measurement");
    h_check_eq(h_sock.nx_tcp_socket_rtt_smoothed, 80 << 3,
               "the sample was not measured from the send");

    /* ---- Karn's algorithm, section 3 ---------------------------------- */
    /*
     * The segment is retransmitted before the acknowledgment arrives, so the
     * acknowledgment could be answering either copy.  No sample, and the
     * estimate is left exactly as it was.
     */
    h_fixture();
    h_now = 3000;
    (VOID)h_send();

    h_check_eq(h_sock.nx_tcp_socket_rtt_timing, NX_TRUE, "the send armed nothing");

    h_now = 3050;
    _nx_tcp_socket_retransmit(&h_ip, &h_sock, NX_FALSE);

    h_check_eq(h_sock.nx_tcp_socket_rtt_timing, NX_FALSE,
               "a retransmission left the measurement running");

    h_now = 3060;
    (VOID)h_ack(H_ISN + H_SEG_BYTES);

    h_check_eq(h_sock.nx_tcp_socket_rtt_smoothed, 0,
               "an ambiguous acknowledgment was taken as a sample");
    h_check_eq(h_sock.nx_tcp_socket_timeout_rate, _nx_tcp_transmit_timer_rate,
               "an ambiguous acknowledgment moved the timeout");

    /* ---- but the next new segment measures again ---------------------- */
    /*
     * Karn's algorithm suppresses the sample, it does not switch the
     * estimator off: the segment sent after the recovery has been transmitted
     * once and its acknowledgment is unambiguous.
     */
    h_sock.nx_tcp_socket_timeout_retries = 0;

    h_now = 4000;
    (VOID)h_send();

    h_check_eq(h_sock.nx_tcp_socket_rtt_timing, NX_TRUE,
               "the estimator stopped after one retransmission");
    h_check_eq(h_sock.nx_tcp_socket_rtt_start, 4000, "the new segment was not timed");

    h_now = 4040;
    (VOID)h_ack(h_sock.nx_tcp_socket_tx_sequence);

    h_check_eq(h_sock.nx_tcp_socket_rtt_smoothed, 320,
               "the segment after a retransmission produced no sample");

    /* ---- an acknowledgment of data that was never sent ---------------- */
    /*
     * RFC 793 section 3.9 answers this with an acknowledgment and drops the
     * segment.  It must not be measured either, whatever sequence it names.
     */
    h_fixture();
    h_now = 5000;
    (VOID)h_send();

    h_now = 5100;
    status = h_ack(H_ISN + (10 * H_SEG_BYTES));

    h_check_eq(status, NX_FALSE, "an impossible acknowledgment was accepted");
    h_check_eq(h_sock.nx_tcp_socket_rtt_smoothed, 0,
               "an impossible acknowledgment was taken as a sample");
    h_check_eq(h_sock.nx_tcp_socket_rtt_timing, NX_TRUE,
               "an impossible acknowledgment ended the measurement");
}


/* --------------------------------------------------------------- main ----- */

int main(void)
{
    _nx_tcp_fast_timer_rate     = (NX_IP_PERIODIC_RATE + (NX_TCP_FAST_TIMER_RATE - 1)) / NX_TCP_FAST_TIMER_RATE;
    _nx_tcp_ack_timer_rate      = (NX_IP_PERIODIC_RATE + (NX_TCP_ACK_TIMER_RATE - 1)) / NX_TCP_ACK_TIMER_RATE;
    _nx_tcp_transmit_timer_rate = (NX_IP_PERIODIC_RATE + (NX_TCP_TRANSMIT_TIMER_RATE - 1)) / NX_TCP_TRANSMIT_TIMER_RATE;

    printf("RFC 6298 retransmission timeout estimation\n");
    printf("  tick rate %d Hz, floor %lu ticks, ceiling %lu ticks,"
           " default base %lu ticks\n",
           NX_IP_PERIODIC_RATE,
           (unsigned long)NX_TCP_RTO_MINIMUM,
           (unsigned long)NX_TCP_RTO_MAXIMUM,
           (unsigned long)_nx_tcp_transmit_timer_rate);

    /* The floor and ceiling are what the sections ask for at this tick rate,
       and the arithmetic below is worked out against these numbers. */
    h_check_eq(NX_TCP_RTO_MINIMUM, 50, "the section 2.4 floor is not one second");
    h_check_eq(NX_TCP_RTO_MAXIMUM, 3000, "the section 2.5 ceiling is not sixty seconds");

    formula_cases();
    wiring_cases();

    printf("%lu checks, %lu failures -- %s\n",
           h_checks, h_failures, (h_failures == 0UL) ? "PASS" : "FAIL");

    return (h_failures == 0UL) ? 0 : 1;
}
