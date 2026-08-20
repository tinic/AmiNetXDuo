/*
 * Application-visible byte counts in NetX Duo's TCP queues.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_BSDSOCKET_TCP_QUEUE_H
#define AMINETXDUO_BSDSOCKET_TCP_QUEUE_H

#include "nx_api.h"

/* NetX maintains this counter as payload enters and leaves the unacknowledged
   list.  Walking that list must use nx_packet_tcp_queue_next, not the generic
   queue link, and packet lengths there still include their TCP headers. */
static ULONG bsd_tcp_send_queue_bytes(const NX_TCP_SOCKET *socket)
{
    return (socket != NX_NULL)
               ? socket->nx_tcp_socket_tx_outstanding_bytes
               : 0UL;
}

#endif /* AMINETXDUO_BSDSOCKET_TCP_QUEUE_H */
