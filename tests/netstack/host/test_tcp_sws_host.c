/*
 * AmiNetXDuo, RFC 1122 4.2.3.4 (MUST-38) sender silly-window avoidance: what
 * this stack puts on the wire when the peer hands its window back in slivers.
 *
 * The vendored send path asked one question about the window -- is it non-zero
 * -- and sent whatever fitted.  A peer whose application reads a few hundred
 * bytes at a time reopens a few hundred bytes at a time, and the connection
 * settles into segments that size: forty bytes of header per two hundred of
 * payload, on a machine whose driver costs half a millisecond a frame.  That
 * is the silly window, from the sending side.
 *
 * This cannot be measured on the rig.  Every peer the lab has is Linux, which
 * runs receiver-side SWS avoidance of its own and therefore never offers the
 * increments this rule is about; producing them means writing a peer that
 * misbehaves on purpose, and then the figure measures the peer.  So the rule
 * is asserted here, against the real send path, where a segment either leaves
 * or does not.
 *
 * IT ALSO PINS THE HALF THAT MUST NOT CHANGE, and that half is the reason the
 * test exists rather than the rule: this must not become Nagle.  Nagle
 * withholds a SMALL WRITE while anything is unacknowledged.  The rule here
 * withholds any write when the WINDOW is a sliver, and c_small_write_is_not_
 * delayed asserts that one byte written into an open window with a segment
 * still in flight leaves immediately.  TCP_NODELAY refuses 0 in
 * src/bsdsocket/options.c on the grounds that there is no Nagle to disable,
 * and that answer has to keep being true.
 *
 * Real, compiled from third_party/netxduo/common/src into this binary:
 * nx_tcp_socket_send_internal.c, nx_tcp_socket_state_transmit_check.c,
 * nx_tcp_socket_state_ack_check.c and nx_tcp_socket_create.c -- the send path,
 * the path that decides when a blocked sender is worth waking, and the path
 * that records Max(SND.WND).
 *
 * Stubbed: everything that would touch a driver, a packet pool or another
 * thread, the same shim tests/netstack/host/test_tcp_earlyretx_host.c uses.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nx_api.h"
#include "nx_tcp.h"
#include "nx_packet.h"
#include "nx_ip.h"

#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------- shim ---- */

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

