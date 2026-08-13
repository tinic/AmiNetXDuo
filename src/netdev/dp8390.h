/*
 * anxnet.device, the DP8390 core's own surface.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_DP8390_H
#define AMINETXDUO_DP8390_H

#include "netdev_nic.h"

#define DP8390_TX_BUSY      1   /* every transmit buffer is in flight */
#define DP8390_TX_OFFLINE   2
#define DP8390_TX_FAILED    3

VOID  dp8390_config(NetdevNic *nic);
LONG  dp8390_init(NetdevNic *nic);
VOID  dp8390_halt(NetdevNic *nic);
VOID  dp8390_reset(NetdevNic *nic);
VOID  dp8390_setfilter(NetdevNic *nic);
LONG  dp8390_tx(NetdevNic *nic, const UBYTE *frame, UWORD len);
BOOL  dp8390_intr(NetdevNic *nic);

#endif /* AMINETXDUO_DP8390_H */
