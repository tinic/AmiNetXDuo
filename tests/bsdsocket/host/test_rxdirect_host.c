/*
 * src/bsdsocket/rxdirect.c on the host: the completer half of the
 * pending-receive descriptor, which runs on the IP thread inside the TCP
 * receive notify.
 *
 * THE CLAIM UNDER TEST.  _nx_tcp_socket_state_data_check() reads the arriving
 * segment's TCP header again after the notify returns
 * (third_party/netxduo/common/src/nx_tcp_socket_state_data_check.c:1286), and
 * that segment is not always the queue tail -- one that fills a hole is
 * spliced in ahead of the out-of-order packets behind it (ibid.:905/912).  So
 * the completer must not release a packet while it is on the IP thread, or a
 * thread suspended on the packet pool is resumed with the segment whose header
 * NetX Duo is still going to read.  On the caller's own thread, inside the
 * bracket, releasing is what the classic path already does.
 *
 * The NetX Duo entry points are stubbed, not linked: the fixture is a queue of
 * NX_PACKETs and a count of releases.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long h_checks;
static unsigned long h_failures;

#define CHECK(cond, what)                                                     \
    do {                                                                      \
        h_checks++;                                                           \
        if (!(cond)) {                                                        \
            h_failures++;                                                     \
            printf("  FAIL %s\n", (what));                                    \
        }                                                                     \
    } while (0)

#define H_PKTS      4
#define H_PAYLOAD   1460
#define H_BUF       8192

static AmiSocket h_sock;
static NX_PACKET h_pkt[H_PKTS];
static UBYTE     h_payload[H_PKTS][H_PAYLOAD];
static UBYTE     h_dst[H_BUF];

static struct
{
    NX_PACKET *queue[H_PKTS];       /* what nx_tcp_socket_receive() hands out */
    unsigned   queued;
    unsigned   next;

    unsigned   releases;
    NX_PACKET *released[H_PKTS];

    /* The packet _nx_tcp_socket_state_data_check() will read again once the
       notify returns.  Releasing it is the defect this file exists for. */
    NX_PACKET *trigger;
} h;

/* ---- the NetX Duo surface rxdirect.c calls ---------------------------- */

UINT _nxe_tcp_socket_receive(NX_TCP_SOCKET *socket_ptr, NX_PACKET **packet_ptr,
                            ULONG wait_option)
{
    (VOID)wait_option;

    if (h.next >= h.queued)
    {
        *packet_ptr = NX_NULL;
        return NX_NO_PACKET;
    }

    *packet_ptr = h.queue[h.next++];

    /* The socket's queue head is what the completer reads to decide whether
       the packet it holds was the last one.  Keep it honest. */
    socket_ptr -> nx_tcp_socket_receive_queue_head =
        (h.next < h.queued) ? h.queue[h.next] : NX_NULL;

    return NX_SUCCESS;
}

UINT _nxe_packet_length_get(NX_PACKET *packet_ptr, ULONG *length)
{
    *length = packet_ptr -> nx_packet_length;
    return NX_SUCCESS;
}

UINT _nxe_packet_data_extract_offset(NX_PACKET *packet_ptr, ULONG offset,
                                    VOID *buffer_start, ULONG buffer_length,
                                    ULONG *bytes_copied)
{
    ULONG avail = packet_ptr -> nx_packet_length - offset;
    ULONG take  = (buffer_length < avail) ? buffer_length : avail;

    memcpy(buffer_start, packet_ptr -> nx_packet_prepend_ptr + offset, take);
    *bytes_copied = take;

    return NX_SUCCESS;
}

UINT _nxe_packet_release(NX_PACKET **packet_ptr_ptr)
{
    NX_PACKET *packet_ptr = *packet_ptr_ptr;

    if (h.releases < H_PKTS)
        h.released[h.releases] = packet_ptr;
    h.releases++;

    /* A release hands the packet to the pool, where a thread suspended on it
       is resumed holding it and writes its own data over the payload.  That
       is what makes the header read after the notify a read of someone else's
       segment; poison it so a test can see it happen. */
    memset(packet_ptr -> nx_packet_prepend_ptr, 0xA5,
           packet_ptr -> nx_packet_length);

    return NX_SUCCESS;
}

/* ---- fixture ---------------------------------------------------------- */

static void h_reset(unsigned packets, ULONG want)
{
    unsigned i;

    memset(&h_sock, 0, sizeof(h_sock));
    memset(&h_pkt, 0, sizeof(h_pkt));
    memset(&h, 0, sizeof(h));
    memset(h_dst, 0, sizeof(h_dst));

    for (i = 0; i < packets; i++)
    {
        memset(h_payload[i], (int)('A' + i), H_PAYLOAD);

        h_pkt[i].nx_packet_prepend_ptr = h_payload[i];
        h_pkt[i].nx_packet_append_ptr  = h_payload[i] + H_PAYLOAD;
        h_pkt[i].nx_packet_length      = H_PAYLOAD;

        h.queue[i] = &h_pkt[i];
    }

    h.queued  = packets;
    h_sock.as_Nx.tcp.nx_tcp_socket_receive_queue_head =
        (packets != 0) ? h.queue[0] : NX_NULL;

    h_sock.as_Flags     = ASF_TCP | ASF_CONNECTED;
    h_sock.as_RxDDst    = h_dst;
    h_sock.as_RxDWant   = want;
    h_sock.as_RxDFilled = 0;
    h_sock.as_RxDStatus = NX_NO_PACKET;
    h_sock.as_RxDState  = BSD_RXD_ARMED;
}

