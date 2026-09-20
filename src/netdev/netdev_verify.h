/*
 * Stateless receive-checksum verification shared by the GENET and classic
 * direct-receive paths.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETDEV_VERIFY_H
#define AMINETXDUO_NETDEV_VERIFY_H

#include <exec/types.h>

/* `sum` is the one's-complement running sum of the complete IP packet. */
UBYTE netdev_rx_verify4(const UBYTE *ip, UWORD plen, ULONG sum);
UBYTE netdev_rx_verify6(const UBYTE *ip, UWORD plen, ULONG sum);
UBYTE netdev_rx_verify(const UBYTE *ip, UWORD plen, ULONG sum);

/* Turn a GEM-style hardware verdict (2 = IP+TCP, 3 = IP+UDP) into the
 * published VERIFIED promise, after checking the structural restrictions
 * that promise makes but without recomputing either checksum. */
UBYTE netdev_rx_trust4(const UBYTE *ip, UWORD plen, UBYTE verdict);

/* Validate the full Ethernet/IPv4 frame selected for negotiated TX checksum and
 * return the byte offset of the TCP or UDP checksum which a full-offload
 * device must clear before transmission. */
UBYTE netdev_tx_csum4(const UBYTE *frame, UWORD len, UBYTE supported,
                      UWORD *checksum_offset);

#endif /* AMINETXDUO_NETDEV_VERIFY_H */
