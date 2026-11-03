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

#else
typedef int ami_ptp_passthrough_unused;
#endif /* AMINETXDUO_PTP */
