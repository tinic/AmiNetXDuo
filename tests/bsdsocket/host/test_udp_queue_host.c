/*
 * Regression for the layout of a UDP packet before nx_udp_socket_receive()
 * strips its header.  WaitSelect() and FIONREAD inspect packets at this stage.
 *
 * SPDX-License-Identifier: MIT
 */

#include "udp_queue.h"

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
    NX_UDP_HEADER *header;
    UINT          port = 0;
    ULONG         length = 0;

    memset(&storage, 0, sizeof(storage));
    memset(&packet, 0, sizeof(packet));

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

    packet.nx_packet_length = (ULONG)sizeof(*header);
    CHECK(bsd_udp_queue_info(&packet, &port, &length) == NX_SUCCESS);
    CHECK(length == 0UL);

    packet.nx_packet_length = (ULONG)sizeof(*header) - 1UL;
    CHECK(bsd_udp_queue_info(&packet, &port, &length) == NX_INVALID_PACKET);

    packet.nx_packet_prepend_ptr = NX_NULL;
    CHECK(bsd_udp_queue_info(&packet, &port, &length) == NX_INVALID_PACKET);
    CHECK(bsd_udp_queue_info(NX_NULL, &port, &length) == NX_INVALID_PACKET);

    puts("udp_queue: queued source port and payload length");
    return 0;
}
