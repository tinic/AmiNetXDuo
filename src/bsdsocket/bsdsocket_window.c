/*
 * bsdsocket.library, the receive window the packet pool can back.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_window.h"

ULONG ami_bsd_tcp_budget(ULONG pool_packets, ULONG payload)
{
    return (pool_packets / (ULONG)BSD_TCP_WINDOW_POOL_SHARE) * payload;
}

ULONG ami_bsd_tcp_window_for(ULONG pool_packets, ULONG payload,
                             ULONG consumers)
{
    ULONG window;

    window = ami_bsd_tcp_budget(pool_packets, payload) / (consumers + 1UL);

    if (window > (ULONG)BSD_TCP_WINDOW_CEILING)
        window = (ULONG)BSD_TCP_WINDOW_CEILING;
    if (window < (ULONG)BSD_TCP_WINDOW)
        window = (ULONG)BSD_TCP_WINDOW;

    return window;
}
