/*
 * AmiNetXDuo, applying default-gateway policy to the live stack.
 *
 * Candidate ordering is the pure policy in netstack_gateway.c.  This file is
 * the stateful boundary: it reads live interface state and makes the NetX Duo
 * calls which install or clear the one machine-wide IPv4 default gateway.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_internal.h"

#include "aminetxduo/events.h"

/* The next hop this slot knows: the lease's option 3 while it is bound, else
   what the file named. */
static ULONG ami_ns_gateway_of(AmiNetStack *ns, UWORD index)
{
    const AmiIfConfig *cfg = &ns->ns_Config.interfaces[index];

#ifdef AMINETXDUO_DHCP
    if (cfg->iptype == AMI_IPTYPE_DHCP &&
        ns->ns_DhcpGateway[index] != 0UL)
        return ns->ns_DhcpGateway[index];
#endif

    return cfg->gateway;
}

/*
 * DHCP callbacks call this while holding the DHCP mutex; that lock order is
 * supported because the unmodified client used to make the same IP calls at
 * exactly those points.  Other callers are already inside a ThreadX bracket.
 * `skip` excludes a lease that just died or an interface whose detach has not
 * finished clearing its slot yet.
 */
VOID ami_ns_gateway_reconcile(AmiNetStack *ns, UWORD skip,
                              const char *reason)
{
    AmiNsGatewayIface table[AMI_CFG_MAX_ATTACHED];
    AmiNsGatewayCandidate candidate[AMI_CFG_MAX_ATTACHED];
    ULONG             installed = 0UL;
    ULONG             wanted = 0UL;
    UWORD             count;
    UWORD             i;
    UINT              status;

    if (ns == NULL || !ns->ns_IpCreated)
        return;

    ami_netstack_config_routes_install(ns);

    /* REQUIRED.  Every branch below acts on `installed', and a failed read
       must not leave an indeterminate value that preserves stale routing. */
    if (nx_ip_gateway_address_get(&ns->ns_Ip, &installed) != NX_SUCCESS)
        installed = 0UL;

    if (ns->ns_GatewayMode == (UBYTE)AMI_NS_GATEWAY_CLEARED)
    {
        if (installed != 0UL)
            /* REQUIRED.  If the old gateway will not go, traffic keeps
               leaving through it, which is the thing this was asked to stop. */
            if (nx_ip_gateway_address_clear(&ns->ns_Ip) != NX_SUCCESS)
                AMI_ERROR("netstack: the default gateway would not clear; "
                          "traffic still leaves through the old one");
        return;
    }

    if (ns->ns_GatewayMode == (UBYTE)AMI_NS_GATEWAY_FIXED)
    {
        wanted = ns->ns_GatewayFixed;
        if (wanted == 0UL || wanted == installed)
            return;

        status = nx_ip_gateway_address_set(&ns->ns_Ip, wanted);
        if (status != NX_SUCCESS)
        {
            ami_event(NETEVENT_GATEWAY_REFUSED, ns->ns_GatewayPrimary,
                      (ULONG)status);
            AMI_WARN("netstack: fixed default gateway was refused after %s "
                     "(%ld)", reason, (long)status);
        }
        return;
    }

    for (i = 0; i < (UWORD)AMI_CFG_MAX_ATTACHED; i++)
    {
        table[i].present = (BOOL)(
            i < ns->ns_IfaceCount && ns->ns_Iface[i] != NULL &&
            ns->ns_Config.interfaces[i].configured &&
            ns->ns_Ip.nx_ip_interface[i].nx_interface_valid != 0 &&
            ns->ns_Ip.nx_ip_interface[i].nx_interface_link_up != NX_FALSE);
        table[i].gateway = table[i].present ? ami_ns_gateway_of(ns, i) : 0UL;
        table[i].priority = table[i].present
                                ? ns->ns_Ip.nx_ip_interface[i].nx_interface_priority
                                : 0;
    }

    count = ami_ns_gateway_candidates(table, (UWORD)AMI_CFG_MAX_ATTACHED,
                                      ns->ns_GatewayPrimary, skip, candidate,
                                      (UWORD)AMI_CFG_MAX_ATTACHED);

    for (i = 0; i < count; i++)
    {
        if (candidate[i].gateway == installed &&
            ns->ns_Ip.nx_ip_gateway_interface ==
                &ns->ns_Ip.nx_ip_interface[candidate[i].iface])
            return;

        if (nx_ip_gateway_interface_address_set(
                &ns->ns_Ip, (UINT)candidate[i].iface,
                candidate[i].gateway) != NX_SUCCESS)
            continue;

        AMI_INFO("netstack: default gateway %lu.%lu.%lu.%lu selected after %s",
                 (unsigned long)((candidate[i].gateway >> 24) & 0xFFUL),
                 (unsigned long)((candidate[i].gateway >> 16) & 0xFFUL),
                 (unsigned long)((candidate[i].gateway >>  8) & 0xFFUL),
                 (unsigned long)(candidate[i].gateway & 0xFFUL), reason);
        return;
    }

    if (installed != 0UL)
        /* REQUIRED, as above: a gateway that will not clear is one that is
           still routing. */
        if (nx_ip_gateway_address_clear(&ns->ns_Ip) != NX_SUCCESS)
            AMI_ERROR("netstack: the default gateway would not clear before "
                      "installing another");

    if (count != 0)
        AMI_WARN("netstack: no live interface accepted a default gateway "
                 "after %s", reason);
}

/* A successful route command is authoritative until another route command
   changes it.  DHCP can neither replace it nor resurrect a deleted default. */
VOID netstack_gateway_override_set(ULONG gateway)
{
    AmiNetStack *ns = ami_netstack_raw();

    if (ns == NULL)
        return;

    ns->ns_GatewayFixed = gateway;
    ns->ns_GatewayMode = (UBYTE)AMI_NS_GATEWAY_FIXED;
}

VOID netstack_gateway_override_clear(VOID)
{
    AmiNetStack *ns = ami_netstack_raw();

    if (ns == NULL)
        return;

    ns->ns_GatewayFixed = 0UL;
    ns->ns_GatewayMode = (UBYTE)AMI_NS_GATEWAY_CLEARED;
}
