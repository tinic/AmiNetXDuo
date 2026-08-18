/*
 * A nonempty transmit ring is not evidence of a stall: under sustained load
 * one completion can be replaced before the next vertical blank sees it.  A
 * transmitter is wedged only when it stays nonempty without completing a
 * frame for the whole watchdog interval.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netdev_watchdog.h"

BOOL netdev_tx_watchdog_tick(UWORD *stall, ULONG *seen, BOOL online,
                            UWORD inuse, ULONG completed)
{
    if (!online || inuse == 0 || completed != *seen)
    {
        *stall = 0;
        *seen = completed;
        return FALSE;
    }

    if (++*stall < NETDEV_TX_STALL_BLANKS)
        return FALSE;

    *stall = 0;
    return TRUE;
}
