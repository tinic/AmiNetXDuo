/*
 * anxnet.device's exact multicast-address table.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETDEV_MCAST_H
#define AMINETXDUO_NETDEV_MCAST_H

#include "netdev_nic.h"

#define NETDEV_MCAST_MAX    32      /* the hash is 64 bits, so more is idle */

typedef struct NetdevMcast
{
    UBYTE   addr[NETDEV_ADDR_LEN];
    UWORD   refs;
} NetdevMcast;

BOOL netdev_mcast_add(NetdevMcast *table, const UBYTE *addr);
BOOL netdev_mcast_del(NetdevMcast *table, const UBYTE *addr);

/* TRUE when an inclusive range is too large for the exact table.  `count` is
   meaningful only on FALSE. */
BOOL netdev_mcast_range_wide(const UBYTE *lo, const UBYTE *hi, ULONG *count);

/* Apply a small inclusive range as one transaction.  On FALSE the table is
   untouched: an add lacked rows, or a delete named an address not held. */
BOOL netdev_mcast_range_apply(NetdevMcast *table, const UBYTE *lo,
                              ULONG count, BOOL add);

#endif /* AMINETXDUO_NETDEV_MCAST_H */
