/*
 * AmiNetXDuo, point-to-point interfaces in NetX Duo's terms, on the host.
 *
 * The LAN card in slot 0 and an OpenVPN tunnel in slot 1, as vpn.device
 * makes one: 10.8.0.15/32 with the peer 10.8.0.1.  netstack_ptp.c against
 * the real _nx_ip_route_find(), _nx_ip_gateway_address_set() and the static
 * route table: a peer is refused as a gateway as it stands, and is taken
 * once it is handed over as the interface's own address; the host route
 * makes the peer itself reachable, and follows a new peer.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nx_api.h"
#include "nx_ip.h"

#include "aminetxduo/ptp.h"

#include <stdio.h>
#include <string.h>

static unsigned long h_checks;
static unsigned long h_failures;

static void h_check(int ok, const char *what)
{
    h_checks++;
    if (!ok)
    {
        h_failures++;
        printf("  FAIL %s\n", what);
        return;
    }
    printf("  ok   %s\n", what);
}

/* ------------------------------------------------ what NetX Duo needs ---- */

TX_THREAD *_tx_thread_current_ptr;
volatile ULONG _tx_thread_preempt_disable;
volatile ULONG _tx_thread_system_state;
TX_THREAD _tx_timer_thread;
static TX_THREAD h_thread;              /* the caller nxe_* asks after */

UINT _tx_mutex_get(TX_MUTEX *mutex_ptr, ULONG wait_option)
{
    (void)mutex_ptr; (void)wait_option;
    return TX_SUCCESS;
}

UINT _tx_mutex_put(TX_MUTEX *mutex_ptr)
{
    (void)mutex_ptr;
    return TX_SUCCESS;
}

UINT _tx_thread_interrupt_disable(void)
{
    return 0;
}

VOID _tx_thread_interrupt_restore(UINT previous_posture)
{
    (void)previous_posture;
}

UINT _tx_thread_sleep(ULONG timer_ticks)
{
    (void)timer_ticks;
    printf("  FAIL NX_ASSERT fired\n");
    h_failures++;
    return TX_SUCCESS;
}

/* ---------------------------------------- the SANA-II side, as recorded -- */

struct AmiSana2If
{
    UINT  ptp;
    ULONG peer;
    ULONG routed;
};

UINT ami_sana2_ptp(const AmiSana2If *iface)
{
    return (iface != NULL) ? iface->ptp : NX_FALSE;
}

ULONG ami_sana2_ptp_peer(const AmiSana2If *iface)
{
    return (iface != NULL && iface->ptp) ? iface->peer : 0UL;
}

ULONG ami_sana2_ptp_routed(const AmiSana2If *iface)
{
    return (iface != NULL) ? iface->routed : 0UL;
}

VOID ami_sana2_set_ptp_routed(AmiSana2If *iface, ULONG peer)
{
    if (iface != NULL)
        iface->routed = peer;
}

/* ---------------------------------------------------------- the machine --- */

#define H_LAN_MASK      0xFFFFFF00UL
#define H_LAN_ADDR      0xC0A801DBUL     /* 192.168.1.219 */
#define H_LAN_GATEWAY   0xC0A80101UL     /* 192.168.1.1   */
#define H_VPN_ADDR      0x0A08000FUL     /* 10.8.0.15     */
#define H_VPN_ADDR2     0x0A080002UL     /* 10.8.0.2      */
#define H_VPN_PEER      0x0A080001UL     /* 10.8.0.1      */
#define H_VPN_PEER2     0x0A080002UL     /* 10.8.0.2      */
#define H_ELSEWHERE     0x08080808UL     /* 8.8.8.8       */

static NX_IP      h_ip;
static AmiSana2If h_lan = { NX_FALSE, 0UL, 0UL };
static AmiSana2If h_vpn = { NX_TRUE, H_VPN_PEER, 0UL };

static void h_interface_set(UINT index, ULONG address, ULONG mask,
                            AmiSana2If *sana)
{
    NX_INTERFACE *nxif = &h_ip.nx_ip_interface[index];

    memset(nxif, 0, sizeof(*nxif));
    nxif -> nx_interface_valid                  = NX_TRUE;
    nxif -> nx_interface_link_up                = NX_TRUE;
    nxif -> nx_interface_index                  = (UCHAR)index;
    nxif -> nx_interface_ip_address             = address;
    nxif -> nx_interface_ip_network_mask        = mask;
    nxif -> nx_interface_ip_network             = address & mask;
    nxif -> nx_interface_ip_mtu_size            = 1400;
    nxif -> nx_interface_additional_link_info   = sana;
    nxif -> nx_interface_address_mapping_needed = (sana -> ptp) ? NX_FALSE
                                                                : NX_TRUE;
}

