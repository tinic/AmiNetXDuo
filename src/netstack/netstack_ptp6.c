/*
 * AmiNetXDuo, IPv6 on a point-to-point interface in NetX Duo's terms.
 * Compiled with AMINETXDUO_PTP and AMINETXDUO_IPV6 both on.
 *
 * SPDX-License-Identifier: MIT
 */

#include "aminetxduo/ptp.h"
#include "aminetxduo/nxstatus.h"

#include "../ipv6/ipv6_srcsel.h"

/*
 * An address on the link with no duplicate address detection.  Held across
 * the set, so the IP thread never sees the address with NetX Duo's own
 * count.  The caller does not hold nx_ip_protection.
 */
UINT ami_ptp_ipv6_address_set(NX_IP *ip, UINT index, NXD_ADDRESS *addr,
                              ULONG prefix)
{
    UINT slot = 0;
    UINT status;

    AMI_NX_ONLY_SUCCESS(tx_mutex_get(&ip->nx_ip_protection, TX_WAIT_FOREVER));

    status = nxd_ipv6_address_set(ip, index, addr, prefix, &slot);
    if (status == NX_SUCCESS)
        ip->nx_ipv6_address[slot].nxd_ipv6_address_DupAddrDetectTransmit = 0;

    AMI_NX_ONLY_SUCCESS(tx_mutex_put(&ip->nx_ip_protection));

    return status;
}

/*
 * IPv6 on a point-to-point link, as RFC 4861 section 2.2 and RFC 4862 have
 * it for a link without link-layer addresses.  The driver has no MAC for
 * nxd_ipv6_address_set() to make an identifier from, so the identifier is
 * random, drawn once per bring-up, with the u bit clear (RFC 4291 2.5.1) --
 * what Linux gives a tun device.  Address mapping is off on the link
 * (sana2_driver.c), so _nx_ipv6_packet_send() hands every datagram straight
 * to the driver and no solicitation for address resolution goes out.  RFC
 * 4862 5.1 lets DupAddrDetectTransmits be zero: _nx_icmpv6_perform_DAD()
 * then promotes the address on its next pass and sends nothing.  Nor are
 * routers solicited: stateless autoconfiguration is off.
 */
UINT ami_ptp_ipv6_start(NX_IP *ip, UINT index)
{
    NXD_IPV6_ADDRESS *a;
    NXD_ADDRESS       ll;
    UINT              status;

    if (ip == NULL || index >= (UINT)NX_MAX_PHYSICAL_INTERFACES)
        return NX_INVALID_INTERFACE;

    /* Held across the check and the set; the set takes it again. */
    AMI_NX_ONLY_SUCCESS(tx_mutex_get(&ip->nx_ip_protection, TX_WAIT_FOREVER));

    /* A link-local already: brought up before, and done then. */
    for (a = ip->nx_ip_interface[index].nxd_interface_ipv6_address_list_head;
         a != NX_NULL; a = a->nxd_ipv6_address_next)
    {
        if ((a->nxd_ipv6_address[0] & 0xFFC00000UL) == 0xFE800000UL)
        {
            AMI_NX_ONLY_SUCCESS(tx_mutex_put(&ip->nx_ip_protection));
            return NX_DUPLICATED_ENTRY;
        }
    }

    ll.nxd_ip_version       = NX_IP_VERSION_V6;
    ll.nxd_ip_address.v6[0] = 0xFE800000UL;
    ll.nxd_ip_address.v6[1] = 0UL;
    ll.nxd_ip_address.v6[2] = ((((ULONG)NX_RAND() << 16) ^ (ULONG)NX_RAND()) &
                               0xFDFFFFFFUL);
    ll.nxd_ip_address.v6[3] = ((ULONG)NX_RAND() << 16) ^ (ULONG)NX_RAND();

    status = ami_ptp_ipv6_address_set(ip, index, &ll, 10);

    AMI_NX_ONLY_SUCCESS(tx_mutex_put(&ip->nx_ip_protection));

    if (status == NX_SUCCESS)
        AMI_NX_CLEANUP(
            nxd_ipv6_stateless_address_autoconfig_disable(ip, index));

    return status;
}

/*
 * The peer: reached through the link whatever ADDRESS6's prefix covers
 * (src/ipv6/ipv6_srcsel.c, 2b), and the next hop a default route through
 * the link names.  NULL or :: for none.
 */
VOID ami_ptp_ipv6_peer_set(UINT index, const ULONG *peer)
{
    anx6_ptp_peer_set(index, peer);
}

/*
 * The point-to-point interface whose peer gw is, NX_MAX_PHYSICAL_INTERFACES
 * for none.  For an add the link must be up, and a link-local peer must be
 * on interface `zone`.
 */
