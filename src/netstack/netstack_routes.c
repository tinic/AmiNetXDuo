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
