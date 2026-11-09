/*
 * AmiNetXDuo, point-to-point interfaces in NetX Duo's terms
 * (src/netstack/netstack_ptp.c has the why).
 *
 * Every place that hands a gateway to NetX Duo passes it through
 * AMI_PTP_NEXT_HOP, and every place that reports one through
 * AMI_PTP_SHOWN_HOP.  Without AMINETXDUO_PTP both are the address itself,
 * so the call sites compile to what they were.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_PTP_H
#define AMINETXDUO_PTP_H

#include "nx_api.h"

#ifdef AMINETXDUO_PTP

typedef struct AmiSana2If AmiSana2If;

/*
 * Point-to-point links.  Roadshow decides from the driver's wire type: PPP,
 * SLIP, CSLIP and PLIP are point-to-point, and POINTTOPOINT=YES makes a
 * driver with no Ethernet address one too (asked is AMI_CFG_PTP_*).  Such a
 * link carries bare IP datagrams, with no link header and no ARP; the peer is the
 * other end, host byte order, 0 when nobody has said.
 */
UINT  ami_sana2_ptp_wire(ULONG hardware_type, UINT addr_bytes, UINT asked);
UINT  ami_sana2_ptp(const AmiSana2If *iface);
ULONG ami_sana2_ptp_peer(const AmiSana2If *iface);
VOID  ami_sana2_set_ptp_peer(AmiSana2If *iface, ULONG peer);
/* The peer the host route was made for, and record a new one. */
ULONG ami_sana2_ptp_routed(const AmiSana2If *iface);
VOID  ami_sana2_set_ptp_routed(AmiSana2If *iface, ULONG peer);

/*
 * A file with no ADDRESS takes the device's station address once the link
 * has one.  The tick asks (ami_sana2_ptp_addr_tick(), interrupt level), the
 * reader polls, and the hook gets the address and the device's DstAddr.
 */
VOID  ami_sana2_ptp_addr_tick(VOID);
VOID  ami_sana2_set_ptp_addr_hook(VOID (*hook)(AmiSana2If *, ULONG, ULONG));

/*
 * Whether station address `src` is one to take on an interface at
 * `current` whose file named peer `named` (0: none) and whose link gave
 * `learned` before.  *peer_out is a peer to record, else 0: DstAddr, where
 * ppp.device puts the IPCP peer, but only when neither DESTINATION nor
 * GATEWAY named one, and never 0 or `src`.
 */
UINT  ami_ptp_station_offer(ULONG current, ULONG named, ULONG learned,
                            ULONG src, ULONG dst, ULONG *peer_out);

/*
 * One station-address poll.  `renew`: after S2EVENT_OFFLINE, when the link
 * may come back with other addresses; `had_*` are the last taken, `rx_moved`
 * whether a frame arrived since the event (IPCP is up again).
 */
#define AMI_PTP_POLL_AGAIN  0U      /* nothing yet, ask next second  */
#define AMI_PTP_POLL_TAKE   1U      /* hand it to the stack          */
#define AMI_PTP_POLL_KEEP   2U      /* back up, the same addresses   */

static inline UINT ami_ptp_poll_verdict(UINT renew, ULONG local, ULONG dst,
                                        ULONG had_local, ULONG had_dst,
                                        UINT rx_moved)
{
    if (local == 0UL)
        return AMI_PTP_POLL_AGAIN;
    if (!renew || local != had_local || (dst != 0UL && dst != had_dst))
        return AMI_PTP_POLL_TAKE;
    return rx_moved ? AMI_PTP_POLL_KEEP : AMI_PTP_POLL_AGAIN;
}

/*
 * The automatic default gateway of a point-to-point interface: GATEWAY,
 * else the peer, else the interface's own address (the link itself), as
 * Roadshow's ppp_dialer routes default through the peer.  0: none.
 */
ULONG ami_ptp_default_hop(ULONG gateway, ULONG peer, ULONG local);

/* The peer of a point-to-point interface, 0 for any other interface. */
ULONG ami_ptp_peer_of(const NX_INTERFACE *ifp);

/* A gateway as NetX Duo takes it: a point-to-point peer becomes the address
   of the interface it is the peer of; anything else is unchanged. */
ULONG ami_ptp_next_hop(NX_IP *ip, ULONG gateway);

/* The reverse, for whatever reports a gateway or a next hop. */
ULONG ami_ptp_shown_hop(NX_IP *ip, ULONG next_hop);

/* The host route to interface `index`'s peer, after its address or its peer
   changed.  The caller holds the stack as for any NetX Duo call. */
VOID  ami_ptp_peer_route(NX_IP *ip, UINT index);

#ifdef AMINETXDUO_IPV6
/* IPv6 on interface `index`: a random link-local, no DAD, no router
   solicitation.  NX_DUPLICATED_ENTRY when it has a link-local already. */
UINT  ami_ptp_ipv6_start(NX_IP *ip, UINT index);

/* An ADDRESS6 on interface `index`, with no duplicate address detection. */
UINT  ami_ptp_ipv6_address_set(NX_IP *ip, UINT index, NXD_ADDRESS *addr,
                               ULONG prefix);

/* DESTINATION6, the peer of interface `index`: NULL or :: for none. */
VOID  ami_ptp_ipv6_peer_set(UINT index, const ULONG *peer);

/* A default router as NetX Duo takes it: a peer becomes the link-local of
   the interface it is the peer of, anything else is unchanged.  And the
   reverse, for a router on interface `ifp`, for whatever reports one. */
VOID  ami_ptp_ipv6_next_hop(NX_IP *ip, const ULONG gw[4], ULONG out[4]);
VOID  ami_ptp_ipv6_shown_hop(NX_IP *ip, const NX_INTERFACE *ifp,
                             const ULONG hop[4], ULONG out[4]);

/* A prefix through gateway gw, which must be the peer of a point-to-point
   interface: up, and interface `zone` for a link-local peer.  Any other
   gateway is NX_NOT_SUPPORTED to the add and NX_ENTRY_NOT_FOUND to the
   delete.  The caller holds nx_ip_protection. */
UINT  ami_ptp_ipv6_route_add(NX_IP *ip, const ULONG dest[4], ULONG prefix_len,
                             const ULONG gw[4], UINT zone);
UINT  ami_ptp_ipv6_route_delete(NX_IP *ip, const ULONG dest[4],
                                ULONG prefix_len, const ULONG gw[4]);
#endif

#define AMI_PTP_NEXT_HOP(ip, gateway)   ami_ptp_next_hop((ip), (gateway))
#define AMI_PTP_SHOWN_HOP(ip, next_hop) ami_ptp_shown_hop((ip), (next_hop))

#else

#define AMI_PTP_NEXT_HOP(ip, gateway)   (gateway)
#define AMI_PTP_SHOWN_HOP(ip, next_hop) (next_hop)

#endif /* AMINETXDUO_PTP */

#endif /* AMINETXDUO_PTP_H */