static BOOL h_was_released(NX_PACKET *p)
{
    unsigned i;

    for (i = 0; i < h.releases && i < H_PKTS; i++)
        if (h.released[i] == p)
            return TRUE;

    return FALSE;
}

/* ---- the tests -------------------------------------------------------- */

/*
 * The hole-filling segment is spliced in at the HEAD of the receive queue,
 * with the out-of-order packets that were waiting on it behind.  It is the
 * first packet the completer dequeues and the last one NetX Duo reads.
 */
static void t_ip_thread_never_releases(void)
{
    printf(" IP thread, hole filled at the head of the queue\n");

    h_reset(3, 4096);
    h.trigger = &h_pkt[0];

    bsd_rxdirect_pump(&h_sock, FALSE);

    CHECK(h.releases == 0,
          "the completer on the IP thread releases nothing");
    CHECK(!h_was_released(h.trigger),
          "and above all not the segment whose header NetX Duo re-reads");
    CHECK(h_sock.as_RxDState == BSD_RXD_DONE,
          "the descriptor still completes");
    CHECK(h_sock.as_RxDFilled == H_PAYLOAD,
          "with the one segment the notify was fired for");
    CHECK(h_sock.as_RxPending == &h_pkt[0],
          "the drained packet is parked, not returned to the pool");
    CHECK(h_sock.as_RxOffset == H_PAYLOAD,
          "parked drained, so the next recv() releases it");
    CHECK(h_payload[0][0] == 'A',
          "the parked segment's payload is intact for the header re-read");
    CHECK(h_dst[0] == 'A' && h_dst[H_PAYLOAD - 1] == 'A',
          "and the caller's buffer has it");
}

/*
 * Same queue, but the completer is running on the caller's own thread inside
 * the bracket.  Nothing is about to read a header, so a drained packet goes
 * back to the pool and the read spans the whole queue.
 */
static void t_caller_thread_releases(void)
{
    printf(" caller thread, inside the bracket\n");

    h_reset(3, 4096);

    bsd_rxdirect_pump(&h_sock, TRUE);

    CHECK(h.releases == 2,
          "the two drained packets go back to the pool");
    CHECK(h_sock.as_RxDFilled == 2 * H_PAYLOAD + (4096 - 2 * H_PAYLOAD),
          "the read fills the caller's buffer");
    CHECK(h_sock.as_RxPending == &h_pkt[2],
          "the partially consumed packet is the one parked");
    CHECK(h_sock.as_RxOffset == 4096 - 2 * H_PAYLOAD,
          "at the offset the copy stopped");
    CHECK(h_sock.as_RxDState == BSD_RXD_DONE, "and the descriptor completes");
}

/*
 * A partial consume parks the packet at its offset whichever thread runs, and
 * an empty queue completes nothing at all -- the caller must fall back rather
 * than return a zero-length read, which recv() spells end-of-file.
 */
static void t_partial_and_empty(void)
{
    printf(" partial consume, and an empty queue\n");

    h_reset(1, 512);
    bsd_rxdirect_pump(&h_sock, FALSE);
    CHECK(h_sock.as_RxDFilled == 512, "a short want takes only what it asked");
    CHECK(h_sock.as_RxPending == &h_pkt[0] && h_sock.as_RxOffset == 512,
          "and parks the rest of the segment");
    CHECK(h.releases == 0, "nothing released");

    h_reset(0, 4096);
    bsd_rxdirect_pump(&h_sock, FALSE);
    CHECK(h_sock.as_RxDState == BSD_RXD_ARMED,
          "an empty queue leaves the descriptor armed");
    CHECK(h_sock.as_RxDFilled == 0, "with nothing copied");
    CHECK(h_sock.as_RxDStatus == NX_NO_PACKET, "and the reason recorded");

    /* Already holding a partially consumed packet: the classic path owns it,
       so the completer must not dequeue behind it and reorder the stream. */
    h_reset(2, 4096);
    h_sock.as_RxPending = &h_pkt[1];
    h_sock.as_RxOffset  = 100;
    bsd_rxdirect_pump(&h_sock, TRUE);
    CHECK(h_sock.as_RxDFilled == 0,
          "a descriptor with a packet already parked copies nothing");
    CHECK(h.releases == 0, "and releases nothing");
}

int main(void)
{
    printf("rxdirect.c host tests\n");

    t_ip_thread_never_releases();
    t_caller_thread_releases();
    t_partial_and_empty();

    printf("%lu checks, %lu failures\n", h_checks, h_failures);
    return h_failures == 0 ? 0 : 1;
}
