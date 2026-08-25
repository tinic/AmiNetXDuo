/*
 * AmiNetXDuo, RFC 1122 4.2.3.4 (MUST-38) sender silly-window avoidance: what
 * this stack puts on the wire when the peer hands its window back in slivers.
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

/* What went on the wire, and how often a blocked sender was woken. */
static UINT h_datagrams;
static UINT h_resumes;

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
    h_resumes++;
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

/* The receiver-side cases need a real packet to build an ACK in; the
   sender-side ones must keep seeing NX_NO_PACKET. */
static int       h_alloc_ok;
static NX_PACKET h_ack_pkt;
static UCHAR     h_ack_buf[256];
static ULONG     h_last_window;
static UINT      h_acks;

UINT _nx_packet_allocate(NX_PACKET_POOL *pool_ptr, NX_PACKET **packet_ptr,
                         ULONG packet_type, ULONG wait_option)
{
    (void)pool_ptr; (void)packet_type; (void)wait_option;

    if (!h_alloc_ok)
        return NX_NO_PACKET;

    memset(&h_ack_pkt, 0, sizeof(h_ack_pkt));
    memset(h_ack_buf, 0, sizeof(h_ack_buf));

    h_ack_pkt.nx_packet_data_start  = h_ack_buf;
    h_ack_pkt.nx_packet_data_end    = h_ack_buf + sizeof(h_ack_buf);
    h_ack_pkt.nx_packet_prepend_ptr = h_ack_buf + 64;
    h_ack_pkt.nx_packet_append_ptr  = h_ack_buf + 64;

    *packet_ptr = &h_ack_pkt;
    return NX_SUCCESS;
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

VOID _nx_ip_packet_send(NX_IP *ip_ptr, NX_PACKET *packet_ptr,
                        ULONG destination_ip, ULONG type_of_service,
                        ULONG time_to_live, ULONG protocol, ULONG fragment,
                        ULONG next_hop_address)
{
    (void)ip_ptr; (void)destination_ip; (void)type_of_service;
    (void)time_to_live; (void)protocol; (void)fragment; (void)next_hop_address;

    h_datagrams++;

    {
    NX_TCP_HEADER *hdr = (NX_TCP_HEADER *)packet_ptr -> nx_packet_prepend_ptr;
    ULONG          word_3 = hdr -> nx_tcp_header_word_3;

        NX_CHANGE_ULONG_ENDIAN(word_3);
        h_last_window = word_3 & 0xFFFFUL;
        h_acks++;
    }

    packet_ptr -> nx_packet_queue_next = (NX_PACKET *)NX_DRIVER_TX_DONE;
}

VOID _nx_tcp_packet_send_ack(NX_TCP_SOCKET *socket_ptr, ULONG tx_sequence)
{
    (void)socket_ptr;
    (void)tx_sequence;
}

VOID _nx_tcp_packet_send_probe(NX_TCP_SOCKET *socket_ptr, ULONG tx_sequence, UCHAR data)
{
    (void)socket_ptr; (void)tx_sequence; (void)data;
    h_datagrams++;
}

VOID _nx_tcp_socket_connection_reset(NX_TCP_SOCKET *socket_ptr)
{
    (void)socket_ptr;
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

#define H_MSS           1460UL
#define H_BUF           2048
#define H_PACKETS       12
#define H_ISN           0x10000000UL
#define H_ISN_RX        0x20000000UL

/* What the peer offers on its SYN, and therefore Max(SND.WND) until it offers
   more.  Half of it, 32767, is the rule (3) threshold on this fixture. */
#define H_PEER_WINDOW   65535UL

static NX_IP          h_ip;
static NX_INTERFACE   h_iface;
static NX_TCP_SOCKET  h_sock;
static TX_THREAD      h_waiter;

static NX_PACKET      h_pkt[H_PACKETS];
static UCHAR          h_pkt_buf[H_PACKETS][H_BUF];
static UINT           h_pkt_next;

static void h_fixture(void)
{
    memset(&h_ip, 0, sizeof(h_ip));
    memset(&h_iface, 0, sizeof(h_iface));
    memset(&h_waiter, 0, sizeof(h_waiter));

    h_now       = 1000;
    h_datagrams = 0;
    h_resumes   = 0;
    h_pkt_next  = 0;

    h_iface.nx_interface_ip_address = 0xC0A80102UL;

    _nx_tcp_socket_create(&h_ip, &h_sock, "host sws", NX_IP_NORMAL,
                          NX_FRAGMENT_OKAY, 0x80, 8192, NX_NULL, NX_NULL);

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

    h_sock.nx_tcp_socket_tx_window_advertised     = H_PEER_WINDOW;
    h_sock.nx_tcp_socket_tx_window_advertised_max = H_PEER_WINDOW;
    h_sock.nx_tcp_socket_tx_window_congestion     = H_PEER_WINDOW;
    h_sock.nx_tcp_socket_tx_slow_start_threshold  = H_PEER_WINDOW;
    h_sock.nx_tcp_socket_tx_outstanding_bytes     = 0;
    h_sock.nx_tcp_socket_tx_sequence              = H_ISN;
    h_sock.nx_tcp_socket_rx_sequence              = H_ISN_RX;
}

/* One write of exactly `bytes`, non-blocking, straight into the real send
   path.  Non-blocking so a refusal comes back as a return code rather than as
   a suspension the shim would have to model. */
static UINT h_write(ULONG bytes)
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
    p -> nx_packet_append_ptr  = p -> nx_packet_prepend_ptr + bytes;
    p -> nx_packet_length      = bytes;

    h_pkt_next++;

    return _nx_tcp_socket_send_internal(&h_sock, p, 0);
}

static void h_in_flight(ULONG flight, ULONG usable)
{
    h_sock.nx_tcp_socket_tx_outstanding_bytes = flight;
    h_sock.nx_tcp_socket_tx_window_advertised = flight + usable;
    h_sock.nx_tcp_socket_tx_window_congestion = H_PEER_WINDOW;
}

static void a_open_window_sends(void)
{
    UINT status;

    h_fixture();

    status = h_write(512);
    h_check_eq(status, NX_SUCCESS, "a first write into an open window failed");
    h_check_eq(h_datagrams, 1, "a first write into an open window sent nothing");

    status = h_write(H_MSS);
    h_check_eq(status, NX_SUCCESS,
               "a full-sized write with 64 KB of window left was refused");
    h_check_eq(h_datagrams, 2,
               "a full-sized write with 64 KB of window left sent nothing");

    printf("  open window         2 writes, %u datagram(s), window "
           "%lu bytes\n", (unsigned int)h_datagrams,
           (unsigned long)h_sock.nx_tcp_socket_tx_window_advertised);
}

static void b_sliver_with_flight_holds(void)
{
    UINT status;

    h_fixture();
    h_in_flight(512, 200);

    status = h_write(100);

    h_check_eq(h_datagrams, 0,
               "a 100-byte write went out into 200 bytes of usable window "
               "with 512 bytes still in flight (no sender SWS avoidance)");
    h_check_eq(status, NX_WINDOW_OVERFLOW,
               "a send the window rule refused did not report the window");

    h_check(h_sock.nx_tcp_socket_zero_window_probe_has_data == NX_FALSE,
            "a non-zero window armed the zero-window persist probe");

    printf("  sliver, in flight   %u datagram(s), status %u\n",
           (unsigned int)h_datagrams, (unsigned int)status);
}

static void c_small_write_is_not_delayed(void)
{
    UINT status;

    h_fixture();
    h_in_flight(512, H_PEER_WINDOW - 512);

    status = h_write(1);

    h_check_eq(status, NX_SUCCESS,
               "a one-byte write with data in flight was refused (Nagle)");
    h_check_eq(h_datagrams, 1,
               "a one-byte write with data in flight was withheld (Nagle)");

    printf("  1 byte, flight 512  %u datagram(s)\n",
           (unsigned int)h_datagrams);
}

static void d_nothing_in_flight_always_sends(void)
{
    UINT status;

    h_fixture();
    h_in_flight(0, 200);

    status = h_write(200);

    h_check_eq(status, NX_SUCCESS,
               "a write into a small window with nothing in flight was "
               "refused, which is a stall with nothing to end it");
    h_check_eq(h_datagrams, 1,
               "a write into a small window with nothing in flight sent "
               "nothing, which is a stall with nothing to end it");

    printf("  sliver, no flight   %u datagram(s)\n",
           (unsigned int)h_datagrams);
}

static void e_half_the_max_window_releases(void)
{
    UINT status;

    h_fixture();
    h_sock.nx_tcp_socket_tx_window_advertised_max = 2400;
    h_in_flight(512, 1200);

    status = h_write(1200);

    h_check_eq(status, NX_SUCCESS,
               "1200 usable bytes of a 2400-byte peer window was refused");
    h_check_eq(h_datagrams, 1,
               "1200 usable bytes of a 2400-byte peer window sent nothing");

    /* The same 1200 bytes, on a peer that has offered 64 KB before now: this
       one IS dribbling, and 1200 is neither a full segment nor half of what it
       has shown it can take. */
    h_fixture();
    h_in_flight(512, 1200);

    status = h_write(1200);

    h_check_eq(h_datagrams, 0,
               "1200 usable bytes of a 65535-byte peer window went out");
    h_check_eq(status, NX_WINDOW_OVERFLOW,
               "a send the window rule refused did not report the window");

    printf("  1200 usable         sent under a 2400-byte peak, held under "
           "65535\n");
}

static void f_zero_window_still_persists(void)
{
    UINT status;

    h_fixture();
    h_sock.nx_tcp_socket_tx_outstanding_bytes = 512;
    h_sock.nx_tcp_socket_tx_window_advertised = 0;
    h_sock.nx_tcp_socket_tx_window_congestion = H_PEER_WINDOW;

    status = h_write(100);

    h_check_eq(status, NX_WINDOW_OVERFLOW,
               "a send into a zero window did not report the window");
    h_check_eq(h_datagrams, 0, "a send into a zero window put a segment out");
    h_check(h_sock.nx_tcp_socket_zero_window_probe_has_data == NX_TRUE,
            "a zero window did not arm the persist probe");

    printf("  zero window         persist probe armed\n");
}

static void g_congestion_window_is_not_a_sliver(void)
{
    UINT status;

    h_fixture();
    h_sock.nx_tcp_socket_tx_outstanding_bytes = 512;
    h_sock.nx_tcp_socket_tx_window_advertised = H_PEER_WINDOW;   /* peer: wide */
    h_sock.nx_tcp_socket_tx_window_congestion = 512 + 200;       /* cwnd: 200  */

    status = h_write(200);

    h_check_eq(status, NX_SUCCESS,
               "a congestion-limited segment was refused as a silly window");
    h_check_eq(h_datagrams, 1,
               "a congestion-limited segment was withheld as a silly window");

    printf("  cwnd sliver         %u datagram(s), peer window %lu\n",
           (unsigned int)h_datagrams,
           (unsigned long)h_sock.nx_tcp_socket_tx_window_advertised);
}

static void i_a_blocked_sender_is_woken_once(void)
{
    h_fixture();
    h_sock.nx_tcp_socket_transmit_suspension_list = &h_waiter;
    h_sock.nx_tcp_socket_transmit_suspended_count = 1;
    h_in_flight(512, 200);

    _nx_tcp_socket_state_transmit_check(&h_sock);
    h_check_eq(h_resumes, 0,
               "a sliver of window woke a sender the send path would refuse");

    /* The acknowledgment that opens the window past a full segment is the one
       worth waking for. */
    h_in_flight(512, H_MSS);
    _nx_tcp_socket_state_transmit_check(&h_sock);
    h_check_eq(h_resumes, 1,
               "a full segment of window did not wake the blocked sender");

    printf("  blocked sender      %u wake-up(s) across two acknowledgments\n",
           (unsigned int)h_resumes);
}

static void j_peak_window_is_remembered(void)
{
    NX_TCP_HEADER hdr;

    h_fixture();
    h_sock.nx_tcp_socket_tx_window_advertised     = 0;
    h_sock.nx_tcp_socket_tx_window_advertised_max = 0;
    h_sock.nx_tcp_socket_tx_sequence              = H_ISN;

    memset(&hdr, 0, sizeof(hdr));
    hdr.nx_tcp_header_word_0         = (80UL << NX_SHIFT_BY_16) | 40000UL;
    hdr.nx_tcp_sequence_number       = h_sock.nx_tcp_socket_rx_sequence;
    hdr.nx_tcp_acknowledgment_number = H_ISN;
    hdr.nx_tcp_header_word_3         = NX_TCP_HEADER_SIZE | NX_TCP_ACK_BIT |
                                       H_PEER_WINDOW;
    (VOID)_nx_tcp_socket_state_ack_check(&h_sock, &hdr);

    h_check_eq(h_sock.nx_tcp_socket_tx_window_advertised_max, H_PEER_WINDOW,
               "the first advertised window was not recorded as the peak");

    hdr.nx_tcp_header_word_3 = NX_TCP_HEADER_SIZE | NX_TCP_ACK_BIT | 300UL;
    (VOID)_nx_tcp_socket_state_ack_check(&h_sock, &hdr);

    h_check_eq(h_sock.nx_tcp_socket_tx_window_advertised, 300,
               "the current advertised window did not follow the peer down");
    h_check_eq(h_sock.nx_tcp_socket_tx_window_advertised_max, H_PEER_WINDOW,
               "the peak advertised window followed the peer down, which "
               "makes every sliver look like half of the window");

    printf("  peak window         %lu advertised, %lu remembered\n",
           (unsigned long)h_sock.nx_tcp_socket_tx_window_advertised,
           (unsigned long)h_sock.nx_tcp_socket_tx_window_advertised_max);
}

/* One pure ACK out of the real control path, with rx_window_current at
   `current` and the socket's buffer at `dflt`.  Answers the window that
   reached the wire. */
static ULONG h_advertise(ULONG current, ULONG dflt)
{
    h_fixture();

    h_sock.nx_tcp_socket_rx_window_default = dflt;
    h_sock.nx_tcp_socket_rx_window_current = current;
    h_sock.nx_tcp_socket_rx_window_last_sent = current;

    h_last_window = 0xFFFFFFFFUL;
    h_acks        = 0;
    h_alloc_ok    = 1;

    _nx_tcp_packet_send_control(&h_sock, NX_TCP_ACK_BIT,
                                h_sock.nx_tcp_socket_tx_sequence,
                                h_sock.nx_tcp_socket_rx_sequence,
                                0, 0, NX_NULL, 0, NX_NULL);

    h_alloc_ok = 0;

    h_check(h_acks == 1, "the control path sent no acknowledgment");

    return h_last_window;
}

static void k_a_runt_window_is_advertised_as_zero(void)
{
ULONG w;

    w = h_advertise(68UL, 8192UL);
    h_check(w == 0UL, "68 bytes of window was advertised rather than zero");

    w = h_advertise(104UL, 8192UL);
    h_check(w == 0UL, "104 bytes of window was advertised rather than zero");

    w = h_advertise(H_MSS - 1UL, 8192UL);
    h_check(w == 0UL, "one byte short of an MSS was advertised rather than "
                      "zero");
}

/* And the rule stops exactly at one MSS: a window a sender can fill is never
   suppressed. */
static void l_a_full_segment_is_advertised(void)
{
ULONG w;

    w = h_advertise(H_MSS, 8192UL);
    h_check(w == H_MSS, "a window of exactly one MSS was not advertised");

    w = h_advertise(8192UL, 8192UL);
    h_check(w == 8192UL, "a full buffer was not advertised");

    w = h_advertise(H_MSS * 2UL, 8192UL);
    h_check(w == H_MSS * 2UL, "two segments of window were not advertised");
}

static void m_a_buffer_below_one_mss_still_opens(void)
{
ULONG w;

    /* Half of a 1000-byte buffer is 500, so 600 is above the floor. */
    w = h_advertise(600UL, 1000UL);
    h_check(w == 600UL, "a small-buffer socket advertised zero and could "
                        "never reopen");

    /* Below half of it, the rule still applies. */
    w = h_advertise(100UL, 1000UL);
    h_check(w == 0UL, "a runt below half a small buffer was still advertised");
}

/* A window that is genuinely zero is still zero, and is not confused with one
   the rule closed. */
static void n_zero_stays_zero(void)
{
    h_check(h_advertise(0UL, 8192UL) == 0UL, "a zero window did not stay zero");
}

int main(void)
{
    _nx_tcp_fast_timer_rate     = (NX_IP_PERIODIC_RATE + (NX_TCP_FAST_TIMER_RATE - 1)) / NX_TCP_FAST_TIMER_RATE;
    _nx_tcp_ack_timer_rate      = (NX_IP_PERIODIC_RATE + (NX_TCP_ACK_TIMER_RATE - 1)) / NX_TCP_ACK_TIMER_RATE;
    _nx_tcp_transmit_timer_rate = (NX_IP_PERIODIC_RATE + (NX_TCP_TRANSMIT_TIMER_RATE - 1)) / NX_TCP_TRANSMIT_TIMER_RATE;

    printf("RFC 1122 4.2.3.4 sender silly-window avoidance, against the real "
           "send path\n");
    printf("  MSS %lu, peer peak window %lu, rule (3) threshold %lu\n",
           (unsigned long)H_MSS, (unsigned long)H_PEER_WINDOW,
           (unsigned long)(H_PEER_WINDOW >> 1));

    a_open_window_sends();
    b_sliver_with_flight_holds();
    c_small_write_is_not_delayed();
    d_nothing_in_flight_always_sends();
    e_half_the_max_window_releases();
    f_zero_window_still_persists();
    g_congestion_window_is_not_a_sliver();
    i_a_blocked_sender_is_woken_once();
    j_peak_window_is_remembered();

    printf("RFC 1122 4.2.3.3 receiver silly-window avoidance, against the real "
           "control path\n");
    k_a_runt_window_is_advertised_as_zero();
    l_a_full_segment_is_advertised();
    m_a_buffer_below_one_mss_still_opens();
    n_zero_stays_zero();

    printf("%lu checks, %lu failures, %s\n",
           h_checks, h_failures, (h_failures == 0UL) ? "PASS" : "FAIL");

    return (h_failures == 0UL) ? 0 : 1;
}
