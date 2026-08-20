/*
 * Regression for the layout of a UDP packet before nx_udp_socket_receive()
 * strips its header.  WaitSelect(), FIONREAD and GetNetworkStatistics()
 * inspect packets at this stage.  The TCP assertions cover the distinct sent
 * and receive-list accounting which the same statistics table reports.
 *
 * SPDX-License-Identifier: MIT
 */

#include "udp_queue.h"
#include "aminetxduo/nx_queue.h"

#include "nx_udp.h"

#include <stdio.h>
#include <string.h>

#define CHECK(expr)                                                        \
    do {                                                                   \
        if (!(expr)) {                                                     \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr);       \
            return 1;                                                      \
        }                                                                  \
    } while (0)

int main(void)
{
    union {
        ULONG words[8];
        UCHAR bytes[32];
    } storage;
    union {
        ULONG words[16];
        UCHAR bytes[64];
    } tcp_storage[2];
    NX_PACKET     packet;
    NX_PACKET     second;
    NX_TCP_SOCKET tcp;
    NX_UDP_SOCKET udp;
    NX_UDP_HEADER *header;
    UINT          port = 0;
    ULONG         length = 0;

    memset(&storage, 0, sizeof(storage));
    memset(tcp_storage, 0, sizeof(tcp_storage));
    memset(&packet, 0, sizeof(packet));
    memset(&second, 0, sizeof(second));
    memset(&tcp, 0, sizeof(tcp));
    memset(&udp, 0, sizeof(udp));

    /* Deliberately put another value where nxd_udp_source_extract() looks:
       two longwords before prepend_ptr.  That location is inside the IP
       header while the packet is queued and must not supply the source port. */
    storage.words[0] = 9UL << NX_SHIFT_BY_16;
    header = (NX_UDP_HEADER *)(void *)&storage.words[2];
    header->nx_udp_header_word_0 = (5353UL << NX_SHIFT_BY_16) | 49152UL;

    packet.nx_packet_prepend_ptr = (UCHAR *)(void *)header;
    packet.nx_packet_length = (ULONG)sizeof(*header) + 19UL;

    CHECK(bsd_udp_queue_info(&packet, &port, &length) == NX_SUCCESS);
    CHECK(port == 5353U);
    CHECK(length == 19UL);

    /* Queue statistics add payloads, not one private header per datagram. */
    second.nx_packet_prepend_ptr = (UCHAR *)(void *)header;
    second.nx_packet_length = (ULONG)sizeof(*header) + 7UL;
    packet.nx_packet_queue_next = &second;
    udp.nx_udp_socket_receive_head = &packet;
    udp.nx_udp_socket_receive_count = 2UL;
    CHECK(ami_nx_udp_receive_bytes(&udp) == 26UL);
    udp.nx_udp_socket_receive_count = 1UL;
    CHECK(ami_nx_udp_receive_bytes(&udp) == 19UL);

    /* The count is only an upper bound on a walk whose chain can end. */
    udp.nx_udp_socket_receive_count = 3UL;
    CHECK(ami_nx_udp_receive_bytes(&udp) == 26UL);

    /* A malformed entry contributes no bytes and does not hide its tail. */
    packet.nx_packet_length = (ULONG)sizeof(*header) - 1UL;
    udp.nx_udp_socket_receive_count = 2UL;
    CHECK(ami_nx_udp_receive_bytes(&udp) == 7UL);

    packet.nx_packet_length = (ULONG)sizeof(*header) + 19UL;

    packet.nx_packet_length = (ULONG)sizeof(*header);
    CHECK(bsd_udp_queue_info(&packet, &port, &length) == NX_SUCCESS);
    CHECK(length == 0UL);

    packet.nx_packet_length = (ULONG)sizeof(*header) - 1UL;
    CHECK(bsd_udp_queue_info(&packet, &port, &length) == NX_INVALID_PACKET);

    packet.nx_packet_prepend_ptr = NX_NULL;
    CHECK(bsd_udp_queue_info(&packet, &port, &length) == NX_INVALID_PACKET);
    CHECK(bsd_udp_queue_info(NX_NULL, &port, &length) == NX_INVALID_PACKET);

    /* TCP's Send-Q is the payload flight counter, not a sum through the
       generic packet link (the sent list does not use that link at all). */
    tcp.nx_tcp_socket_tx_outstanding_bytes = 37UL;
    tcp.nx_tcp_socket_transmit_sent_head = &packet;
    tcp.nx_tcp_socket_transmit_sent_count = 2UL;
    packet.nx_packet_length = 39UL;
    packet.nx_packet_queue_next = NX_NULL;
    packet.nx_packet_union_next.nx_packet_tcp_queue_next = &second;
    second.nx_packet_length = 38UL;
    CHECK(ami_nx_tcp_send_bytes(&tcp) == 37UL);

    /* Receive packets retain their TCP headers and use the TCP-specific link;
       nx_packet_queue_next is a readiness sentinel, not a list pointer. */
    packet.nx_packet_prepend_ptr = tcp_storage[0].bytes;
    packet.nx_packet_length = 39UL;
    packet.nx_packet_queue_next = (NX_PACKET *)NX_PACKET_READY;
    packet.nx_packet_union_next.nx_packet_tcp_queue_next = &second;
    ((NX_TCP_HEADER *)packet.nx_packet_prepend_ptr)->nx_tcp_header_word_3 =
        5UL << NX_TCP_HEADER_SHIFT;

    second.nx_packet_prepend_ptr = tcp_storage[1].bytes;
    second.nx_packet_length = 38UL;
    second.nx_packet_queue_next = (NX_PACKET *)NX_PACKET_READY;
    second.nx_packet_union_next.nx_packet_tcp_queue_next =
        (NX_PACKET *)NX_PACKET_ENQUEUED;
    ((NX_TCP_HEADER *)second.nx_packet_prepend_ptr)->nx_tcp_header_word_3 =
        5UL << NX_TCP_HEADER_SHIFT;

    tcp.nx_tcp_socket_receive_queue_head = &packet;
    tcp.nx_tcp_socket_receive_queue_tail = &second;
    tcp.nx_tcp_socket_receive_queue_count = 2UL;
    CHECK(ami_nx_tcp_receive_bytes(&tcp) == 37UL);

    packet.nx_packet_queue_next = NX_NULL;
    CHECK(ami_nx_tcp_receive_bytes(&tcp) == 0UL);

    puts("socket queues: application-visible byte counts");
    return 0;
}
