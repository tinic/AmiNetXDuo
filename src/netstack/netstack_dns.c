/*
 * AmiNetXDuo -- name resolution.
 *
 * DEVS:Internet/hosts wins over the network, the way every BSD resolver has
 * always done it, and only then does the query go to NetX Duo's addons/dns.
 * The DNS client owns its own packet pool (NX_DNS_CLIENT_USER_CREATE_PACKET_POOL
 * is not defined upstream), so nothing here competes with the stack pool.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_internal.h"

#include <proto/exec.h>

static VOID ami_ns_copy_name(char *dst, const char *src, ULONG size)
{
    ULONG i = 0;

    if (dst == NULL || size == 0)
        return;

    while (src != NULL && src[i] != '\0' && i + 1 < size)
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

LONG ami_netstack_dns_start(AmiNetStack *ns)
{
    UINT  status;
    UWORD i;

    if (ns == NULL || !ns->ns_IpCreated)
        return AMI_NET_ERR_STATE;

    if (ns->ns_DnsCreated)
        return AMI_NET_OK;

    status = nx_dns_create(&ns->ns_Dns, &ns->ns_Ip,
                           (UCHAR *)ns->ns_Config.resolver.domain);
    if (status != NX_SUCCESS)
    {
        AMI_WARN("netstack: nx_dns_create failed (%ld)", (long)status);
        return AMI_NET_ERR_KERNEL;
    }

    ns->ns_DnsCreated = TRUE;

    for (i = 0; i < ns->ns_Config.resolver.nameserver_count; i++)
    {
        ULONG server = ns->ns_Config.resolver.nameserver[i];

        if (server == 0UL)
            continue;

        status = nx_dns_server_add(&ns->ns_Dns, server);
        if (status != NX_SUCCESS)
            AMI_WARN("netstack: DNS server %lu.%lu.%lu.%lu rejected (%ld)",
                     (unsigned long)((server >> 24) & 0xFFUL),
                     (unsigned long)((server >> 16) & 0xFFUL),
                     (unsigned long)((server >>  8) & 0xFFUL),
                     (unsigned long)(server & 0xFFUL), (long)status);
    }

    /*
     * DHCP normally supplies the servers. nx_dhcp handed them to the IP
     * instance rather than to us, so pick them out of the lease.
     */
    if (ns->ns_DhcpStarted)
    {
        UCHAR buffer[4 * NX_DNS_MAX_SERVERS];
        UINT  size = (UINT)sizeof(buffer);

        if (nx_dhcp_user_option_retrieve(&ns->ns_Dhcp, NX_DHCP_OPTION_DNS_SVR,
                                         buffer, &size) == NX_SUCCESS)
        {
            UINT offset;

            for (offset = 0; offset + 4 <= size; offset += 4)
            {
                ULONG server = ((ULONG)buffer[offset]     << 24) |
                               ((ULONG)buffer[offset + 1] << 16) |
                               ((ULONG)buffer[offset + 2] <<  8) |
                                (ULONG)buffer[offset + 3];

                if (server == 0UL)
                    continue;

                if (nx_dns_server_add(&ns->ns_Dns, server) == NX_SUCCESS)
                    AMI_INFO("netstack: DHCP name server %lu.%lu.%lu.%lu",
                             (unsigned long)((server >> 24) & 0xFFUL),
                             (unsigned long)((server >> 16) & 0xFFUL),
                             (unsigned long)((server >>  8) & 0xFFUL),
                             (unsigned long)(server & 0xFFUL));
            }
        }
    }

    return AMI_NET_OK;
}

VOID ami_netstack_dns_stop(AmiNetStack *ns)
{
    if (ns == NULL || !ns->ns_DnsCreated)
        return;

    (VOID)nx_dns_delete(&ns->ns_Dns);
    ns->ns_DnsCreated = FALSE;
}

/* -------------------------------------------------------------- public API */

