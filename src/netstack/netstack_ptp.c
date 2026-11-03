/*
 * AmiNetXDuo, point-to-point interfaces in NetX Duo's terms.
 *
 * A point-to-point interface is its own address with a 32-bit mask, as NetX
 * Duo's PPP addon configures one (addons/ppp/nx_ppp.c).  NetX Duo then finds
 * no network the peer is on, and refuses it as a gateway or a next hop: both
 * must sit inside an interface's network.  The PPP addon's answer is ours.
 * The next hop through such an interface is the interface's own address,
 * which is on its network, and the driver ignores the next hop, because
 * every datagram it is given goes to the one peer.
 *
 * So a gateway that names a peer is handed to NetX Duo as that interface's
 * address (ami_ptp_next_hop), and read back as the peer (ami_ptp_shown_hop),
 * and the peer itself gets a host route (ami_ptp_peer_route).
 *
 * SPDX-License-Identifier: MIT
 */

#include "aminetxduo/ptp.h"

#define AMI_PTP_HOST_MASK   0xFFFFFFFFUL

static const AmiSana2If *ami_ptp_link(const NX_INTERFACE *ifp)
{
    const AmiSana2If *sana;

    if (ifp == NULL || ifp->nx_interface_valid == 0)
        return NULL;

    sana = (const AmiSana2If *)ifp->nx_interface_additional_link_info;
    return ami_sana2_ptp(sana) ? sana : NULL;
}

ULONG ami_ptp_peer_of(const NX_INTERFACE *ifp)
{
    const AmiSana2If *sana = ami_ptp_link(ifp);

    return (sana != NULL) ? ami_sana2_ptp_peer(sana) : 0UL;
}

ULONG ami_ptp_next_hop(NX_IP *ip, ULONG gateway)
{
    UINT i;

    if (ip == NULL || gateway == 0UL)
        return gateway;

    for (i = 0; i < (UINT)NX_MAX_PHYSICAL_INTERFACES; i++)
    {
        const NX_INTERFACE *ifp = &ip->nx_ip_interface[i];

        if (ifp->nx_interface_ip_address != 0UL &&
            ami_ptp_peer_of(ifp) == gateway)
            return ifp->nx_interface_ip_address;
    }

    return gateway;
}

ULONG ami_ptp_shown_hop(NX_IP *ip, ULONG next_hop)
{
    UINT i;

    if (ip == NULL || next_hop == 0UL)
        return next_hop;

    for (i = 0; i < (UINT)NX_MAX_PHYSICAL_INTERFACES; i++)
    {
        const NX_INTERFACE *ifp  = &ip->nx_ip_interface[i];
        ULONG               peer = ami_ptp_peer_of(ifp);

        if (peer != 0UL && ifp->nx_interface_ip_address == next_hop)
            return peer;
    }

    return next_hop;
}

/*
 * The host route to the peer, made again whenever the address or the peer
 * changes.  A peer inside the interface's own network is on it already and
 * needs none, and one made before goes.  NetX Duo finds an existing entry by
 * destination and mask and updates its next hop, so a changed address needs
 * no delete first.
 */
VOID ami_ptp_peer_route(NX_IP *ip, UINT index)
{
#ifdef NX_ENABLE_IP_STATIC_ROUTING
    NX_INTERFACE *ifp;
    AmiSana2If   *sana;
    ULONG         peer;
    ULONG         routed;
    ULONG         local;
    UINT          need;

    if (ip == NULL || index >= (UINT)NX_MAX_PHYSICAL_INTERFACES)
        return;

    ifp  = &ip->nx_ip_interface[index];
    sana = (AmiSana2If *)ami_ptp_link(ifp);
    if (sana == NULL)
        return;

    peer   = ami_sana2_ptp_peer(sana);
    routed = ami_sana2_ptp_routed(sana);
    local  = ifp->nx_interface_ip_address;
    need   = (peer != 0UL && local != 0UL &&
              (peer & ifp->nx_interface_ip_network_mask) !=
                  ifp->nx_interface_ip_network) ? NX_TRUE : NX_FALSE;

    /* The old host route goes when the peer moved or no longer needs one. */
    if (routed != 0UL && (routed != peer || !need))
        (VOID)nx_ip_static_route_delete(ip, routed, AMI_PTP_HOST_MASK);
    ami_sana2_set_ptp_routed(sana, 0UL);

    /* Every next hop through the link is the interface's own address
       (AMI_PTP_NEXT_HOP), so a new address takes them all with it: the
       default gateway and each static route NetX Duo has on this
       interface.  The caller holds the stack, as for the route add. */
    if (local != 0UL)
    {
        ULONG r;

        if (ip->nx_ip_gateway_interface == ifp)
            ip->nx_ip_gateway_address = local;
        for (r = 0; r < ip->nx_ip_routing_table_entry_count; r++)
            if (ip->nx_ip_routing_table[r].nx_ip_routing_entry_ip_interface == ifp)
                ip->nx_ip_routing_table[r].nx_ip_routing_next_hop_address = local;
    }

    if (!need)
        return;

    /* A full table leaves the peer reachable only by a default route
       through the link; ShowNetStatus shows the route missing. */
    if (nx_ip_static_route_add(ip, peer, AMI_PTP_HOST_MASK, local) ==
        NX_SUCCESS)
        ami_sana2_set_ptp_routed(sana, peer);
#else
    (VOID)ip;
    (VOID)index;
#endif
}
