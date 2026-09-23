/*
 * AmiNetXDuo, the send the cork's pass makes: nx_tcp_socket_send() with
 * NX_NO_WAIT from the IP thread, which already holds nx_ip_protection for its
 * whole event pass (nx_ip_thread_entry.c).  Against the real
 * _nx_tcp_socket_send_internal() and the real transmit check.
 *
 * WHAT IS CLAIMED (src/bsdsocket/cork.c relies on each):
 *
 *   - It never suspends: not on the transmit list, not on the pool, not on the
 *     mutex.  The send takes the mutex again and gives it back around its
 *     segment allocation, and nested under the pass's own hold that is a count
 *     going 2 -> 1 -> 2 -- never 0, so no other thread gets in mid-send.
 *
 *   - A refusal leaves the remainder on the caller's packet, trimmed off the
 *     front: length reduced and prepend advanced by exactly what reached the
 *     wire, which is the arithmetic bsd_send_consumed() (transfer.c) and the
 *     cork's reattach both do.  Nothing is credited twice.
 *
 *   - The statuses the cork's settle step switches on come back as named:
 *     NX_WINDOW_OVERFLOW, NX_TX_QUEUE_DEPTH, NX_NO_PACKET, NX_NOT_CONNECTED.
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

static NX_IP h_ip;

/*
 * nx_ip_protection, modelled: an owner and a count.  A get by a thread that
 * is not the owner while it is held is a suspension -- counted, and a failure
 * here, because the pass must never wait.  The low-water mark is what proves
 * the IP thread's own hold was never let go during a send.
 */
static TX_THREAD *h_mutex_owner;
static ULONG      h_mutex_count;
static ULONG      h_mutex_low;
static UINT       h_suspends;
static UINT       h_alloc_waits;         /* allocations asked to wait     */

UINT _tx_mutex_get(TX_MUTEX *mutex_ptr, ULONG wait_option)
{
    (void)wait_option;

    if (mutex_ptr != &h_ip.nx_ip_protection)
        return TX_SUCCESS;

    if (h_mutex_count != 0 && h_mutex_owner != _tx_thread_current_ptr)
    {
        h_suspends++;
        return TX_NOT_AVAILABLE;
    }

    h_mutex_owner = _tx_thread_current_ptr;
    h_mutex_count++;
    return TX_SUCCESS;
}

UINT _tx_mutex_put(TX_MUTEX *mutex_ptr)
{
    if (mutex_ptr != &h_ip.nx_ip_protection)
        return TX_SUCCESS;

    if (h_mutex_count == 0)
    {
        printf("FAIL mutex put with nothing held\n");
        h_failures++;
        return TX_NOT_OWNED;
    }

    h_mutex_count--;
    if (h_mutex_count < h_mutex_low)
        h_mutex_low = h_mutex_count;
    if (h_mutex_count == 0)
        h_mutex_owner = TX_NULL;
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
        *priority = 0;
    return TX_SUCCESS;
}