static UINT ami_ptp6_peer_index(NX_IP *ip, const ULONG gw[4], UINT zone,
                                UINT adding)
{
    ULONG peer[4];
    UINT  i;

    for (i = 0; i < (UINT)NX_MAX_PHYSICAL_INTERFACES; i++)
    {
        if (!anx6_ptp_peer_get(i, peer) ||
            peer[0] != gw[0] || peer[1] != gw[1] ||
            peer[2] != gw[2] || peer[3] != gw[3])
            continue;

        if (adding &&
            (ip->nx_ip_interface[i].nx_interface_link_up == NX_FALSE ||
             ((gw[0] & 0xFFC00000UL) == 0xFE800000UL && i != zone)))
            continue;

        break;
    }

    return i;
}

/*
 * A prefix through the peer of a point-to-point link: the link carries it,
 * and the peer is the next hop without being asked.  Through any other
 * gateway it is NX_NOT_SUPPORTED, as before: NetX Duo has no IPv6 route
 * table to resolve one on a LAN.  The caller holds nx_ip_protection.
 */
UINT ami_ptp_ipv6_route_add(NX_IP *ip, const ULONG dest[4], ULONG prefix_len,
                            const ULONG gw[4], UINT zone)
{
    UINT i = ami_ptp6_peer_index(ip, gw, zone, NX_TRUE);

    if (i >= (UINT)NX_MAX_PHYSICAL_INTERFACES)
        return NX_NOT_SUPPORTED;

    return anx6_ptp_route_add(dest, (UINT)prefix_len, i);
}

UINT ami_ptp_ipv6_route_delete(NX_IP *ip, const ULONG dest[4],
                               ULONG prefix_len, const ULONG gw[4])
{
    UINT i = ami_ptp6_peer_index(ip, gw, 0, NX_FALSE);

    if (i >= (UINT)NX_MAX_PHYSICAL_INTERFACES)
        return NX_ENTRY_NOT_FOUND;

    return anx6_ptp_route_delete(dest, (UINT)prefix_len, i);
}

static const ULONG *ami_ptp6_link_local(const NX_INTERFACE *ifp)
{
    const NXD_IPV6_ADDRESS *a;

    for (a = ifp->nxd_interface_ipv6_address_list_head; a != NX_NULL;
         a = a->nxd_ipv6_address_next)
        if ((a->nxd_ipv6_address[0] & 0xFFC00000UL) == 0xFE800000UL)
            return a->nxd_ipv6_address;

    return NX_NULL;
}

/*
 * The IPv4 answer (netstack_ptp.c) again.  NetX Duo takes a global router
 * only inside a prefix of the interface, and a /128 covers no peer.  The
 * router a default route through the link is given is the link's own
 * link-local, which is unicast and so taken as it is; the driver ignores
 * the next hop.  A peer is unique to its link, so the reverse is exact.
 */
VOID ami_ptp_ipv6_next_hop(NX_IP *ip, const ULONG gw[4], ULONG out[4])
{
    const ULONG *hop = gw;
    ULONG        peer[4];
    UINT         i;

    for (i = 0; i < (UINT)NX_MAX_PHYSICAL_INTERFACES; i++)
    {
        const ULONG *ll;

        if (!anx6_ptp_peer_get(i, peer) ||
            peer[0] != gw[0] || peer[1] != gw[1] ||
            peer[2] != gw[2] || peer[3] != gw[3])
            continue;

        ll = ami_ptp6_link_local(&ip->nx_ip_interface[i]);
        if (ll != NX_NULL)
            hop = ll;
        break;
    }

    out[0] = hop[0];
    out[1] = hop[1];
    out[2] = hop[2];
    out[3] = hop[3];
}

VOID ami_ptp_ipv6_shown_hop(NX_IP *ip, const NX_INTERFACE *ifp,
                            const ULONG hop[4], ULONG out[4])
{
    const ULONG *ll;
    ULONG        peer[4];
    UINT         i;

    out[0] = hop[0];
    out[1] = hop[1];
    out[2] = hop[2];
    out[3] = hop[3];

    if (ifp == NX_NULL || !anx6_ptp_peer_get((UINT)ifp->nx_interface_index, peer))
        return;

    ll = ami_ptp6_link_local(&ip->nx_ip_interface[ifp->nx_interface_index]);
    if (ll == NX_NULL || ll[0] != hop[0] || ll[1] != hop[1] ||
        ll[2] != hop[2] || ll[3] != hop[3])
        return;

    for (i = 0; i < 4U; i++)
        out[i] = peer[i];
}
