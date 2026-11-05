/*
 * Whether recv() on a NetX Duo TCP socket can return data now.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NX_READY_H
#define AMINETXDUO_NX_READY_H

#include "nx_api.h"
#include "nx_packet.h"

/* The test that NetX Duo's _nx_tcp_socket_receive() makes before it hands
   over a packet: a head marked NX_PACKET_READY.  NetX Duo marks a segment
   ready when it arrives in order, or when the segment before it fills the
   gap.  One that arrived out of order is queued, and counted in
   nx_tcp_socket_receive_queue_count, with nx_packet_queue_next at NX_NULL,
   and recv() cannot return it: neither the count nor a bare head says that
   anything is readable.  The segment that fills the gap advances
   rx_sequence, which calls the receive notify, so a reader waiting on that
   notify loses no wakeup.

   A load or two, without nx_ip_protection, as bsd_readable() needs.  On
   its own in this header (nx_queue.h's helpers have other callers), so that
   select.c and tests/netsim include it without unused functions. */
static UINT ami_nx_tcp_receive_ready(const NX_TCP_SOCKET *socket)
{
    const NX_PACKET *head = socket->nx_tcp_socket_receive_queue_head;

    return (head != NX_NULL &&
            head->nx_packet_queue_next == (NX_PACKET *)NX_PACKET_READY)
               ? NX_TRUE : NX_FALSE;
}

#endif /* AMINETXDUO_NX_READY_H */
