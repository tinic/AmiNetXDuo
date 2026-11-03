/*
 * AmiNetXDuo, IPv6 on a point-to-point interface, on the host.
 *
 * The LAN card in slot 0 and an OpenVPN tunnel in slot 1, no MAC and no
 * address mapping, as sana2_driver.c attaches vpn.device.
 * ami_ptp_ipv6_start() against the real nxd_ipv6_address_set(), stateless
 * autoconfiguration switch, _nx_icmpv6_perform_DAD() and
 * _nx_ipv6_packet_send(): the tunnel gets a link-local of its own, detects
 * nothing, solicits no router and resolves no address; the LAN card is
 * configured as before and does all three.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nx_api.h"
#include "nx_ip.h"
#include "nx_ipv6.h"
#include "nx_icmpv6.h"

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
static TX_THREAD h_thread;              /* the caller nxde_* asks after */

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

/* netstack_ptp.c is built with ThreadX error checking, as in the library. */
UINT _txe_mutex_get(TX_MUTEX *mutex_ptr, ULONG wait_option)
{
    return _tx_mutex_get(mutex_ptr, wait_option);
}

UINT _txe_mutex_put(TX_MUTEX *mutex_ptr)
{
    return _tx_mutex_put(mutex_ptr);
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

/* Every neighbour solicitation, DAD's and address resolution's alike. */
static unsigned h_ns;

VOID _nx_icmpv6_send_ns(NX_IP *ip_ptr, ULONG *neighbor_IP_address,
                        INT send_slla, NXD_IPV6_ADDRESS *outgoing_address,
                        INT sendUnicast, ND_CACHE_ENTRY *NDCacheEntry)
{
    (void)ip_ptr; (void)neighbor_IP_address; (void)send_slla;
    (void)outgoing_address; (void)sendUnicast; (void)NDCacheEntry;
    h_ns++;
}

UINT _nx_mld_group_join(NX_IP *ip_ptr, ULONG *group_address,
                        NX_INTERFACE *nx_interface)
{
    (void)ip_ptr; (void)group_address; (void)nx_interface;
    return NX_SUCCESS;
}

UINT _nx_mld_is_message(NX_PACKET *packet_ptr, ULONG protocol)
{
    (void)packet_ptr; (void)protocol;
    return NX_FALSE;
}

VOID _nx_icmpv6_DAD_clear_NDCache_entry(NX_IP *ip_ptr, ULONG *ipv6_address)
{
    (void)ip_ptr; (void)ipv6_address;
}

VOID _nx_ip_packet_checksum_compute(NX_PACKET *packet_ptr)
{
    (void)packet_ptr;
}

VOID _nx_ipv6_fragment_process(struct NX_IP_DRIVER_STRUCT *driver_req_ptr,
                               UINT mtu)
{
    (void)driver_req_ptr; (void)mtu;
    h_check(0, "nothing here is fragmented");
}

VOID _nx_ip_packet_deferred_receive(NX_IP *ip_ptr, NX_PACKET *packet_ptr)
{
    (void)ip_ptr; (void)packet_ptr;
}

UINT _nx_packet_copy(NX_PACKET *packet_ptr, NX_PACKET **new_packet_ptr,
                     NX_PACKET_POOL *pool_ptr, ULONG wait_option)
{
    (void)packet_ptr; (void)new_packet_ptr; (void)pool_ptr; (void)wait_option;
    return NX_NO_PACKET;
}

static unsigned h_released;

UINT _nx_packet_transmit_release(NX_PACKET *packet_ptr)
{
    (void)packet_ptr;
    h_released++;
    return NX_SUCCESS;
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

/* What reached the driver, the last time anything did. */
static unsigned     h_sent;
static NX_IP_DRIVER h_req;

static VOID h_driver(NX_IP_DRIVER *req)
{
    h_sent++;
    h_req = *req;
}

static VOID h_icmpv6_on(NX_IP *ip_ptr, NX_PACKET *packet_ptr)
{
    (void)ip_ptr; (void)packet_ptr;
}

static unsigned h_dad_ok[2];

static VOID h_address_changed(NX_IP *ip_ptr, UINT status, UINT interface_index,
                              UINT address_index, ULONG *address)
{
    (void)ip_ptr; (void)address_index; (void)address;
    if (status == NX_IPV6_ADDRESS_DAD_SUCCESSFUL && interface_index < 2)
        h_dad_ok[interface_index]++;
}

/* ---------------------------------------------------------- the machine --- */

static NX_IP      h_ip;
static AmiSana2If h_lan = { NX_FALSE, 0UL, 0UL };
static AmiSana2If h_vpn = { NX_TRUE, 0x0A080001UL, 0UL };

static void h_interface_set(UINT index, ULONG msw, ULONG lsw, AmiSana2If *sana)
{
    NX_INTERFACE *nxif = &h_ip.nx_ip_interface[index];

    nxif -> nx_interface_valid                  = NX_TRUE;
    nxif -> nx_interface_link_up                = NX_TRUE;
    nxif -> nx_interface_index                  = (UCHAR)index;
    nxif -> nx_interface_ip_mtu_size            = (index == 0) ? 1500 : 1400;
    nxif -> nx_interface_physical_address_msw   = msw;
    nxif -> nx_interface_physical_address_lsw   = lsw;
    nxif -> nx_interface_additional_link_info   = sana;
    nxif -> nx_interface_link_driver_entry      = h_driver;
    /* As sana2_driver.c configures it. */
    nxif -> nx_interface_address_mapping_needed = (sana -> ptp) ? NX_FALSE
                                                                : NX_TRUE;
    /* As nxd_ipv6_enable() seeds every interface. */
    nxif -> nx_ipv6_rtr_solicitation_max      = NX_ICMPV6_MAX_RTR_SOLICITATIONS;
    nxif -> nx_ipv6_rtr_solicitation_count    = NX_ICMPV6_MAX_RTR_SOLICITATIONS;
    nxif -> nx_ipv6_rtr_solicitation_interval = NX_ICMPV6_RTR_SOLICITATION_INTERVAL;
    nxif -> nx_ipv6_rtr_solicitation_timer    = NX_ICMPV6_RTR_SOLICITATION_DELAY;
}

static void h_machine(void)
{
    UINT i;

    memset(&h_ip, 0, sizeof(h_ip));
    h_ip.nx_ip_id                      = NX_IP_ID;
    h_ip.nx_ip_icmpv6_packet_process   = h_icmpv6_on;
    h_ip.nx_ipv6_address_change_notify = h_address_changed;
    for (i = 0; i < NX_MAX_IPV6_ADDRESSES; i++)
        h_ip.nx_ipv6_address[i].nxd_ipv6_address_index = (UCHAR)i;

    h_interface_set(0, 0x00000080UL, 0x10AABBCCUL, &h_lan);
    h_interface_set(1, 0UL, 0UL, &h_vpn);

    _tx_thread_current_ptr = &h_thread;
}

static NXD_IPV6_ADDRESS *h_first(UINT index)
{
    return h_ip.nx_ip_interface[index].nxd_interface_ipv6_address_list_head;
}

static unsigned h_addresses(UINT index)
{
    NXD_IPV6_ADDRESS *a;
    unsigned          n = 0;

    for (a = h_first(index); a != NX_NULL; a = a -> nxd_ipv6_address_next)
        n++;
    return n;
}

static int h_nd_untouched(void)
{
    UINT i;

    for (i = 0; i < NX_IPV6_NEIGHBOR_CACHE_SIZE; i++)
        if (h_ip.nx_ipv6_nd_cache[i].nx_nd_cache_nd_status !=
            ND_CACHE_STATE_INVALID)
            return 0;
    return 1;
}

/* An ICMPv6-less datagram, no next header, from interface `index`'s first
   address to `dest`, through the real send path. */
static UCHAR     h_buffer[256];
static NX_PACKET h_packet;

static void h_send(UINT index, const ULONG dest[4])
{
    NXD_IPV6_ADDRESS *src = h_first(index);
    ULONG             d[4];

    memcpy(d, dest, sizeof(d));
    memset(&h_packet, 0, sizeof(h_packet));
    h_packet.nx_packet_data_start    = h_buffer;
    h_packet.nx_packet_data_end      = h_buffer + sizeof(h_buffer);
    h_packet.nx_packet_prepend_ptr   = h_buffer + 128;
    h_packet.nx_packet_append_ptr    = h_buffer + 128 + 8;
    h_packet.nx_packet_length        = 8;
    h_packet.nx_packet_ip_version    = NX_IP_VERSION_V6;
    h_packet.nx_packet_address.nx_packet_ipv6_address_ptr = src;

    _nx_ipv6_packet_send(&h_ip, &h_packet, NX_PROTOCOL_NO_NEXT_HEADER, 8, 64,
                         0, src -> nxd_ipv6_address, d);
}

/* ------------------------------------------------------------- the tests -- */

static void test_link_local_without_mac(void)
{
    NXD_IPV6_ADDRESS *ll;
    UINT              status;

    printf("ptp6: a point-to-point link gets a link-local without a MAC\n");

    status = ami_ptp_ipv6_start(&h_ip, 1);
    h_check(status == NX_SUCCESS, "ptp: the link-local is set");

    ll = h_first(1);
    h_check(ll != NX_NULL && h_addresses(1) == 1, "ptp: one address, on it");
    if (ll == NX_NULL)
        return;
    h_check(ll -> nxd_ipv6_address[0] == 0xFE800000UL &&
            ll -> nxd_ipv6_address[1] == 0UL,
            "ptp: in fe80::/64");
    h_check(ll -> nxd_ipv6_address_prefix_length == 10,
            "ptp: stored as NetX Duo stores a link-local, length 10");
    h_check((ll -> nxd_ipv6_address[2] | ll -> nxd_ipv6_address[3]) != 0UL,
            "ptp: with an interface identifier");
    h_check((ll -> nxd_ipv6_address[2] & 0x02000000UL) == 0UL,
            "ptp: the u bit clear, not a universal identifier");
    h_check(ll -> nxd_ipv6_address_attached == &h_ip.nx_ip_interface[1],
            "ptp: attached to the tunnel");

    status = ami_ptp_ipv6_start(&h_ip, 1);
    h_check(status == NX_DUPLICATED_ENTRY && h_addresses(1) == 1,
            "ptp: a second bring-up adds no second link-local");
}

static void test_ethernet_link_local(void)
{
    NXD_IPV6_ADDRESS *ll;
    UINT              index = 0;

    printf("ptp6: the LAN card's link-local is made from its MAC, as before\n");

    h_check(nxd_ipv6_address_set(&h_ip, 0, NX_NULL, 10, &index) == NX_SUCCESS,
            "lan: the link-local is set");
    ll = h_first(0);
    h_check(ll != NX_NULL &&
            ll -> nxd_ipv6_address[0] == 0xFE800000UL &&
            ll -> nxd_ipv6_address[2] == 0x028010FFUL &&
            ll -> nxd_ipv6_address[3] == 0xFEAABBCCUL,
            "lan: fe80::280:10ff:feaa:bbcc, the EUI-64 of 00:80:10:aa:bb:cc");
    h_check(ll != NX_NULL &&
            ll -> nxd_ipv6_address_DupAddrDetectTransmit == NX_IPV6_DAD_TRANSMITS,
            "lan: DAD transmits are NetX Duo's own");
    h_check(h_ip.nx_ip_interface[0].nx_ipv6_rtr_solicitation_count ==
                NX_ICMPV6_MAX_RTR_SOLICITATIONS,
            "lan: router solicitation untouched");
}

static void test_no_dad(void)
{
    NXD_IPV6_ADDRESS *ll = h_first(1);
    UINT              n;

    printf("ptp6: no duplicate address detection on a point-to-point link\n");

    h_check(ll != NX_NULL && ll -> nxd_ipv6_address_DupAddrDetectTransmit == 0,
            "ptp: DAD transmits are 0");
    h_check(ll != NX_NULL &&
            ll -> nxd_ipv6_address_state == NX_IPV6_ADDR_STATE_TENTATIVE,
            "ptp: tentative until the DAD pass");

    h_ns = 0;
    _nx_icmpv6_perform_DAD(&h_ip);

    h_check(h_ns == 1, "one solicitation in the pass, the LAN card's");
    h_check(ll != NX_NULL &&
            ll -> nxd_ipv6_address_state == NX_IPV6_ADDR_STATE_VALID,
            "ptp: valid after the one pass");
    h_check(h_dad_ok[1] == 1, "ptp: reported as DAD_SUCCESSFUL");
    h_check(h_first(0) -> nxd_ipv6_address_state ==
                NX_IPV6_ADDR_STATE_TENTATIVE && h_dad_ok[0] == 0,
            "lan: still tentative, detecting");

    for (n = 0; n < NX_IPV6_DAD_TRANSMITS; n++)
        _nx_icmpv6_perform_DAD(&h_ip);
    h_check(h_first(0) -> nxd_ipv6_address_state == NX_IPV6_ADDR_STATE_VALID,
            "lan: valid after its NX_IPV6_DAD_TRANSMITS");
}

static void test_no_router_solicitation(void)
{
    printf("ptp6: no router solicitation on a point-to-point link\n");

    h_check(h_ip.nx_ip_interface[1].nx_ipv6_rtr_solicitation_count == 0,
            "ptp: the solicitation count is 0");
    h_check(h_ip.nx_ip_interface[1].nx_ipv6_stateless_address_autoconfig_status ==
                NX_STATELESS_ADDRESS_AUTOCONFIG_DISABLED,
            "ptp: stateless autoconfiguration is off");
}

static void test_straight_to_the_peer(void)
{
    static const ULONG peer[4]  = { 0xFE800000UL, 0UL, 0UL, 1UL };
    static const ULONG far[4]   = { 0x20010DB8UL, 0UL, 0UL, 1UL };

    printf("ptp6: a datagram to the peer goes straight to the driver\n");

    h_ns = 0;
    h_sent = 0;
    h_send(1, peer);
    h_check(h_sent == 1 && h_req.nx_ip_driver_command == NX_LINK_PACKET_SEND,
            "ptp: handed to the driver as a packet send");
    h_check(h_req.nx_ip_driver_interface == &h_ip.nx_ip_interface[1],
            "ptp: on the tunnel");
    h_check(h_req.nx_ip_driver_physical_address_msw == 0UL &&
            h_req.nx_ip_driver_physical_address_lsw == 0UL,
            "ptp: with no link-layer address");
    h_check(h_req.nx_ip_driver_packet == &h_packet &&
            h_packet.nx_packet_ip_version == NX_IP_VERSION_V6 &&
            (h_packet.nx_packet_prepend_ptr[0] >> 4) == 6,
            "ptp: an IPv6 datagram, which sana2_driver.c sends as 0x86DD");
    h_check(h_ns == 0, "ptp: no solicitation for address resolution");
    h_check(h_nd_untouched(), "ptp: and no neighbour cache entry to resolve");

    h_sent = 0;
    h_send(1, far);
    h_check(h_sent == 1 &&
            h_req.nx_ip_driver_interface == &h_ip.nx_ip_interface[1] &&
            h_ns == 0 && h_nd_untouched(),
            "ptp: off-link too, with no router to resolve");
}

static void test_ethernet_resolves(void)
{
    static const ULONG neighbour[4] = { 0xFE800000UL, 0UL, 0UL, 0x1234UL };

    printf("ptp6: the LAN card resolves its neighbour, as before\n");

    h_ns = 0;
    h_sent = 0;
    h_send(0, neighbour);
    h_check(h_ns == 1, "lan: one solicitation for address resolution");
    h_check(h_sent == 0, "lan: and the datagram waits for the answer");
    h_check(!h_nd_untouched(), "lan: in an incomplete neighbour cache entry");
}

int main(void)
{
    h_machine();

    test_link_local_without_mac();
    test_ethernet_link_local();
    test_no_dad();
    test_no_router_solicitation();
    test_straight_to_the_peer();
    test_ethernet_resolves();

    printf("ptp6: %lu checks, %lu failures\n", h_checks, h_failures);
    return (h_failures == 0) ? 0 : 1;
}
