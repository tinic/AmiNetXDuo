/*
 * AmiNetXDuo, RFC 5827 early retransmit: what recovers a lost segment when the
 * flight is too small to produce three duplicate acknowledgments.
 *
 * Three duplicate acknowledgments are three segments arriving past the hole,
 * so a flight of fewer than four cannot produce them.  Below that the only
 * thing left is the retransmission timeout, and on this port that has a one
 * second floor against a lab round trip of a millisecond.  Section 2 lowers
 * the threshold to one less than the number of outstanding segments while
 * there are fewer than four of them.
 *
 * This cannot be measured on the rig.  A recovery that takes one round trip is
 * indistinguishable, at the guest's 20 ms clock, from a fetch that lost
 * nothing: tests/perf/run-reqresp.sh over eight arms could separate the tail
 * loss probe, which lands in a 200-899 ms band of its own, and could not
 * separate this from noise.  So the threshold is asserted here, against the
 * real acknowledgment path, where one duplicate either retransmits or does
 * not.
 *
 * It also pins the half that must NOT change: at four segments and above the
 * threshold is three, which is every bulk transfer.
 *
 * Real, compiled from third_party/netxduo/common/src into this binary:
 * nx_tcp_socket_state_ack_check.c, nx_tcp_socket_send_internal.c,
 * nx_tcp_socket_retransmit.c and nx_tcp_socket_create.c -- the whole path from
 * "a duplicate acknowledgment arrived" to "a segment went out again".
 *
 * Stubbed: everything that would touch a driver, a packet pool or another
 * thread, the same shim tests/netstack/host/test_tcp_rtt_host.c uses.
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

VOID _nx_ip_packet_send(NX_IP *ip_ptr, NX_PACKET *packet_ptr,
                        ULONG destination_ip, ULONG type_of_service,
                        ULONG time_to_live, ULONG protocol, ULONG fragment,
                        ULONG next_hop_address)
{
    (void)ip_ptr; (void)destination_ip; (void)type_of_service;
    (void)time_to_live; (void)protocol; (void)fragment; (void)next_hop_address;

    h_datagrams++;

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

#define H_SEG_BYTES     512
#define H_BUF           1024
#define H_PACKETS       8
#define H_ISN           0x10000000UL

static NX_IP          h_ip;
static NX_INTERFACE   h_iface;
static NX_TCP_SOCKET  h_sock;

static NX_PACKET      h_pkt[H_PACKETS];
static UCHAR          h_pkt_buf[H_PACKETS][H_BUF];
static UINT           h_pkt_next;

static void h_fixture(void)
{
    memset(&h_ip, 0, sizeof(h_ip));
    memset(&h_iface, 0, sizeof(h_iface));

    h_now       = 1000;
    h_datagrams = 0;
    h_pkt_next  = 0;

    h_iface.nx_interface_ip_address = 0xC0A80102UL;

    _nx_tcp_socket_create(&h_ip, &h_sock, "host early retx", NX_IP_NORMAL,
                          NX_FRAGMENT_OKAY, 0x80, 8192, NX_NULL, NX_NULL);

    h_sock.nx_tcp_socket_bound_next   = &h_sock;
    h_sock.nx_tcp_socket_client_type  = NX_TRUE;
    h_sock.nx_tcp_socket_state        = NX_TCP_ESTABLISHED;
    h_sock.nx_tcp_socket_port         = 40000;
    h_sock.nx_tcp_socket_connect_port = 80;
    h_sock.nx_tcp_socket_connect_ip.nxd_ip_version    = NX_IP_VERSION_V4;
    h_sock.nx_tcp_socket_connect_ip.nxd_ip_address.v4 = 0xC0A80101UL;
    h_sock.nx_tcp_socket_connect_interface = &h_iface;
    h_sock.nx_tcp_socket_connect_mss  = 1460;
    h_sock.nx_tcp_socket_connect_mss2 = 1460 * 1460;

    h_sock.nx_tcp_socket_tx_window_advertised    = 65535;
    h_sock.nx_tcp_socket_tx_window_congestion    = 65535;
    h_sock.nx_tcp_socket_tx_slow_start_threshold = 65535;
    h_sock.nx_tcp_socket_tx_outstanding_bytes    = 0;
    h_sock.nx_tcp_socket_tx_sequence             = H_ISN;
    h_sock.nx_tcp_socket_rx_sequence             = 0x20000000UL;
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
    p -> nx_packet_append_ptr  = p -> nx_packet_prepend_ptr + H_SEG_BYTES;
    p -> nx_packet_length      = H_SEG_BYTES;

    h_pkt_next++;

    return _nx_tcp_socket_send_internal(&h_sock, p, 0);
}

/* One duplicate acknowledgment: it names the front of the transmit queue, so
   it acknowledges nothing new. */
static UINT h_dupack(void)
{
    NX_TCP_HEADER hdr;

    memset(&hdr, 0, sizeof(hdr));
    hdr.nx_tcp_header_word_0         = (80UL << NX_SHIFT_BY_16) | 40000UL;
    hdr.nx_tcp_sequence_number       = h_sock.nx_tcp_socket_rx_sequence;
    hdr.nx_tcp_acknowledgment_number = H_ISN;
    hdr.nx_tcp_header_word_3         = NX_TCP_HEADER_SIZE | NX_TCP_ACK_BIT | 65535UL;

    return _nx_tcp_socket_state_ack_check(&h_sock, &hdr);
}

