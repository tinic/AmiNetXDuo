/*
 * Persistent IPv4 routes from DEVS:Internet/routes.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_internal.h"

#include "aminetxduo/netstack.h"

_Static_assert(AMI_CFG_MAX_STATIC_ROUTES == NX_IP_ROUTING_TABLE_SIZE,
               "configured and live IPv4 route capacities must match");

/* A DHCP interface has no network at IP creation, so ADDRESS_ERROR means
   "try again when an address changes", not bad configuration. */
VOID ami_netstack_config_routes_install(AmiNetStack *ns)
{
#ifdef NX_ENABLE_IP_STATIC_ROUTING
    UWORD i;

    if (ns == NULL || !ns->ns_IpCreated)
        return;

    for (i = 0; i < ns->ns_Config.static_route_count; i++)
    {
        const AmiRouteConfig *route;
        UINT status;

        if (ns->ns_ConfigRouteInstalled[i] != 0U)
            continue;

        route = &ns->ns_Config.static_route[i];
        status = nx_ip_static_route_add(&ns->ns_Ip, route->destination,
                                        route->netmask, route->gateway);
        if (status == NX_SUCCESS)
        {
            ns->ns_ConfigRouteInstalled[i] = 1U;
            AMI_INFO("netstack: installed configured route %lu via %lu",
                     (unsigned long)route->destination,
                     (unsigned long)route->gateway);
        }
        else if (status != NX_IP_ADDRESS_ERROR)
        {
            AMI_WARN("netstack: configured route %lu via %lu was refused (%ld)",
                     (unsigned long)route->destination,
                     (unsigned long)route->gateway, (long)status);
        }
    }
#else
    (VOID)ns;
#endif
}

static VOID ami_ns_config_route_state(ULONG destination, ULONG netmask,
                                      UBYTE state)
{
    AmiNetStack *ns = netstack_get();
    UWORD i;

    if (ns == NULL)
        return;

    destination &= netmask;
    for (i = 0; i < ns->ns_Config.static_route_count; i++)
    {
        const AmiRouteConfig *route = &ns->ns_Config.static_route[i];

        if (route->destination == destination && route->netmask == netmask)
        {
            ns->ns_ConfigRouteInstalled[i] = state;
            return;
        }
    }
}

VOID netstack_config_route_added(ULONG destination, ULONG netmask)
{
    ami_ns_config_route_state(destination, netmask, 1U);
}

VOID netstack_config_route_deleted(ULONG destination, ULONG netmask)
{
    /* Non-zero keeps the installer away; two distinguishes this operator
       decision from an entry NetX removes with an interface. */
    ami_ns_config_route_state(destination, netmask, 2U);
}

/*
 * Keep the private NetX route walker on the netstack side of the boundary.
 * There is no public NetX API which returns both the selected interface and
 * next hop.  BSD callers need both, but do not need an NX_INTERFACE pointer.
 */
BOOL netstack_ipv4_route(ULONG destination, LONG preferred_index,
                         UWORD *index_out, ULONG *next_hop_out,
                         ULONG *source_address_out)
{
    AmiNetStack  *ns = netstack_get();
    NX_INTERFACE *nxif;
    ULONG         next_hop = 0UL;
    ULONG         source = 0UL;
    UINT          status;
    UWORD         index;

    if (ns == NULL || !ns->ns_IpCreated ||
        preferred_index < -1L ||
        preferred_index >= (LONG)NX_MAX_IP_INTERFACES)
        return FALSE;

    nxif = (preferred_index >= 0L)
               ? &ns->ns_Ip.nx_ip_interface[preferred_index]
               : NX_NULL;

    tx_mutex_get(&ns->ns_Ip.nx_ip_protection, TX_WAIT_FOREVER);
    status = _nx_ip_route_find(&ns->ns_Ip, destination, &nxif, &next_hop);

    if (status != NX_SUCCESS || nxif == NX_NULL)
    {
        tx_mutex_put(&ns->ns_Ip.nx_ip_protection);
        return FALSE;
    }

    /* Do not derive an index by subtracting an unchecked pointer.  NetX's
       private routine currently returns an entry from this array, but this
       boundary should fail closed if that contract ever changes. */
    for (index = 0U; index < (UWORD)NX_MAX_IP_INTERFACES; index++)
    {
        if (nxif == &ns->ns_Ip.nx_ip_interface[index])
            break;
    }

    if (index == (UWORD)NX_MAX_IP_INTERFACES ||
        nxif->nx_interface_valid == 0U)
    {
        tx_mutex_put(&ns->ns_Ip.nx_ip_protection);
        return FALSE;
    }

    source = nxif->nx_interface_ip_address;
    tx_mutex_put(&ns->ns_Ip.nx_ip_protection);

    if (index_out != NULL)
        *index_out = index;
    if (next_hop_out != NULL)
        *next_hop_out = next_hop;
    if (source_address_out != NULL)
        *source_address_out = source;

    return TRUE;
}
