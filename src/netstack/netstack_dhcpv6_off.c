/*
 * AmiNetXDuo, DHCPv6 with the client compiled out.
 *
 * The twin of netstack_dns_off.c, for the same reason: bsdsocket.library
 * exports the DHCPv6 control calls whatever the build, and netstack_ipv6.c
 * hands NetX Duo's single address-change slot on unconditionally.  Both need
 * something to call.
 *
 * Compiled when IPv6 is on and AMINETXDUO_DHCP is off.  With IPv6 off none of
 * this is reachable and neither file is built.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_internal.h"

#include <stddef.h>

VOID ami_netstack_dhcpv6_address_notify(NX_IP *ip_ptr, UINT status,
                                        UINT interface_index,
                                        UINT address_index, ULONG *address)
{
    (VOID)ip_ptr;
    (VOID)status;
    (VOID)interface_index;
    (VOID)address_index;
    (VOID)address;
}

VOID ami_netstack_dhcpv6_configure(AmiNetStack *ns)
{
    (VOID)ns;
}

VOID ami_netstack_dhcpv6_release(AmiNetStack *ns)
{
    (VOID)ns;
}

VOID ami_netstack_dhcpv6_pause(AmiNetStack *ns)
{
    (VOID)ns;
}

VOID ami_netstack_dhcpv6_resume(AmiNetStack *ns, UWORD interface_index)
{
    (VOID)ns;
    (VOID)interface_index;
}

VOID ami_netstack_dhcpv6_destroy(AmiNetStack *ns)
{
    (VOID)ns;
}

/*
 * A machine with no client has no lease to report or hand back.  SLAAC still
 * configures addresses, and netstack_interface_addresses6() reports those.
 */
LONG netstack_interface_dhcp6_status(UWORD interface_index,
                                     AmiDhcp6Status *out)
{
    (VOID)interface_index;
    (VOID)out;
    return AMI_NET_ERR_STATE;
}

LONG netstack_interface_dhcp6_release(UWORD interface_index)
{
    (VOID)interface_index;
    return AMI_NET_ERR_STATE;
}
