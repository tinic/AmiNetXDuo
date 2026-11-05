/*
 * DeleteNetRoute's lookup of a route added by hand (F-133): bare 0.0.0.0
 * finds a static /0, an explicit mask needs an exact static row, a static /0
 * never stands in for a more specific route, and the live default gateway is
 * never a match.
 *
 * SPDX-License-Identifier: MIT
 */
#include "routematch.h"

#include <stdio.h>

static int checks, failures;

#define CHECK(c) do { checks++; if (!(c)) { failures++; \
    printf("  FAIL line %d: %s\n", __LINE__, #c); } } while (0)

#define IP(a, b, c, d)  (((ULONG)(a) << 24) | ((ULONG)(b) << 16) | \
                         ((ULONG)(c) << 8) | (ULONG)(d))

#define LIVE_GW     { 0, 0, IP(192,168,1,1), NETSTATUS_RT_UP | NETSTATUS_RT_GATEWAY, 0 }
#define IFACE       { IP(192,168,1,0), IP(255,255,255,0), 0, NETSTATUS_RT_UP, 0 }
#define STATIC_0    { 0, 0, IP(192,168,1,2), \
                      NETSTATUS_RT_UP | NETSTATUS_RT_STATIC | NETSTATUS_RT_GATEWAY, 0 }
#define STATIC_77   { IP(192,168,77,0), IP(255,255,255,0), IP(192,168,1,3), \
                      NETSTATUS_RT_UP | NETSTATUS_RT_STATIC | NETSTATUS_RT_GATEWAY, 0 }

int main(void)
{
    {   /* A static /0 listed before a more specific static route. */
        const NetStatusRoute r[] = { IFACE, STATIC_0, STATIC_77, LIVE_GW };
        const LONG n = 4;

        CHECK(route_match_static(r, n, 0, 0, FALSE) == 1);          /* bare 0.0.0.0 */
        CHECK(route_match_static(r, n, 0, 0, TRUE) == 1);           /* 0.0.0.0/0 */
        CHECK(route_match_static(r, n, IP(192,168,77,5), 0, FALSE) == 2);
        CHECK(route_match_static(r, n, IP(192,168,77,0), 0, FALSE) == 2);
        CHECK(route_match_static(r, n, IP(10,1,2,3), 0, FALSE) == -1);
        CHECK(route_match_static(r, n, IP(192,168,1,9), 0, FALSE) == -1);
        CHECK(route_match_static(r, n, IP(192,168,77,0), IP(255,255,255,0), TRUE) == 2);
        CHECK(route_match_static(r, n, IP(192,168,77,5), IP(255,255,255,0), TRUE) == 2);
        CHECK(route_match_static(r, n, IP(192,168,77,0), IP(255,255,255,128), TRUE) == -1);
        CHECK(route_match_static(r, n, IP(192,168,1,0), IP(255,255,255,0), TRUE) == -1);
    }
    {   /* The same, the static /0 listed last. */
        const NetStatusRoute r[] = { LIVE_GW, STATIC_77, IFACE, STATIC_0 };
        const LONG n = 4;

        CHECK(route_match_static(r, n, 0, 0, FALSE) == 3);
        CHECK(route_match_static(r, n, 0, 0, TRUE) == 3);
        CHECK(route_match_static(r, n, IP(192,168,77,5), 0, FALSE) == 1);
    }
    {   /* Nothing added by hand: only the live gateway and the interface.
           NetX reports success deleting from this empty table. */
        const NetStatusRoute r[] = { IFACE, LIVE_GW };
        const LONG n = 2;

        CHECK(route_match_static(r, n, 0, 0, FALSE) == -1);
        CHECK(route_match_static(r, n, 0, 0, TRUE) == -1);
        CHECK(route_match_static(r, n, IP(192,168,77,0), IP(255,255,255,0), TRUE) == -1);
        CHECK(route_match_static(r, n, IP(192,168,1,0), IP(255,255,255,0), TRUE) == -1);
    }
    {   /* No answer at all. */
        CHECK(route_match_static(NULL, 0, 0, 0, TRUE) == -1);
        CHECK(route_match_static(NULL, 0, IP(192,168,77,5), 0, FALSE) == -1);
    }

    {   /* F-145: ConfigureNetInterface's IPv6 default router lookup. */
        static const ULONG gw_a[4] = { 0xFE800000UL, 0, 0, 1 };
        static const ULONG gw_b[4] = { 0xFE800000UL, 0, 0, 2 };
        NetStatusRoute6 r6[3];
        ULONG           out[4] = { 0, 0, 0, 0 };
        LONG            k;

        for (k = 0; k < 3; k++)
        {
            LONG w;
            for (w = 0; w < (LONG)sizeof(r6[k]); w++)
                ((UBYTE *)&r6[k])[w] = 0;
        }
        r6[0].nsr6_Flags = 0;                           /* a prefix, no gateway */
        r6[1].nsr6_Flags = NETSTATUS_RT6_GATEWAY;       /* gw_a on interface 1 */
        r6[1].nsr6_Interface = 1;
        for (k = 0; k < 4; k++) r6[1].nsr6_NextHop[k] = gw_a[k];
        r6[2].nsr6_Flags = NETSTATUS_RT6_GATEWAY;       /* gw_b on interface 2 */
        r6[2].nsr6_Interface = 2;
        for (k = 0; k < 4; k++) r6[2].nsr6_NextHop[k] = gw_b[k];

        /* A query that failed is not an empty table. */
        CHECK(route6_find_router(r6, -1, 3, 1, gw_a, TRUE, NULL) == -1);
        CHECK(route6_find_router(r6, -1, 3, 1, NULL, FALSE, out) == -1);

        CHECK(route6_find_router(r6, 0, 3, 1, gw_a, TRUE, NULL) == 0);
        CHECK(route6_find_router(r6, 3, 3, 1, gw_a, TRUE, NULL) == 1);
        CHECK(route6_find_router(r6, 3, 3, 1, gw_b, TRUE, NULL) == 0);   /* other iface */
        CHECK(route6_find_router(r6, 3, 3, 1, gw_a, FALSE, out) == 0);   /* keep gw_a */
        CHECK(route6_find_router(r6, 3, 3, 1, NULL, FALSE, out) == 1 && out[3] == 1);
        CHECK(route6_find_router(r6, 3, 2, 2, NULL, FALSE, out) == 0);   /* past max */
#ifdef AMINETXDUO_PTP
        /* A prefix through a point-to-point peer is not a default router. */
        r6[1].nsr6_PrefixLength = 3;
        CHECK(route6_find_router(r6, 3, 3, 1, gw_a, TRUE, NULL) == 0);
        r6[1].nsr6_PrefixLength = 0;
#endif
    }

    {   /* F-147: IPv6 in the running library, or no answer. */
        CHECK(route6_stack_ipv6(1, NETSTATUS_SYS_IPV6) == 1);
        CHECK(route6_stack_ipv6(1, 0) == 0);
        CHECK(route6_stack_ipv6(0, 0) == -1);      /* no row */
        CHECK(route6_stack_ipv6(-1, 0) == -1);     /* the query failed */
    }

    printf("RESULT route_match checks=%d failures=%d\n", checks, failures);
    return failures != 0;
}