static NX_INTERFACE *h_route(ULONG destination, ULONG *next_hop)
{
    NX_INTERFACE *chosen = NX_NULL;

    if (_nx_ip_route_find(&h_ip, destination, &chosen, next_hop) != NX_SUCCESS)
        return NX_NULL;

    return chosen;
}

static int h_route_to(ULONG destination, ULONG mask, ULONG next_hop)
{
    ULONG i;

    for (i = 0; i < h_ip.nx_ip_routing_table_entry_count; i++)
    {
        const NX_IP_ROUTING_ENTRY *e = &h_ip.nx_ip_routing_table[i];

        if (e -> nx_ip_routing_dest_ip == destination &&
            e -> nx_ip_routing_net_mask == mask &&
            e -> nx_ip_routing_next_hop_address == next_hop)
            return 1;
    }
    return 0;
}

int main(void)
{
    NX_INTERFACE *lan = &h_ip.nx_ip_interface[0];
    NX_INTERFACE *vpn = &h_ip.nx_ip_interface[1];
    ULONG         next_hop;

    memset(&h_ip, 0, sizeof(h_ip));
    h_ip.nx_ip_id = NX_IP_ID;
    _tx_thread_current_ptr = &h_thread;

    printf("Ptp: a LAN card and a /32 tunnel\n");

    h_interface_set(0, H_LAN_ADDR, H_LAN_MASK, &h_lan);
    h_interface_set(1, H_VPN_ADDR, 0xFFFFFFFFUL, &h_vpn);

    /* The translation, both ways. */
    h_check(ami_ptp_peer_of(vpn) == H_VPN_PEER && ami_ptp_peer_of(lan) == 0UL,
            "the peer is the tunnel's and not the card's");
    h_check(ami_ptp_next_hop(&h_ip, H_VPN_PEER) == H_VPN_ADDR,
            "a gateway naming the peer becomes the tunnel's own address");
    h_check(ami_ptp_next_hop(&h_ip, H_LAN_GATEWAY) == H_LAN_GATEWAY,
            "any other gateway is unchanged");
    h_check(ami_ptp_shown_hop(&h_ip, H_VPN_ADDR) == H_VPN_PEER &&
            ami_ptp_shown_hop(&h_ip, H_LAN_GATEWAY) == H_LAN_GATEWAY,
            "and reads back as the peer, and nothing else does");

    /* As it stands NetX Duo refuses the peer, which is why the translation
       exists, and takes the tunnel's own address. */
    h_check(_nx_ip_gateway_address_set(&h_ip, H_VPN_PEER) != NX_SUCCESS,
            "NetX Duo refuses the peer itself as a gateway (off the /32)");

    /* Without the host route the peer is not reachable. */
    h_check(h_route(H_VPN_PEER, &next_hop) == NX_NULL,
            "with no route the peer is unreachable");

    ami_ptp_peer_route(&h_ip, 1);
    h_check(h_route_to(H_VPN_PEER, 0xFFFFFFFFUL, H_VPN_ADDR) &&
            h_vpn.routed == H_VPN_PEER,
            "the host route to the peer goes through the tunnel's address");
    h_check(h_route(H_VPN_PEER, &next_hop) == vpn,
            "and the peer is reached through the tunnel");
    h_check(h_route(H_LAN_GATEWAY, &next_hop) == lan,
            "the LAN stays on the card");

    /* The default route through the tunnel: what OpenVPN's RouteUp asks for
       with AddNetRoute DEFAULT=<peer>. */
    h_check(_nx_ip_gateway_address_set(&h_ip,
                ami_ptp_next_hop(&h_ip, H_VPN_PEER)) == NX_SUCCESS,
            "the peer is taken as the default gateway once translated");
    h_check(h_route(H_ELSEWHERE, &next_hop) == vpn && next_hop == H_VPN_ADDR,
            "the rest of the world goes through the tunnel");
    h_check(h_route(H_LAN_GATEWAY, &next_hop) == lan,
            "and the LAN still goes through the card");

    /* The server pushes a new peer. */
    h_vpn.peer = H_VPN_PEER2;
    ami_ptp_peer_route(&h_ip, 1);
    h_check(!h_route_to(H_VPN_PEER, 0xFFFFFFFFUL, H_VPN_ADDR) &&
            h_route_to(H_VPN_PEER2, 0xFFFFFFFFUL, H_VPN_ADDR) &&
            h_vpn.routed == H_VPN_PEER2,
            "a new peer replaces the old host route");

    /* A peer inside the interface's own network needs no route. */
    h_vpn.peer = H_VPN_PEER;
    h_interface_set(1, H_VPN_ADDR, 0xFFFFFF00UL, &h_vpn);
    ami_ptp_peer_route(&h_ip, 1);
    h_check(!h_route_to(H_VPN_PEER2, 0xFFFFFFFFUL, H_VPN_ADDR) &&
            !h_route_to(H_VPN_PEER, 0xFFFFFFFFUL, H_VPN_ADDR) &&
            h_vpn.routed == 0UL,
            "a peer on the interface's network gets no host route");

    /* The route made for a peer goes when the peer comes onto the
       interface's network (antigravity-agy review, finding 2). */
    h_interface_set(1, H_VPN_ADDR, 0xFFFFFFFFUL, &h_vpn);
    ami_ptp_peer_route(&h_ip, 1);
    h_check(h_route_to(H_VPN_PEER, 0xFFFFFFFFUL, H_VPN_ADDR) &&
            h_vpn.routed == H_VPN_PEER, "a /32 again has its host route");
    h_interface_set(1, H_VPN_ADDR, 0xFFFFFF00UL, &h_vpn);
    ami_ptp_peer_route(&h_ip, 1);
    h_check(!h_route_to(H_VPN_PEER, 0xFFFFFFFFUL, H_VPN_ADDR) &&
            h_ip.nx_ip_routing_table_entry_count == 0 && h_vpn.routed == 0UL,
            "and loses it when the same peer comes onto its network");

    /* A new local address takes the default gateway and the routes through
       the link with it (finding 3). */
    h_interface_set(1, H_VPN_ADDR, 0xFFFFFFFFUL, &h_vpn);
    ami_ptp_peer_route(&h_ip, 1);
    h_check(_nx_ip_gateway_address_set(&h_ip,
                ami_ptp_next_hop(&h_ip, H_VPN_PEER)) == NX_SUCCESS,
            "the default gateway is the tunnel again");
    h_ip.nx_ip_interface[1].nx_interface_ip_address = H_VPN_ADDR2;
    h_ip.nx_ip_interface[1].nx_interface_ip_network = H_VPN_ADDR2;
    ami_ptp_peer_route(&h_ip, 1);
    h_check(h_ip.nx_ip_gateway_address == H_VPN_ADDR2 &&
            h_route_to(H_VPN_PEER, 0xFFFFFFFFUL, H_VPN_ADDR2) &&
            !h_route_to(H_VPN_PEER, 0xFFFFFFFFUL, H_VPN_ADDR),
            "a new address moves the gateway and the host route to it");
    h_check(h_route(H_ELSEWHERE, &next_hop) == vpn &&
            next_hop == H_VPN_ADDR2 &&
            ami_ptp_shown_hop(&h_ip, next_hop) == H_VPN_PEER,
            "and the default route still goes through the tunnel, shown as the peer");
    _nx_ip_gateway_address_clear(&h_ip);
    (VOID)_nx_ip_static_route_delete(&h_ip, H_VPN_PEER, 0xFFFFFFFFUL);
    h_vpn.routed = 0UL;

    /* A file with no ADDRESS: the station address once the link has one,
       as ppp.device answers S2_GETSTATIONADDRESS after IPCP. */
    {
        ULONG peer = 1UL;

        h_check(!ami_ptp_station_offer(0UL, 0UL, 0UL, 0UL, H_VPN_PEER,
                                       &peer) &&
                peer == 0UL,
                "no station address yet is nothing to take");
        h_check(ami_ptp_station_offer(0UL, H_VPN_PEER, H_VPN_PEER,
                                      H_VPN_ADDR, H_VPN_PEER2, &peer) &&
                peer == 0UL,
                "the first address is taken, and a named peer is kept");
        h_check(ami_ptp_station_offer(0UL, 0UL, 0UL, H_VPN_ADDR, H_VPN_PEER,
                                      &peer) &&
                peer == H_VPN_PEER,
                "with no peer named, the device's DstAddr is the peer");
        h_check(ami_ptp_station_offer(0UL, 0UL, 0UL, H_VPN_ADDR, H_VPN_ADDR,
                                      &peer) &&
                peer == 0UL,
                "a DstAddr equal to the address is not a peer");
        h_check(!ami_ptp_station_offer(H_VPN_ADDR, H_VPN_PEER, H_VPN_PEER,
                                       H_VPN_ADDR, H_VPN_PEER, &peer) &&
                peer == 0UL,
                "the same address again changes nothing");
        h_check(ami_ptp_station_offer(H_VPN_ADDR, H_VPN_PEER, H_VPN_PEER,
                                      H_VPN_ADDR2, 0UL, &peer) &&
                peer == 0UL,
                "a new link with a new address is taken");
        h_check(!ami_ptp_station_offer(H_VPN_ADDR, 0UL, H_VPN_PEER,
                                       H_VPN_ADDR, H_VPN_PEER, &peer),
                "the same address and learned peer again change nothing");
        h_check(ami_ptp_station_offer(H_VPN_ADDR, 0UL, H_VPN_PEER,
                                      H_VPN_ADDR, H_VPN_PEER2, &peer) &&
                peer == H_VPN_PEER2,
                "IPCP again with a new peer: the learned one is replaced");
        h_check(!ami_ptp_station_offer(H_VPN_ADDR, H_VPN_PEER, H_VPN_PEER,
                                       H_VPN_ADDR, H_VPN_PEER2, &peer) &&
                peer == 0UL,
                "a named peer is not replaced by the link's");
    }

    /* The polls after the link enable and after S2EVENT_OFFLINE. */
    {
        h_check(ami_ptp_poll_verdict(NX_FALSE, 0UL, 0UL, 0UL, 0UL, NX_FALSE) ==
                    AMI_PTP_POLL_AGAIN,
                "poll: no address yet asks again");
        h_check(ami_ptp_poll_verdict(NX_FALSE, H_VPN_ADDR, H_VPN_PEER,
                                     H_VPN_ADDR, H_VPN_PEER, NX_TRUE) ==
                    AMI_PTP_POLL_TAKE,
                "poll: after an enable the first address is taken");
        h_check(ami_ptp_poll_verdict(NX_TRUE, H_VPN_ADDR, H_VPN_PEER,
                                     H_VPN_ADDR, H_VPN_PEER, NX_FALSE) ==
                    AMI_PTP_POLL_AGAIN,
                "poll: renegotiating, the old address is not the answer");
        h_check(ami_ptp_poll_verdict(NX_TRUE, H_VPN_ADDR2, H_VPN_PEER,
                                     H_VPN_ADDR, H_VPN_PEER, NX_FALSE) ==
                    AMI_PTP_POLL_TAKE,
                "poll: renegotiated, a new local address is taken");
        h_check(ami_ptp_poll_verdict(NX_TRUE, H_VPN_ADDR, H_VPN_PEER2,
                                     H_VPN_ADDR, H_VPN_PEER, NX_FALSE) ==
                    AMI_PTP_POLL_TAKE,
                "poll: renegotiated, a new peer is taken");
        h_check(ami_ptp_poll_verdict(NX_TRUE, H_VPN_ADDR, 0UL,
                                     H_VPN_ADDR, H_VPN_PEER, NX_FALSE) ==
                    AMI_PTP_POLL_AGAIN,
                "poll: no DstAddr is not a new peer");
        h_check(ami_ptp_poll_verdict(NX_TRUE, H_VPN_ADDR, H_VPN_PEER,
                                     H_VPN_ADDR, H_VPN_PEER, NX_TRUE) ==
                    AMI_PTP_POLL_KEEP,
                "poll: frames again on the old addresses end the polls");
    }

    /* The automatic default through a point-to-point link. */
    {
        h_check(ami_ptp_default_hop(H_VPN_PEER2, H_VPN_PEER, H_VPN_ADDR) ==
                    H_VPN_PEER2,
                "default: GATEWAY wins");
        h_check(ami_ptp_default_hop(0UL, H_VPN_PEER, H_VPN_ADDR) ==
                    H_VPN_PEER,
                "default: no GATEWAY, through the peer");
        h_check(ami_ptp_default_hop(0UL, 0UL, H_VPN_ADDR) == H_VPN_ADDR,
                "default: no peer known, through the link itself");
        h_check(ami_ptp_default_hop(0UL, 0UL, 0UL) == 0UL,
                "default: no address yet, none");
    }

    {

        /* Taken: the interface gets it /32, and the host route follows. */
        h_vpn.peer = H_VPN_PEER;
        h_interface_set(1, 0UL, 0xFFFFFFFFUL, &h_vpn);
        ami_ptp_peer_route(&h_ip, 1);
        h_check(h_ip.nx_ip_routing_table_entry_count == 0,
                "no address, no host route");
        h_interface_set(1, H_VPN_ADDR, 0xFFFFFFFFUL, &h_vpn);
        ami_ptp_peer_route(&h_ip, 1);
        h_check(h_route_to(H_VPN_PEER, 0xFFFFFFFFUL, H_VPN_ADDR) &&
                h_route(H_VPN_PEER, &next_hop) == vpn,
                "once the address is taken the peer is routed");
        (VOID)_nx_ip_static_route_delete(&h_ip, H_VPN_PEER, 0xFFFFFFFFUL);
        h_vpn.routed = 0UL;
    }

    /* An Ethernet interface is left alone. */
    ami_ptp_peer_route(&h_ip, 0);
    h_check(h_ip.nx_ip_routing_table_entry_count == 0,
            "an Ethernet interface gets no route");

    printf("%lu checks, %lu failures, %s\n", h_checks, h_failures,
           (h_failures == 0) ? "PASS" : "FAIL");
    return (h_failures == 0) ? 0 : 1;
}
