/*
 * AddNetRoute/DeleteNetRoute: which NETSTATUS_ROUTES row is a route added by
 * hand, kept apart so the host can test it.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AMINETXDUO_ROUTEMATCH_H
#define AMINETXDUO_ROUTEMATCH_H

#include <aminetxduo/netstatus.h>

/*
 * The static row for dest, or -1.  With a mask (have_mask), only the row with
 * exactly that destination and mask: NetX reports success deleting from an
 * empty table, so nothing else may reach it (F-133).  Without, the first
 * static row that carries dest; a /0 row only for 0.0.0.0 itself, so it never
 * stands in for a more specific route.  The live default gateway is listed
 * without NETSTATUS_RT_STATIC and never matches.
 */
static inline LONG route_match_static(const NetStatusRoute *r, LONG n,
                                      ULONG dest, ULONG mask, BOOL have_mask)
{
    LONG i;

    for (i = 0; i < n; i++)
    {
        if (!(r[i].nsr_Flags & NETSTATUS_RT_STATIC))
            continue;

        if (have_mask || dest == 0)
        {
            ULONG want = have_mask ? mask : 0;

            if (r[i].nsr_NetMask == want &&
                r[i].nsr_Destination == (dest & want))
                return i;
        }
    }

    if (have_mask)
        return -1;

    for (i = 0; i < n; i++)
    {
        if (r[i].nsr_NetMask == 0)
            continue;
        if (!(r[i].nsr_Flags & NETSTATUS_RT_STATIC))
            continue;
        if ((dest & r[i].nsr_NetMask) != r[i].nsr_Destination)
            continue;
        return i;
    }

    return -1;
}

/*
 * ConfigureNetInterface: an IPv6 default router on interface `index` in the
 * NETSTATUS_ROUTES6 rows r[0..n), n as the query returned it.  `match` TRUE
 * finds `want`, FALSE finds any other; the row's next hop goes to out.
 * 1 found, 0 none, and -1 when the query failed (n < 0): a table that could
 * not be read is not an empty one (F-145).
 */
static inline LONG route6_find_router(const NetStatusRoute6 *r, LONG n,
                                      LONG max, LONG index, const ULONG *want,
                                      BOOL match, ULONG out[4])
{
    LONG i;

    if (n < 0)
        return -1;

    for (i = 0; i < n && i < max; i++)
    {
        BOOL same;

        if (!(r[i].nsr6_Flags & NETSTATUS_RT6_GATEWAY))
            continue;
#ifdef AMINETXDUO_PTP
        if (r[i].nsr6_PrefixLength != 0UL)
            continue;                   /* a prefix through a peer */
#endif
        if ((LONG)r[i].nsr6_Interface != index)
            continue;

        same = (BOOL)(want != NULL &&
                      r[i].nsr6_NextHop[0] == want[0] &&
                      r[i].nsr6_NextHop[1] == want[1] &&
                      r[i].nsr6_NextHop[2] == want[2] &&
                      r[i].nsr6_NextHop[3] == want[3]);
        if (match ? !same : same)
            continue;

        if (out != NULL)
        {
            out[0] = r[i].nsr6_NextHop[0];
            out[1] = r[i].nsr6_NextHop[1];
            out[2] = r[i].nsr6_NextHop[2];
            out[3] = r[i].nsr6_NextHop[3];
        }
        return 1;
    }

    return 0;
}

/*
 * Whether the running library has IPv6, from a NETSTATUS_SYSTEM query that
 * returned n rows: 1 yes, 0 built without it, -1 the query failed and it
 * cannot be told (F-147).
 */
static inline LONG route6_stack_ipv6(LONG n, ULONG sys_flags)
{
    if (n <= 0)
        return -1;

    return (sys_flags & NETSTATUS_SYS_IPV6) ? 1 : 0;
}

#endif
