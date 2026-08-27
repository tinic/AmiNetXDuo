/*
 * Fast extraction from the common one-buffer NetX Duo receive packet.
 *
 * Kept in a small header so transfer.c can inline it even without LTO and the
 * host regression test executes the exact shipping implementation.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_BSDSOCKET_PACKET_EXTRACT_H
#define AMINETXDUO_BSDSOCKET_PACKET_EXTRACT_H

#include "nx_api.h"

#include <string.h>

static inline ULONG bsd_packet_length(const NX_PACKET *packet)
{
#ifdef TX_ENABLE_EVENT_TRACE
    ULONG length = 0;

    if (packet != NX_NULL)
        (VOID)nx_packet_length_get((NX_PACKET *)packet, &length);

    return length;
#else
    return (packet != NX_NULL) ? packet->nx_packet_length : 0;
#endif
}

/*
 * A packet returned by nx_tcp_socket_receive() is private to this socket
 * until it is released.  Bypass NetX Duo's generic chain walk only when the
 * requested range is wholly inside both the packet's logical length and its
 * first (and only) data buffer.  Every unusual or malformed shape retains the
 * native routine and therefore its status and bytes-copied semantics.
 *
 * Event-trace builds always retain the native routine: both NetX helpers emit
 * packet events, and removing those events would make an instrumented build
 * lie about the path it recorded.
 */
static inline UINT bsd_packet_extract(NX_PACKET *packet, ULONG offset,
                                      UCHAR *dst, ULONG want, ULONG *moved)
{
#ifndef TX_ENABLE_EVENT_TRACE
    if (packet != NX_NULL && dst != NX_NULL && moved != NX_NULL &&
        packet->nx_packet_prepend_ptr != NX_NULL &&
        packet->nx_packet_append_ptr >= packet->nx_packet_prepend_ptr)
    {
        ULONG logical = packet->nx_packet_length;
        ULONG contiguous = (ULONG)(packet->nx_packet_append_ptr -
                                   packet->nx_packet_prepend_ptr);

#ifndef NX_DISABLE_PACKET_CHAIN
        if (packet->nx_packet_next == NX_NULL)
#endif
        {
            /* NetX treats offset == logical as an error for a nonempty
               packet, even when want is zero; keep that exact boundary. */
            if (offset < logical && want <= logical - offset &&
                offset <= contiguous && want <= contiguous - offset)
            {
                memcpy(dst, packet->nx_packet_prepend_ptr + offset, want);
                *moved = want;
                return NX_SUCCESS;
            }
        }
    }
#endif

    return nx_packet_data_extract_offset(packet, offset, dst, want, moved);
}

#endif /* AMINETXDUO_BSDSOCKET_PACKET_EXTRACT_H */
