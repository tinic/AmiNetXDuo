/*
 * AmiNetXDuo, the live configuration surface of the running stack.
 *
 * These calls map NetX interface slots back to configuration, expose and
 * change per-interface mDNS policy, and arbitrate runtime hostname offers.
 * They need the singleton state, but none of the singleton lifecycle.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_internal.h"

const AmiIfConfig *netstack_iface_config(UWORD nx_index)
{
    const AmiNetStack *ns = ami_netstack_raw();
    UWORD              slot;

    if (ns == NULL || nx_index >= ns->ns_IfaceCount ||
        nx_index >= (UWORD)AMI_CFG_MAX_ATTACHED)
        return NULL;

    slot = ns->ns_IfaceCfg[nx_index];
    if (slot >= (UWORD)AMI_CFG_MAX_ATTACHED ||
        !ns->ns_Config.interfaces[slot].configured)
        return NULL;

    return &ns->ns_Config.interfaces[slot];
}

BOOL netstack_iface_mdns(UWORD nx_index)
{
    AmiNetStack *ns = ami_netstack_raw();

    if (ns == NULL || nx_index >= (UWORD)AMI_CFG_MAX_ATTACHED)
        return FALSE;

#ifdef AMINETXDUO_MDNS
    if (!ns->ns_IfaceMdns[nx_index])
        return FALSE;

    return ami_netstack_mdns_is_published(ns, nx_index);
#else
    return FALSE;
#endif
}

LONG netstack_iface_mdns_set(UWORD nx_index, BOOL enable)
{
#ifdef AMINETXDUO_MDNS
    AmiNetStack *ns = ami_netstack_raw();

    if (ns == NULL || !ns->ns_IpCreated ||
        nx_index >= (UWORD)AMI_CFG_MAX_ATTACHED ||
        ns->ns_Iface[nx_index] == NULL)
        return AMI_NET_ERR_STATE;

    return ami_netstack_mdns_iface_set(ns, nx_index, enable);
#else
    (VOID)nx_index;
    (VOID)enable;

    return AMI_NET_ERR_NODEV;
#endif
}

/*
 * A pointer to the running configuration, and nothing else.  The DHCP/RA
 * handoff is absorbed at the entry points that report live resolver state,
 * not here.
 */
const AmiConfig *netstack_config(VOID)
{
    AmiNetStack *ns = ami_netstack_raw();

    return (ns != NULL) ? &ns->ns_Config : NULL;
}

LONG netstack_hostname_offer(UWORD source, const char *name)
{
    AmiNetStack  *ns = ami_netstack_raw();
    AmiNetCaller *caller;
    BOOL          taken;

    if (ns == NULL)
        return AMI_NET_ERR_STATE;

    if (name == NULL || name[0] == '\0')
        return AMI_NET_ERR_CONFIG;

    /*
     * Inside the bracket: reports and DHCP lease reconciliation read this
     * buffer from the ThreadX side, and the baton also protects the DHCP
     * client's stable outgoing name while it is copied below.
     */
    caller = ami_netstack_enter_alloc();
    if (caller == NULL)
        return AMI_NET_ERR_KERNEL;

    taken = ami_config_hostname_offer(&ns->ns_Config, source, name);
    if (taken)
    {
#ifdef AMINETXDUO_DHCP
        ami_ns_dhcp_hostname_displace(&ns->ns_DhcpHostname);

        if (ns->ns_DhcpCreated)
            ami_ns_copy_name(ns->ns_DhcpName, ns->ns_Config.hostname,
                             sizeof(ns->ns_DhcpName));
#endif
    }

    ami_netstack_leave_free(caller);

    if (!taken)
        return AMI_NET_ERR_CONFIG;

    AMI_INFO("netstack: host name is now \"%s\"", name);

    return AMI_NET_OK;
}