UINT _tx_thread_preemption_change(TX_THREAD *thread_ptr, UINT new_threshold,
                                  UINT *old_threshold)
{
    (void)thread_ptr;
    (void)new_threshold;

    /* Only a sender about to suspend raises its threshold. */
    h_suspends++;
    if (old_threshold)
        *old_threshold = 0;
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
    h_suspends++;
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

/* Segments the send carves off the caller's packet come from here. */
#define H_BUF           2048
#define H_SEGS          8

static NX_PACKET_POOL h_pool;
static NX_PACKET      h_seg[H_SEGS];
static UCHAR          h_seg_buf[H_SEGS][H_BUF];
static UINT           h_seg_next;
static int            h_alloc_fails;

UINT _nx_packet_allocate(NX_PACKET_POOL *pool_ptr, NX_PACKET **packet_ptr,
                         ULONG packet_type, ULONG wait_option)
{
    NX_PACKET *p;

    (void)pool_ptr;

    if (wait_option != NX_NO_WAIT)
        h_alloc_waits++;

    if (h_alloc_fails || h_seg_next >= H_SEGS)
        return NX_NO_PACKET;

    p = &h_seg[h_seg_next];
    memset(p, 0, sizeof(*p));
    p -> nx_packet_data_start  = h_seg_buf[h_seg_next];
    p -> nx_packet_data_end    = h_seg_buf[h_seg_next] + H_BUF;
    p -> nx_packet_prepend_ptr = h_seg_buf[h_seg_next] + packet_type;
    p -> nx_packet_append_ptr  = p -> nx_packet_prepend_ptr;
    p -> nx_packet_pool_owner  = &h_pool;
    h_seg_next++;

    *packet_ptr = p;
    return NX_SUCCESS;
}

UINT _nx_packet_release(NX_PACKET *packet_ptr)
{
    (void)packet_ptr;
    return NX_SUCCESS;
}

/* The carving path copies the window's worth into a fresh segment here.
   Single buffer: a segment packet has room for an MSS. */
UINT _nx_packet_data_append(NX_PACKET *packet_ptr, VOID *data_start, ULONG data_size,
                            NX_PACKET_POOL *pool_ptr, ULONG wait_option)
{
    (void)pool_ptr;

    if (wait_option != NX_NO_WAIT)
        h_alloc_waits++;

    if ((ULONG)(packet_ptr -> nx_packet_data_end -
                packet_ptr -> nx_packet_append_ptr) < data_size)
        return NX_NO_PACKET;

    memcpy(packet_ptr -> nx_packet_append_ptr, data_start, data_size);
    packet_ptr -> nx_packet_append_ptr += data_size;
    packet_ptr -> nx_packet_length     += data_size;
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

/* What reached the wire: segments, and the payload bytes in order. */
static UINT  h_datagrams;
static ULONG h_wire_bytes;
static UCHAR h_wire[4096];

VOID _nx_ip_packet_send(NX_IP *ip_ptr, NX_PACKET *packet_ptr,
                        ULONG destination_ip, ULONG type_of_service,
                        ULONG time_to_live, ULONG protocol, ULONG fragment,
                        ULONG next_hop_address)
{
    NX_TCP_HEADER *hdr = (NX_TCP_HEADER *)packet_ptr -> nx_packet_prepend_ptr;
    ULONG          word_3 = hdr -> nx_tcp_header_word_3;
    ULONG          hlen, payload;

    (void)ip_ptr; (void)destination_ip; (void)type_of_service;
    (void)time_to_live; (void)protocol; (void)fragment; (void)next_hop_address;

    if (h_mutex_count == 0)
    {
        printf("FAIL a segment went out with nx_ip_protection released\n");
        h_failures++;
    }

    NX_CHANGE_ULONG_ENDIAN(word_3);
    hlen    = (word_3 >> 28) * 4UL;
    payload = packet_ptr -> nx_packet_length - hlen;

    if (h_wire_bytes + payload <= sizeof(h_wire))
        memcpy(&h_wire[h_wire_bytes],
               packet_ptr -> nx_packet_prepend_ptr + hlen, payload);

    h_datagrams++;
    h_wire_bytes += payload;

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
}

VOID _nx_tcp_socket_connection_reset(NX_TCP_SOCKET *socket_ptr)
{
    (void)socket_ptr;
}

UINT _nx_tcp_socket_state_wait(NX_TCP_SOCKET *socket_ptr, UINT desired_state,
                               ULONG wait_option)
{
    (void)socket_ptr; (void)desired_state; (void)wait_option;
    h_suspends++;
    return NX_SUCCESS;
}

VOID _nx_tcp_socket_retransmit_queue_flush(NX_TCP_SOCKET *socket_ptr)
{
    (void)socket_ptr;
}

#define H_MSS           1460UL
#define H_PEER_WINDOW   65535UL
#define H_ISN           0x10000000UL
#define H_ISN_RX        0x20000000UL

static NX_INTERFACE   h_iface;
static NX_TCP_SOCKET  h_sock;

static NX_PACKET      h_pkt;
static UCHAR          h_pkt_buf[H_BUF];
static UCHAR          h_data[H_BUF];

static void h_fixture(void)
{
    ULONG i;

    memset(&h_ip, 0, sizeof(h_ip));
    memset(&h_iface, 0, sizeof(h_iface));
    memset(&h_pool, 0, sizeof(h_pool));

    h_now         = 1000;
    h_datagrams   = 0;
    h_wire_bytes  = 0;
    h_seg_next    = 0;
    h_alloc_fails = 0;
    h_suspends    = 0;
    h_alloc_waits = 0;

    for (i = 0; i < H_BUF; i++)
        h_data[i] = (UCHAR)(i * 13 + 5);

    h_pool.nx_packet_pool_payload_size = H_BUF;
    h_pool.nx_packet_pool_available    = H_SEGS;

    h_iface.nx_interface_ip_address = 0xC0A80102UL;

    _nx_tcp_socket_create(&h_ip, &h_sock, "host cork", NX_IP_NORMAL,
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
    h_sock.nx_tcp_socket_tx_sequence_recover      = H_ISN - 1;
    h_sock.nx_tcp_socket_previous_highest_ack     = H_ISN - 1;
    h_sock.nx_tcp_socket_rx_sequence              = H_ISN_RX;

    /* The IP thread, in its event pass: nx_ip_protection held once. */
    _tx_thread_current_ptr = &h_ip.nx_ip_thread;
    h_mutex_owner = TX_NULL;
    h_mutex_count = 0;
    (void)_tx_mutex_get(&h_ip.nx_ip_protection, TX_WAIT_FOREVER);
    h_mutex_low = h_mutex_count;
}

/* The cork's segment: NX_TCP_PACKET headroom, `bytes` of payload, prepend
   offset by `skew` so a caller can force the carving path. */
static NX_PACKET *h_segment(ULONG bytes, ULONG skew)
{
    NX_PACKET *p = &h_pkt;

    memset(p, 0, sizeof(*p));
    memset(h_pkt_buf, 0, sizeof(h_pkt_buf));

    p -> nx_packet_data_start  = h_pkt_buf;
    p -> nx_packet_data_end    = h_pkt_buf + H_BUF;
    p -> nx_packet_prepend_ptr = h_pkt_buf + NX_TCP_PACKET + skew;
    p -> nx_packet_append_ptr  = p -> nx_packet_prepend_ptr + bytes;
    p -> nx_packet_length      = bytes;
    p -> nx_packet_pool_owner  = &h_pool;
    memcpy(p -> nx_packet_prepend_ptr, h_data, bytes);

    return p;
}

/* transfer.c's bsd_send_consumed(), restated. */
static ULONG h_consumed(const NX_PACKET *p, ULONG filled)
{
    ULONG left = p -> nx_packet_length;

    return (left >= filled) ? 0UL : filled - left;
}

static void h_pass_invariants(const char *what)
{
    char line[160];

    snprintf(line, sizeof(line), "%s: suspended", what);
    h_check_eq(h_suspends, 0, line);

    snprintf(line, sizeof(line), "%s: an allocation was asked to wait", what);
    h_check_eq(h_alloc_waits, 0, line);

    snprintf(line, sizeof(line),
             "%s: nx_ip_protection dropped below the pass's own hold", what);
    h_check_eq(h_mutex_low, 1, line);

    snprintf(line, sizeof(line), "%s: the pass's hold did not come back", what);
    h_check_eq(h_mutex_count, 1, line);
}

static void a_whole_segment(void)
{
    NX_PACKET *p;
    UINT       status;

    h_fixture();
    p = h_segment(300, 0);

    status = _nx_tcp_socket_send_internal(&h_sock, p, NX_NO_WAIT);

    h_check_eq(status, NX_SUCCESS, "an open window refused the segment");
    h_check_eq(h_datagrams, 1, "the segment is one datagram");
    h_check(h_wire_bytes == 300 && memcmp(h_wire, h_data, 300) == 0,
            "the payload on the wire is the segment");
    h_pass_invariants("open window");

    printf("  open window         %u datagram, %lu bytes\n",
           (unsigned int)h_datagrams, (unsigned long)h_wire_bytes);
}

static void b_partial_window(void)
{
    NX_PACKET *p;
    UCHAR     *prepend;
    UINT       status;

    h_fixture();

    /* Room for 300 of a 1000-byte segment, with nothing in flight (the SWS
       rule then lets the sliver go). */
    h_sock.nx_tcp_socket_tx_window_advertised = 300;
    p       = h_segment(1000, 0);
    prepend = p -> nx_packet_prepend_ptr;

    status = _nx_tcp_socket_send_internal(&h_sock, p, NX_NO_WAIT);

    h_check_eq(status, NX_WINDOW_OVERFLOW,
               "a window short of the segment is not NX_WINDOW_OVERFLOW");
    h_check_eq(h_wire_bytes, 300, "not what the window had room for");
    h_check_eq(p -> nx_packet_length, 700,
               "the remainder's length is not the segment less what went");
    h_check(p -> nx_packet_prepend_ptr == prepend + 300,
            "the remainder does not start where the wire stopped");
    h_check(memcmp(p -> nx_packet_prepend_ptr, h_data + 300, 700) == 0,
            "the remainder's bytes are not the ones still owed");
    h_check_eq(h_consumed(p, 1000), h_wire_bytes,
               "bsd_send_consumed() would credit other than what went");
    h_pass_invariants("partial window");

    /* The window opens: the remainder goes whole and nothing twice. */
    h_sock.nx_tcp_socket_tx_window_advertised = H_PEER_WINDOW;
    h_sock.nx_tcp_socket_tx_outstanding_bytes = 0;
    status = _nx_tcp_socket_send_internal(&h_sock, p, NX_NO_WAIT);
    h_check_eq(status, NX_SUCCESS, "the reopened window refused the rest");
    h_check(h_wire_bytes == 1000 && memcmp(h_wire, h_data, 1000) == 0,
            "the stream is not the segment, once, in order");
    h_pass_invariants("reopened window");

    printf("  partial window      300 went, 700 kept, then the 700\n");
}

static void c_closed_window_and_queue(void)
{
    NX_PACKET *p;
    UINT       status;

    h_fixture();
    h_sock.nx_tcp_socket_tx_window_advertised = 0;
    h_sock.nx_tcp_socket_tx_outstanding_bytes = 512;
    p = h_segment(100, 0);

    status = _nx_tcp_socket_send_internal(&h_sock, p, NX_NO_WAIT);
    h_check_eq(status, NX_WINDOW_OVERFLOW, "a zero window");
    h_check_eq(p -> nx_packet_length, 100, "a zero window took bytes");
    h_check_eq(h_datagrams, 0, "a zero window sent a segment");
    h_pass_invariants("zero window");

    h_fixture();
    h_sock.nx_tcp_socket_transmit_sent_count =
        h_sock.nx_tcp_socket_transmit_queue_maximum;
    p = h_segment(100, 0);

    status = _nx_tcp_socket_send_internal(&h_sock, p, NX_NO_WAIT);
    h_check_eq(status, NX_TX_QUEUE_DEPTH, "a full transmit queue");
    h_check_eq(p -> nx_packet_length, 100, "a full queue took bytes");
    h_pass_invariants("full queue");

    printf("  closed              NX_WINDOW_OVERFLOW, NX_TX_QUEUE_DEPTH\n");
}

static void d_no_segment_packet(void)
{
    NX_PACKET *p;
    UINT       status;

    /* A prepend that is not four-aligned sends the packet down the carving
       path, which allocates -- and here finds nothing. */
    h_fixture();
    h_alloc_fails = 1;
    p = h_segment(100, 2);

    status = _nx_tcp_socket_send_internal(&h_sock, p, NX_NO_WAIT);
    h_check_eq(status, NX_NO_PACKET, "an empty pool is not NX_NO_PACKET");
    h_check_eq(p -> nx_packet_length, 100, "an empty pool took bytes");
    h_pass_invariants("empty pool");

    printf("  empty pool          NX_NO_PACKET, segment intact\n");
}

static void e_not_connected(void)
{
    NX_PACKET *p;
    UINT       status;

    h_fixture();
    h_sock.nx_tcp_socket_state = NX_TCP_CLOSED;
    p = h_segment(100, 0);

    status = _nx_tcp_socket_send_internal(&h_sock, p, NX_NO_WAIT);
    h_check_eq(status, NX_NOT_CONNECTED, "a closed connection");
    h_check_eq(h_datagrams, 0, "a closed connection sent something");
    h_pass_invariants("closed connection");

    printf("  closed connection   NX_NOT_CONNECTED\n");
}

int main(void)
{
    printf("cork: the IP thread's NX_NO_WAIT send\n");

    a_whole_segment();
    b_partial_window();
    c_closed_window_and_queue();
    d_no_segment_packet();
    e_not_connected();

    printf("%lu checks, %lu failures\n", h_checks, h_failures);

    return (h_failures == 0) ? 0 : 1;
}
