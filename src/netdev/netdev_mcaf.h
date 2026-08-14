/*
 * anxnet.device, the multicast hash.
 *
 * Split out of the chip core so it builds and runs on the host: it is pure
 * arithmetic, it decides whether an IPv6 solicited-node frame reaches the
 * machine, and it is the only part of a network card driver that can be
 * tested without a network card.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETDEV_MCAF_H
#define AMINETXDUO_NETDEV_MCAF_H

#include <exec/types.h>

#define NETDEV_ADDR_LEN     6

ULONG netdev_ether_crc32_be(const UBYTE *addr, UWORD len);
VOID  netdev_mar_clear(UBYTE mar[8]);
VOID  netdev_mar_set(UBYTE mar[8], const UBYTE addr[NETDEV_ADDR_LEN]);
VOID  netdev_mar_all(UBYTE mar[8]);

#endif /* AMINETXDUO_NETDEV_MCAF_H */