VOID _nx_ip_packet_send(NX_IP *ip_ptr, NX_PACKET *packet_ptr,
                        ULONG destination_ip, ULONG type_of_service,
                        ULONG time_to_live, ULONG protocol, ULONG fragment,
                        ULONG next_hop_address)
{
    (void)ip_ptr; (void)destination_ip; (void)type_of_service;
    (void)time_to_live; (void)protocol; (void)fragment; (void)next_hop_address;

    h_datagrams++;

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

/* ------------------------------------------------------------- fixture ---- */

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

    /* The peer opened with a full window and has not shrunk it yet.  The cases
       below move nx_tcp_socket_tx_window_advertised and leave
       nx_tcp_socket_tx_window_advertised_max alone, which is the whole point
       of keeping the two apart: the sliver being judged is not the number the
       threshold is derived from. */
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

/* Put the socket in the state the rule is about: `flight` bytes unacknowledged
   and a PEER window that leaves exactly `usable` bytes on top of them.  The
   congestion window is left wide open, because the rule is not about it --
   g_congestion_window_is_not_a_sliver is the case that says so. */
static void h_in_flight(ULONG flight, ULONG usable)
{
    h_sock.nx_tcp_socket_tx_outstanding_bytes = flight;
    h_sock.nx_tcp_socket_tx_window_advertised = flight + usable;
    h_sock.nx_tcp_socket_tx_window_congestion = H_PEER_WINDOW;
}

/* --------------------------------------------------------------- cases ---- */

static void a_open_window_sends(void)
{
    UINT status;

    /*
     * The case that is every bulk transfer, and the one the rule must leave
     * alone.  A full window, a segment already in flight, and a full-sized
     * write: rule (1), a maximum-sized segment can be sent.
     */
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

    /*
     * The defect.  512 bytes in flight, the peer has handed back 200 bytes of
     * room, and the application offers 100.  The vendored path asked only
     * whether the window was non-zero and put a 100-byte segment on the wire;
     * the next 200-byte reopening would have got another, and so on for as
     * long as the peer read in that size.
     *
     * 100 rather than 512 on purpose: a write LARGER than the sliver is
     * refused on the old code too, because the fragmenting arm needs a packet
     * from the pool and this shim has none.  A write that FITS separates the
     * two builds.
     */
    h_fixture();
    h_in_flight(512, 200);

    status = h_write(100);

    h_check_eq(h_datagrams, 0,
               "a 100-byte write went out into 200 bytes of usable window "
               "with 512 bytes still in flight (no sender SWS avoidance)");
    h_check_eq(status, NX_WINDOW_OVERFLOW,
               "a send the window rule refused did not report the window");

    /* And it must not have been mistaken for a zero window: the persist timer
       probes a receiver that said zero, and this one said 712.  Arming it here
       would move the retransmission retry limit onto the probe failure count
       and stop it ever being reached (nx_tcp_socket_send_internal.c). */
    h_check(h_sock.nx_tcp_socket_zero_window_probe_has_data == NX_FALSE,
            "a non-zero window armed the zero-window persist probe");

    printf("  sliver, in flight   %u datagram(s), status %u\n",
           (unsigned int)h_datagrams, (unsigned int)status);
}

static void c_small_write_is_not_delayed(void)
{
    UINT status;

    /*
     * NOT NAGLE, asserted.  One byte offered while 512 are unacknowledged, and
     * a window wide open.  Nagle would hold this until the flight is
     * acknowledged; rule (1) is measured against the window and not against
     * the size of the write, so it leaves now.
     *
     * If this ever fails, TCP_NODELAY's refusal of 0 in
     * src/bsdsocket/options.c has become a lie.
     */
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

    /*
     * The clause that makes the rule safe to hold with.  200 bytes of window,
     * nothing outstanding: no acknowledgment is coming to reopen anything, the
     * persist timer declines to arm because the window is not zero, and a
     * stack that held here would wait on a window update the peer has no
     * reason to send.  RFC 1122 4.2.3.4 rule (2), SND.NXT = SND.UNA.
     */
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

    /*
     * Rule (3), Fs = 1/2.  A peer that never advertised more than 2400 bytes
     * is not a silly-window peer at 1200: half of everything it ever offered
     * is as good as this connection gets, and a stack that waited for a full
     * segment would wait forever.  Max(SND.WND) rather than the current
     * window is what makes the two cases here different, because the current
     * window is the same 1200 in both.
     */
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

    /*
     * The path this rule must not have taken over.  A zero window is
     * RFC 1122 4.2.2.17, not 4.2.3.4, and it is answered by arming the persist
     * probe.  Nothing about that changes.
     */
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

    /*
     * THE CASE THAT KEEPS THE RULE OFF THE BULK PATH, and the one that cost
     * measurable throughput before it existed.  RFC 1122 4.2.3.4's U is
     * SND.UNA + SND.WND - SND.NXT: the window the RECEIVER advertised.  A
     * congestion window that happens to leave 200 bytes on top of what is in
     * flight is not a silly window -- it is the ACK clock, and those 200 bytes
     * are the segment that keeps the pipe full.
     *
     * Judging cwnd by this rule held that segment back once per round trip on
     * every bulk transfer, and cost 0.3% (a2065), 0.4% (ariadne) and 2.8%
     * (x-surf-100 Z3) of the write path, n=3 per card on tests/tools/
     * run-iperf.sh.  Not one of those peers ever had a small window.
     */
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
    /*
     * The other half of the rule, and the reason it is a shared function
     * rather than four lines in the send path.  A thread suspended on the
     * transmit list is woken by _nx_tcp_socket_state_transmit_check, which
     * used to ask only whether the window was non-zero -- so every
     * acknowledgment that reopened a byte woke it, the send path refused
     * again, and it suspended again.  On a 68020 each of those is a context
     * switch and a mutex pair for nothing.
     */
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

    /*
     * Max(SND.WND) is a peak and not a sample.  The peer offers 65535, then
     * runs down to 300 as its application falls behind; the threshold rule (3)
     * measures against has to stay 65535/2, because 300/2 would call every
     * sliver acceptable and the rule would do nothing at all.
     */
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

    printf("%lu checks, %lu failures, %s\n",
           h_checks, h_failures, (h_failures == 0UL) ? "PASS" : "FAIL");

    return (h_failures == 0UL) ? 0 : 1;
}
