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
 * Include after nx_api.h.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AMINETXDUO_TCPGSO_H
#define AMINETXDUO_TCPGSO_H

/* The contract itself is the vendor's, nx_api.h (tinic/netxduo b665b935
   on): this header only names what sana2 adds to it. */
#ifndef NX_INTERFACE_CAPABILITY_TCP_GSO
#error "aminetxduo/tcpgso.h needs nx_api.h with TCP GSO (record version 2)"
#endif

/*
 * The most segments one block may carry on this interface, 0 when it does
 * not take GSO at all (sana2_driver.c).  Settled at link-up, with the
 * capability bit; read it with the bit, not instead of it.
 */
UINT ami_sana2_gso_max_segs(NX_INTERFACE *interface_ptr);

#endif /* AMINETXDUO_TCPGSO_H */