/* --------------------------------------------------------------- cases ---- */

static void small_flight(void)
{
    UINT before;

    /*
     * Two segments out and the first of them lost.  The peer holds the second
     * and answers it with one duplicate acknowledgment, and there is no third
     * segment to produce a second one: this is the whole of what a request and
     * its response can ever elicit.
     */
    h_fixture();
    (VOID)h_send();
    (VOID)h_send();

    h_check_eq(h_sock.nx_tcp_socket_transmit_sent_count, 2,
               "the fixture did not leave two segments outstanding");

    before = h_datagrams;
    (VOID)h_dupack();

    printf("  flight of two       %u datagram(s) after one duplicate ack\n",
           (unsigned int)(h_datagrams - before));

#ifdef NX_ENABLE_TCP_EARLY_RETRANSMIT
    h_check(h_datagrams > before,
            "one duplicate ack on a flight of two did not retransmit");
    h_check(h_sock.nx_tcp_socket_fast_recovery == NX_TRUE,
            "the early retransmit did not enter fast recovery");
#else
    h_check(h_datagrams == before,
            "a flight of two retransmitted with early retransmit off");
#endif

    /*
     * Three out, two duplicates.  The threshold is two here, so the first
     * duplicate must not be enough: an early retransmit that fires one
     * acknowledgment sooner than the flight justifies is a retransmission of
     * something merely reordered.
     */
    h_fixture();
    (VOID)h_send();
    (VOID)h_send();
    (VOID)h_send();

    h_check_eq(h_sock.nx_tcp_socket_transmit_sent_count, 3,
               "the fixture did not leave three segments outstanding");

    before = h_datagrams;
    (VOID)h_dupack();

#ifdef NX_ENABLE_TCP_EARLY_RETRANSMIT
    h_check(h_datagrams == before,
            "one duplicate ack on a flight of three retransmitted too early");

    (VOID)h_dupack();
    h_check(h_datagrams > before,
            "two duplicate acks on a flight of three did not retransmit");
#endif
}

static void large_flight(void)
{
    UINT before;

    /*
     * Four segments can produce three duplicates, so the threshold is three
     * and nothing here has moved.  This is every bulk transfer, and it is the
     * half of RFC 5827 that must stay exactly as it was.
     */
    h_fixture();
    (VOID)h_send();
    (VOID)h_send();
    (VOID)h_send();
    (VOID)h_send();

    h_check_eq(h_sock.nx_tcp_socket_transmit_sent_count, 4,
               "the fixture did not leave four segments outstanding");

    before = h_datagrams;

    (VOID)h_dupack();
    h_check(h_datagrams == before,
            "the first duplicate ack retransmitted on a flight of four");

    (VOID)h_dupack();
    h_check(h_datagrams == before,
            "the second duplicate ack retransmitted on a flight of four");

    (VOID)h_dupack();
    h_check(h_datagrams > before,
            "three duplicate acks on a flight of four did not retransmit");

    printf("  flight of four      retransmitted on duplicate ack 3\n");
}

static void single_segment(void)
{
    UINT before;

    /*
     * One segment out is the case no threshold reaches, because a duplicate
     * acknowledgment is a later segment arriving and there is no later
     * segment.  Nothing here should fire; what covers it is the tail loss
     * probe, which is a timer and belongs to test_tcp_retries.
     */
    h_fixture();
    (VOID)h_send();

    before = h_datagrams;
    (VOID)h_dupack();

    h_check(h_datagrams == before,
            "a flight of one retransmitted on a duplicate ack");
}

int main(void)
{
    _nx_tcp_fast_timer_rate     = (NX_IP_PERIODIC_RATE + (NX_TCP_FAST_TIMER_RATE - 1)) / NX_TCP_FAST_TIMER_RATE;
    _nx_tcp_ack_timer_rate      = (NX_IP_PERIODIC_RATE + (NX_TCP_ACK_TIMER_RATE - 1)) / NX_TCP_ACK_TIMER_RATE;
    _nx_tcp_transmit_timer_rate = (NX_IP_PERIODIC_RATE + (NX_TCP_TRANSMIT_TIMER_RATE - 1)) / NX_TCP_TRANSMIT_TIMER_RATE;

    printf("RFC 5827 early retransmit, against a socket rather than a network\n");
#ifdef NX_ENABLE_TCP_EARLY_RETRANSMIT
    printf("  threshold           %d, lowered to the flight below four\n",
           NX_TCP_FAST_RETRANSMIT_THRESHOLD);
#else
    printf("  threshold           %d, never lowered (built with it off)\n",
           NX_TCP_FAST_RETRANSMIT_THRESHOLD);
#endif

    small_flight();
    large_flight();
    single_segment();

    printf("%lu checks, %lu failures, %s\n",
           h_checks, h_failures, (h_failures == 0UL) ? "PASS" : "FAIL");

    return (h_failures == 0UL) ? 0 : 1;
}
