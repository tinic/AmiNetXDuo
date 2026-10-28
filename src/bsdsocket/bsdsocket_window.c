/*
 * bsdsocket.library, the receive window the packet pool can back.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_window.h"

ULONG ami_bsd_tcp_budget(ULONG pool_packets, ULONG payload)
{
    ULONG share = (pool_packets >= (ULONG)BSD_TCP_WINDOW_BIG_POOL)
                ? (ULONG)BSD_TCP_WINDOW_POOL_SHARE_BIG
                : (ULONG)BSD_TCP_WINDOW_POOL_SHARE;

    return (pool_packets / share) * payload;
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

ULONG ami_bsd_tcp_tx_queue_for(ULONG pool_packets, ULONG ceiling)
{
    ULONG depth = pool_packets / (ULONG)BSD_TCP_TX_POOL_SHARE;

    if (depth < (ULONG)BSD_TCP_TX_QUEUE_MIN)
        depth = (ULONG)BSD_TCP_TX_QUEUE_MIN;
    if (depth > ceiling)
        depth = ceiling;

    return depth;
}

ULONG ami_bsd_tcp_window_fit(ULONG window, ULONG hw_bytes, ULONG mss)
{
    ULONG frame;
    ULONG frames;
    ULONG fit;

    if (hw_bytes == 0UL || mss == 0UL)
        return window;

    /* A segment on the wire: MSS + 40 of TCP/IP + 14 of Ethernet, rounded
       up to the 256-byte page a DP8390 ring stores it in, plus that ring's
       4-byte header.  The GENET's 2 KB buffers round the same way. */
    frame  = (mss + 40UL + 14UL + 4UL + 255UL) & ~255UL;
    frames = hw_bytes / frame;
    if (frames < 2UL)
        frames = 2UL;
    fit = frames * mss;

    return (fit < window) ? fit : window;
}

ULONG ami_bsd_tcp_window_ring_share(ULONG hw_bytes, ULONG sharers)
{
    return (sharers > 1UL) ? hw_bytes / sharers : hw_bytes;
}

/* TCPGROWRTT, or the built-in line where the interface sets none. */
static ULONG bsd_grow_rtt(ULONG grow_rtt_ms)
{
    return (grow_rtt_ms != 0UL) ? grow_rtt_ms
                                : (ULONG)BSD_TCP_WINDOW_GROW_RTT_MS;
}

ULONG ami_bsd_tcp_window_settle(ULONG created, ULONG maximum, ULONG bps,
                                ULONG rtt_ms, ULONG grow_rtt_ms)
{
    /* A long path: the window is the rate, bursts are paced far away. */
    if (rtt_ms >= bsd_grow_rtt(grow_rtt_ms))
        return (maximum > created) ? maximum : created;

    /* A short path on a link that can put the whole window on the wire at
       once: the driver's ring and its posted reads back the burst now
       (sana2_internal.h, AMI_SANA2_RX_MAX_DEPTH; genet.c, the held pass), so
       the window is the rate here too -- up to the size that burst was
       measured at (bsdsocket_window.h, BSD_TCP_WINDOW_MAX_LAN). */
    if (bps >= (ULONG)BSD_TCP_WINDOW_FAST_BPS)
    {
        if (maximum > (ULONG)BSD_TCP_WINDOW_MAX_LAN)
            maximum = (ULONG)BSD_TCP_WINDOW_MAX_LAN;
        return (maximum > created) ? maximum : created;
    }

    return created;
}

BOOL ami_bsd_tcp_window_burst_bound(ULONG bps, ULONG rtt_ms,
                                    ULONG grow_rtt_ms)
{
    if (rtt_ms >= bsd_grow_rtt(grow_rtt_ms) &&
        bps >= (ULONG)BSD_TCP_WINDOW_FAST_BPS)
        return FALSE;

    return TRUE;
}

BOOL ami_bsd_tcp_window_long_path(ULONG rtt_ms, ULONG grow_rtt_ms)
{
    return (rtt_ms >= bsd_grow_rtt(grow_rtt_ms)) ? TRUE : FALSE;
}

/* TCPWANWINDOW within the window settled for the socket, never below
   `floor`. */
static ULONG bsd_wan_window(ULONG want, ULONG wan_window, ULONG floor)
{
    ULONG wan = (wan_window < want) ? wan_window : want;

    if (floor > want)
        floor = want;

    return (wan > floor) ? wan : floor;
}

ULONG ami_bsd_tcp_window_receive_bound(ULONG want, ULONG hw_bytes, ULONG mss,
                                       ULONG bps, ULONG rtt_ms,
                                       ULONG grow_rtt_ms, ULONG wan_window,
                                       BOOL wan_fitted)
{
    ULONG fit;

    /* A long path on a gigabit card: the card is not consulted, so
       TCPWANWINDOW, when set, is the window -- floored at two segments, the
       fit's own floor.  #354: genet kept 802,816 under TCPWANWINDOW=300000. */
    if (!ami_bsd_tcp_window_burst_bound(bps, rtt_ms, grow_rtt_ms))
        return (wan_window != 0UL)
               ? bsd_wan_window(want, wan_window, 2UL * mss)
               : want;

    fit = ami_bsd_tcp_window_fit(want, hw_bytes, mss);

    /* A long path: an upstream link spaces the data, so the settled window
       stands until the card has overrun under it; TCPWANWINDOW, when set,
       is the window instead -- never less than the fit, never more than the
       window settled for the socket. */
    if (rtt_ms >= bsd_grow_rtt(grow_rtt_ms))
    {
        if (wan_window == 0UL)
            return wan_fitted ? fit : want;
        return bsd_wan_window(want, wan_window, fit);
    }

    return fit;
}

ULONG ami_bsd_tcp_window_buffer(ULONG want, ULONG bound)
{
    if (bound >= want)
        return bound;           /* nothing was cut */
    if (bound > (want >> 1))
        return want;            /* twice the cut would pass the window */

    return bound << 1;
}
