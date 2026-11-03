/*
 * AmiNetXDuo, IPv6 on a point-to-point interface in NetX Duo's terms.
 * Compiled with AMINETXDUO_PTP and AMINETXDUO_IPV6 both on.
 *
 * SPDX-License-Identifier: MIT
 */

#include "aminetxduo/ptp.h"
#include "aminetxduo/nxstatus.h"

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
    UINT              slot = 0;
    UINT              status;

    if (ip == NULL || index >= (UINT)NX_MAX_PHYSICAL_INTERFACES)
        return NX_INVALID_INTERFACE;

    /* Held across the set, so the IP thread never sees the address with
       NetX Duo's own count. */
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

    status = nxd_ipv6_address_set(ip, index, &ll, 10, &slot);
    if (status == NX_SUCCESS)
        ip->nx_ipv6_address[slot].nxd_ipv6_address_DupAddrDetectTransmit = 0;

    AMI_NX_ONLY_SUCCESS(tx_mutex_put(&ip->nx_ip_protection));

    if (status == NX_SUCCESS)
        AMI_NX_CLEANUP(
            nxd_ipv6_stateless_address_autoconfig_disable(ip, index));

    return status;
}
