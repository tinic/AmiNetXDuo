/*
 * Regression for the layout of a UDP packet before nx_udp_socket_receive()
 * strips its header.  WaitSelect(), FIONREAD and GetNetworkStatistics()
 * inspect packets at this stage.  The last assertion covers TCP's separate
 * sent-list accounting, which the same statistics table reports.
 *
 * SPDX-License-Identifier: MIT
 */

#include "udp_queue.h"
#include "tcp_queue.h"

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
    NX_PACKET     packet;
    NX_PACKET     second;
    NX_TCP_SOCKET tcp;
    NX_UDP_HEADER *header;
    UINT          port = 0;
    ULONG         length = 0;

    memset(&storage, 0, sizeof(storage));
    memset(&packet, 0, sizeof(packet));
    memset(&second, 0, sizeof(second));
    memset(&tcp, 0, sizeof(tcp));

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
    CHECK(bsd_udp_queue_payload_bytes(&packet, 2UL) == 26UL);
    CHECK(bsd_udp_queue_payload_bytes(&packet, 1UL) == 19UL);

    /* The count is only an upper bound on a walk whose chain can end. */
    CHECK(bsd_udp_queue_payload_bytes(&packet, 3UL) == 26UL);

    /* A malformed entry contributes no bytes and does not hide its tail. */
    packet.nx_packet_length = (ULONG)sizeof(*header) - 1UL;
    CHECK(bsd_udp_queue_payload_bytes(&packet, 2UL) == 7UL);

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
    CHECK(bsd_tcp_send_queue_bytes(&tcp) == 37UL);

    puts("socket queues: application-visible byte counts");
    return 0;
}
