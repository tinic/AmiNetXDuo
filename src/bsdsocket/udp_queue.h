/*
 * Interpret the UDP header NetX leaves on a packet while it is queued.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_BSDSOCKET_UDP_QUEUE_H
#define AMINETXDUO_BSDSOCKET_UDP_QUEUE_H

#include "nx_api.h"

/* Either output may be NULL. */
UINT bsd_udp_queue_info(const NX_PACKET *packet, UINT *source_port,
                        ULONG *payload_length);

/* Application-visible bytes in up to `count` queued datagrams. */
ULONG bsd_udp_queue_payload_bytes(const NX_PACKET *head, ULONG count);

#endif /* AMINETXDUO_BSDSOCKET_UDP_QUEUE_H */
