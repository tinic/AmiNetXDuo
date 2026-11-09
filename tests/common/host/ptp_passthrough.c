/*
 * AmiNetXDuo host tests: point-to-point support with no point-to-point
 * interface in the machine.  For a test that links a file which consults
 * src/netstack/netstack_ptp.c or the SANA-II shim's point-to-point
 * accessors but models neither: every gateway is its own next hop, and no
 * interface has a peer.
 *
 * SPDX-License-Identifier: MIT
 */

#ifdef AMINETXDUO_PTP

#include "aminetxduo/ptp.h"

ULONG ami_ptp_peer_of(const NX_INTERFACE *ifp)
{
    (void)ifp;
    return 0UL;
}

ULONG ami_ptp_next_hop(NX_IP *ip, ULONG gateway)
{
    (void)ip;
    return gateway;
}

ULONG ami_ptp_shown_hop(NX_IP *ip, ULONG next_hop)
{
    (void)ip;
    return next_hop;
}

VOID ami_ptp_peer_route(NX_IP *ip, UINT index)
{
    (void)ip;
    (void)index;
}

ULONG ami_ptp_default_hop(ULONG gateway, ULONG peer, ULONG local)
{
    (void)peer;
    (void)local;
    return gateway;
}

UINT ami_sana2_ptp(const AmiSana2If *iface)
{
    (void)iface;
    return NX_FALSE;
}

ULONG ami_sana2_ptp_peer(const AmiSana2If *iface)
{
    (void)iface;
    return 0UL;
}

VOID ami_sana2_set_ptp_peer(AmiSana2If *iface, ULONG peer)
{
    (void)iface;
    (void)peer;
}

ULONG ami_sana2_ptp_routed(const AmiSana2If *iface)
{
    (void)iface;
    return 0UL;
}

VOID ami_sana2_set_ptp_routed(AmiSana2If *iface, ULONG peer)
{
    (void)iface;
    (void)peer;
}

#ifdef AMINETXDUO_IPV6
/* No IPv6 peer either: every router is shown as NetX Duo holds it. */
UINT anx6_ptp_peer_get(UINT interface_index, ULONG *peer)
{
    (void)interface_index;
    peer[0] = peer[1] = peer[2] = peer[3] = 0UL;
    return NX_FALSE;
}

VOID ami_ptp_ipv6_shown_hop(NX_IP *ip, const NX_INTERFACE *ifp,
                            const ULONG hop[4], ULONG out[4])
{
    (void)ip;
    (void)ifp;
    out[0] = hop[0];
    out[1] = hop[1];
    out[2] = hop[2];
    out[3] = hop[3];
}

UINT anx6_ptp_route_get(UINT n, ULONG *prefix, UINT *length,
                        UINT *interface_index)
{
    (void)n;
    (void)prefix;
    (void)length;
    (void)interface_index;
    return NX_FALSE;
}

UINT netstack_ipv6_peer_set(unsigned short index, const ULONG peer[4])
{
    (void)index;
    (void)peer;
    return NX_NOT_SUPPORTED;
}
#endif

#else
typedef int ami_ptp_passthrough_unused;
#endif /* AMINETXDUO_PTP */
