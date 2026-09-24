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

/* A 1518-byte frame occupies a 10-Mbit wire for 1214.4 us.  STP finishes the
   frame already in progress before ISR.RST is raised; leave measured margin. */
#define DP8390_STOP_WAIT_US  1500u

/* Overrun recovery waits for a frame in flight to finish; the DP8390D
   datasheet says wait at least 1.6 ms.  RST cannot be the done signal -- the
   overflow itself sets it on the reference core -- so this is unconditional. */
#define DP8390_OVW_STOP_WAIT_US  1600u

/* The overwrite wait's fallback floor, four reads per microsecond, used only
   when the beam clock is down and there is no measured per-line work to price
   the wait with.  Four is an assumed bus pace, not a verified 1.6 ms on every
   bus; with the beam up, floor_spins() returns the measured work per line
   instead.  Sized for 1.6 ms, unlike halt's 900 which was sized for its
   1.5 ms RST poll. */
#define DP8390_OVW_STOP_SPINS  (DP8390_OVW_STOP_WAIT_US * 4u)

VOID  dp8390_config(NetdevNic *nic);
LONG  dp8390_init(NetdevNic *nic);
VOID  dp8390_halt(NetdevNic *nic);
VOID  dp8390_reset(NetdevNic *nic);
VOID  dp8390_setfilter(NetdevNic *nic);
LONG  dp8390_tx(NetdevNic *nic, const UBYTE *frame, UWORD len);
BOOL  dp8390_intr(NetdevNic *nic);

#endif /* AMINETXDUO_DP8390_H */
