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

#endif /* AMINETXDUO_BSDSOCKET_UDP_QUEUE_H */
