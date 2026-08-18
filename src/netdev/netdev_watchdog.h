/*
 * anxnet.device transmit-watchdog state, separated from the vertical-blank
 * interrupt so its progress rule can be proved on the host.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETDEV_WATCHDOG_H
#define AMINETXDUO_NETDEV_WATCHDOG_H

#include <exec/types.h>

#define NETDEV_TX_STALL_BLANKS  120     /* about 2 s at PAL or NTSC */

BOOL netdev_tx_watchdog_tick(UWORD *stall, ULONG *seen, BOOL online,
                            UWORD inuse, ULONG completed);

#endif /* AMINETXDUO_NETDEV_WATCHDOG_H */
