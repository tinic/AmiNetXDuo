/*
 * The part of queued UDP inspection that depends on NetX Duo's packet layout.
 * Kept small so host CI can compile and execute the shipping implementation.
 *
 * SPDX-License-Identifier: MIT
 */

#include "udp_queue.h"

#include "nx_udp.h"

UINT bsd_udp_queue_info(const NX_PACKET *packet, UINT *source_port,
                        ULONG *payload_length)
{
    const NX_UDP_HEADER *header;

    if (packet == NX_NULL || packet->nx_packet_prepend_ptr == NX_NULL ||
        packet->nx_packet_length < (ULONG)sizeof(NX_UDP_HEADER))
        return NX_INVALID_PACKET;

    header = (const NX_UDP_HEADER *)(const VOID *)
             packet->nx_packet_prepend_ptr;

    if (source_port != NX_NULL)
        *source_port = (UINT)(header->nx_udp_header_word_0 >> NX_SHIFT_BY_16);

    if (payload_length != NX_NULL)
        *payload_length = packet->nx_packet_length -
                          (ULONG)sizeof(NX_UDP_HEADER);

    return NX_SUCCESS;
}

ULONG bsd_udp_queue_payload_bytes(const NX_PACKET *head, ULONG count)
{
    const ULONG maximum = (ULONG)-1;
    ULONG       bytes   = 0;
    ULONG       n;

    for (n = 0; n < count && head != NX_NULL; n++)
    {
        ULONG payload = 0;

        /* A malformed queued packet offers no application data.  Still
           follow the queue so one bad entry does not hide later datagrams. */
        if (bsd_udp_queue_info(head, NX_NULL, &payload) == NX_SUCCESS)
        {
            if (payload > maximum - bytes)
                return maximum;
            bytes += payload;
        }

        head = head->nx_packet_queue_next;
    }

    return bytes;
}
