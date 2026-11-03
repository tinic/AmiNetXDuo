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
 * link carries IPv4 alone, with no link header and no ARP; the peer is the
 * other end, host byte order, 0 when nobody has said.
 */
UINT  ami_sana2_ptp_wire(ULONG hardware_type, UINT addr_bytes, UINT asked);
UINT  ami_sana2_ptp(const AmiSana2If *iface);
ULONG ami_sana2_ptp_peer(const AmiSana2If *iface);
VOID  ami_sana2_set_ptp_peer(AmiSana2If *iface, ULONG peer);
/* The peer the host route was made for, and record a new one. */
ULONG ami_sana2_ptp_routed(const AmiSana2If *iface);
VOID  ami_sana2_set_ptp_routed(AmiSana2If *iface, ULONG peer);

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

#define AMI_PTP_NEXT_HOP(ip, gateway)   ami_ptp_next_hop((ip), (gateway))
#define AMI_PTP_SHOWN_HOP(ip, next_hop) ami_ptp_shown_hop((ip), (next_hop))

#else

#define AMI_PTP_NEXT_HOP(ip, gateway)   (gateway)
#define AMI_PTP_SHOWN_HOP(ip, next_hop) (next_hop)

#endif /* AMINETXDUO_PTP */

#endif /* AMINETXDUO_PTP_H */
