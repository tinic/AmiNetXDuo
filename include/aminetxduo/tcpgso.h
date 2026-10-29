/*
 * AmiNetXDuo, TCP segmentation offload between NetX Duo and the SANA-II shim.
 *
 * A GSO packet is the IPv4 and TCP headers of its first segment followed by
 * up to N * mss bytes of payload, possibly chained, with
 * NX_INTERFACE_CAPABILITY_TCP_GSO in nx_packet_interface_capability_flag.
 * The record below sits at the first packet's nx_packet_data_start, in the
 * bytes allocated in front of the ordinary headroom.  The IPv4 ID in the
 * header is the first of k consecutive IDs the core reserved for the block.
 * sana2_tx.c cuts the block into wire frames; sana2_copy.c writes them.
 *
 *   flags 0       the whole block; offset and length 0
 *   REPAIR        one frame for payload bytes [offset, offset + length),
 *                 0 < length <= mss: seq + offset, PSH only if it ends the
 *                 payload, no FIN and no CWR, the header's IPv4 ID
 *
 * A version 1 record is the first four bytes alone and means flags 0.
 *
 * Include after nx_api.h.  The contract block is the vendor's own once the
 * fork carries it, and its guard then drops this copy without an edit.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AMINETXDUO_TCPGSO_H
#define AMINETXDUO_TCPGSO_H

#ifndef NX_INTERFACE_CAPABILITY_TCP_GSO

/* Record version 2, as agreed with codex on 2026-10-29; the vendor header
   that carries it replaces this block.  89eda689 (codex/354-gso) has the
   four-byte version 1. */
#define NX_INTERFACE_CAPABILITY_TCP_GSO             0x00004000

#define NX_TCP_GSO_VERSION                          2U
#define NX_TCP_GSO_FLAG_REPAIR                      0x01U
typedef struct NX_TCP_GSO_INFO_STRUCT
{
    UCHAR version;
    UCHAR flags;             /* 0, or NX_TCP_GSO_FLAG_REPAIR. */
    USHORT mss;              /* Wire TCP payload MSS, excluding options. */
    USHORT offset;           /* REPAIR: first payload byte; else 0. */
    USHORT length;           /* REPAIR: payload bytes; else 0. */
} NX_TCP_GSO_INFO;
#define NX_TCP_GSO_PACKET                           (NX_TCP_PACKET + (ULONG)sizeof(NX_TCP_GSO_INFO))

#endif /* NX_INTERFACE_CAPABILITY_TCP_GSO */

/*
 * The most segments one block may carry on this interface, 0 when it does
 * not take GSO at all (sana2_driver.c).  Settled at link-up, with the
 * capability bit; read it with the bit, not instead of it.
 */
UINT ami_sana2_gso_max_segs(NX_INTERFACE *interface_ptr);

#endif /* AMINETXDUO_TCPGSO_H */
