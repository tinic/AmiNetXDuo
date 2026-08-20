/*
 * Application-visible byte counts in NetX Duo socket queues.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NX_QUEUE_H
#define AMINETXDUO_NX_QUEUE_H

#include "nx_api.h"
#include "nx_packet.h"
#include "nx_tcp.h"
#include "nx_udp.h"

/* NetX maintains this counter as payload enters and leaves the unacknowledged
   list.  Walking that list must use nx_packet_tcp_queue_next, not the generic
   queue link, and packet lengths there still include their TCP headers. */
static ULONG ami_nx_tcp_send_bytes(const NX_TCP_SOCKET *socket)
{
    return (socket != NX_NULL)
               ? socket->nx_tcp_socket_tx_outstanding_bytes
               : 0UL;
}

/* Receive packets use nx_packet_tcp_queue_next for the list; the generic
   link is instead a readiness marker.  Only the ready, contiguous prefix is
   visible to recv(), and packet lengths still include their TCP headers. */
static ULONG ami_nx_tcp_receive_bytes(const NX_TCP_SOCKET *socket)
{
    const NX_PACKET *packet;
    ULONG            bytes = 0;
    ULONG            n;

    if (socket == NX_NULL)
        return 0UL;

    packet = socket->nx_tcp_socket_receive_queue_head;

    for (n = 0; n < socket->nx_tcp_socket_receive_queue_count &&
                packet != NX_NULL; n++)
    {
        const NX_TCP_HEADER *header;
        ULONG                header_length;

        if (packet->nx_packet_queue_next != (NX_PACKET *)NX_PACKET_READY)
            break;

        header = (const NX_TCP_HEADER *)packet->nx_packet_prepend_ptr;
        header_length =
            (header->nx_tcp_header_word_3 >> NX_TCP_HEADER_SHIFT) *
            (ULONG)sizeof(ULONG);

        if (header_length > packet->nx_packet_length)
            break;

        if (packet->nx_packet_length - header_length > (ULONG)-1 - bytes)
            return (ULONG)-1;
        bytes += packet->nx_packet_length - header_length;

        if (packet == socket->nx_tcp_socket_receive_queue_tail)
            break;

        packet = packet->nx_packet_union_next.nx_packet_tcp_queue_next;
    }

    return bytes;
}

/* NetX retains the UDP header until nx_udp_socket_receive() returns a packet.
   Queue reports describe the payload which recv() can return. */
static ULONG ami_nx_udp_receive_bytes(const NX_UDP_SOCKET *socket)
{
    const NX_PACKET *packet;
    ULONG            bytes = 0;
    ULONG            n;

    if (socket == NX_NULL)
        return 0UL;

    packet = socket->nx_udp_socket_receive_head;

    for (n = 0; n < socket->nx_udp_socket_receive_count &&
                packet != NX_NULL; n++)
    {
        if (packet->nx_packet_length >= (ULONG)sizeof(NX_UDP_HEADER))
        {
            ULONG payload = packet->nx_packet_length -
                            (ULONG)sizeof(NX_UDP_HEADER);

            if (payload > (ULONG)-1 - bytes)
                return (ULONG)-1;
            bytes += payload;
        }

        packet = packet->nx_packet_queue_next;
    }

    return bytes;
}

#endif /* AMINETXDUO_NX_QUEUE_H */
