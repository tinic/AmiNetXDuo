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

static ULONG bsd_window_share(ULONG pool_packets, ULONG payload,
                              ULONG consumers, ULONG cap)
{
    ULONG window;

    window = ami_bsd_tcp_budget(pool_packets, payload) / (consumers + 1UL);

    if (window > cap)
        window = cap;
    if (window < (ULONG)BSD_TCP_WINDOW)
        window = (ULONG)BSD_TCP_WINDOW;

    return window;
}

ULONG ami_bsd_tcp_window_for(ULONG pool_packets, ULONG payload,
                             ULONG consumers)
{
    ULONG cap = (ULONG)BSD_TCP_WINDOW_LAN;

    /* The LAN window is under the wire's ceiling in every build that has
       one; said explicitly so that a build with a smaller ceiling stays
       right. */
    if (cap > (ULONG)BSD_TCP_WINDOW_CEILING)
        cap = (ULONG)BSD_TCP_WINDOW_CEILING;

    return bsd_window_share(pool_packets, payload, consumers, cap);
}

ULONG ami_bsd_tcp_window_max_for(ULONG pool_packets, ULONG payload,
                                 ULONG consumers)
{
    ULONG max = bsd_window_share(pool_packets, payload, consumers,
                                 (ULONG)BSD_TCP_WINDOW_CEILING);
    ULONG lan = ami_bsd_tcp_window_for(pool_packets, payload, consumers);

    return (max < lan) ? lan : max;
}

ULONG ami_bsd_tcp_window_settle(ULONG created, ULONG maximum, ULONG bps,
                                ULONG rtt_ms)
{
    /* A long path: the window is the rate, bursts are paced far away. */
    if (rtt_ms >= (ULONG)BSD_TCP_WINDOW_GROW_RTT_MS)
        return (maximum > created) ? maximum : created;

    /* A short path on a link that can put the whole window on the wire at
       once: under the ring's knee, whatever the budget offered. */
    if (bps >= (ULONG)BSD_TCP_WINDOW_FAST_BPS &&
        created > (ULONG)BSD_TCP_WINDOW_FAST)
        return (ULONG)BSD_TCP_WINDOW_FAST;

    return created;
}