LONG netstack_resolve(const char *name, ULONG *addr_out, ULONG timeout_ticks)
{
    AmiNetStack         *ns = ami_netstack_raw();
    const AmiNetdbEntry *entry;
    AmiNetCaller         caller;
    ULONG                address = 0;
    UINT                 status;

    if (name == NULL || *name == '\0' || addr_out == NULL)
        return AMI_NET_ERR_CONFIG;

    /* DEVS:Internet/hosts first -- it must work with the network down. */
    entry = ami_netdb_host_by_name(name);
    if (entry != NULL)
    {
        *addr_out = entry->value;
        return AMI_NET_OK;
    }

    if (ns == NULL || !ns->ns_DnsCreated)
        return AMI_NET_ERR_STATE;

    if (ami_netstack_enter(&caller) != AMI_NET_OK)
        return AMI_NET_ERR_KERNEL;

    status = nx_dns_host_by_name_get(&ns->ns_Dns, (UCHAR *)name, &address,
                                     timeout_ticks);

    ami_netstack_leave(&caller);

    if (status != NX_SUCCESS)
        return AMI_NET_ERR_NODEV;

    *addr_out = address;

    return AMI_NET_OK;
}

LONG netstack_resolve_reverse(ULONG addr, char *name_out, ULONG name_len,
                              ULONG timeout_ticks)
{
    AmiNetStack         *ns = ami_netstack_raw();
    const AmiNetdbEntry *entry;
    AmiNetCaller         caller;
    UINT                 status;

    if (name_out == NULL || name_len == 0)
        return AMI_NET_ERR_CONFIG;

    entry = ami_netdb_host_by_addr(addr);
    if (entry != NULL)
    {
        ami_ns_copy_name(name_out, entry->name, name_len);
        return AMI_NET_OK;
    }

    if (ns == NULL || !ns->ns_DnsCreated)
        return AMI_NET_ERR_STATE;

    if (ami_netstack_enter(&caller) != AMI_NET_OK)
        return AMI_NET_ERR_KERNEL;

    status = nx_dns_host_by_address_get(&ns->ns_Dns, addr, (UCHAR *)name_out,
                                        (UINT)name_len, timeout_ticks);

    ami_netstack_leave(&caller);

    return (status == NX_SUCCESS) ? AMI_NET_OK : AMI_NET_ERR_NODEV;
}

#ifdef AMINETXDUO_IPV6
LONG netstack_resolve6(const char *name, ULONG addr_out[4], ULONG timeout_ticks)
{
    AmiNetStack        *ns = ami_netstack_raw();
    AmiNetCaller        caller;
    NX_DNS_IPV6_ADDRESS answer[1];
    UINT                count = 0;
    UINT                status;

    if (name == NULL || *name == '\0' || addr_out == NULL)
        return AMI_NET_ERR_CONFIG;

    /*
     * DEVS:Internet/hosts is NOT consulted here, and that is a real gap rather
     * than an oversight: src/config/netdb.c parses a hosts entry's address
     * with ami_config_parse_ip(), which only understands dotted quads, so the
     * store cannot hold an IPv6 address to find. Making it able to is a change
     * to the netdb schema (a second value field, or a family tag on every
     * entry) that touches get{host,net}by* as well, and it belongs with that
     * work rather than being smuggled in here.
     *
     * The practical consequence: an IPv6 literal in DEVS:Internet/hosts is
     * ignored, and an IPv6-only name has to be resolvable by DNS.
     */
    if (ns == NULL || !ns->ns_DnsCreated)
        return AMI_NET_ERR_STATE;

    if (ami_netstack_enter(&caller) != AMI_NET_OK)
        return AMI_NET_ERR_KERNEL;

    status = nxd_dns_ipv6_address_by_name_get(&ns->ns_Dns, (UCHAR *)name,
                                              answer, (UINT)sizeof(answer),
                                              &count, timeout_ticks);

    ami_netstack_leave(&caller);

    if (status != NX_SUCCESS || count == 0)
        return AMI_NET_ERR_NODEV;

    addr_out[0] = answer[0].ipv6_address[0];
    addr_out[1] = answer[0].ipv6_address[1];
    addr_out[2] = answer[0].ipv6_address[2];
    addr_out[3] = answer[0].ipv6_address[3];

    return AMI_NET_OK;
}
#endif /* AMINETXDUO_IPV6 */
