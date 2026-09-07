/*
 * AmiNetXDuo, name resolution with the resolver compiled out.
 *
 * netstack_dns.c consults DEVS:Internet/hosts before it ever forms a query,
 * because that half has to work with the network down.  This file is that half
 * on its own: every entry point netstack_dns.c exports is answered here, the
 * hosts file still wins, and where the query would have gone out the answer is
 * AMI_NET_ERR_NONAME -- the name does not exist, which for a machine with no
 * resolver is the truth rather than a placeholder.
 *
 * So gethostbyname("192.168.1.10") still works: src/config/netdb.c parses a
 * dotted quad with ami_config_parse_ip() and never asks the network.
 *
 * The search list is not walked.  It exists to qualify a bare name for a
 * server to answer, and there is no server; DEVS:Internet/hosts is matched on
 * what it literally contains.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_internal.h"

#include <proto/exec.h>
#include <stddef.h>

/* ------------------------------------------------------------- helpers --- */

/* The same one netstack_dns.c exports; netstack_internal.h declares it and
   callers outside the resolver use it. */
VOID ami_ns_copy_name(char *dst, const char *src, ULONG size)
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

/* ------------------------------------------------------------- forward --- */

LONG netstack_resolve_until(const char *name, ULONG *addr_out,
                            ULONG timeout_ticks, AmiNetGiveUpFn give_up,
                            VOID *give_up_arg)
{
    const AmiNetdbEntry *entry;

    (VOID)timeout_ticks;
    (VOID)give_up;
    (VOID)give_up_arg;

    if (name == NULL || *name == '\0' || addr_out == NULL)
        return AMI_NET_ERR_CONFIG;

    entry = ami_netdb_host_by_name(name);
    if (entry != NULL)
    {
        *addr_out = entry->value;
        return AMI_NET_OK;
    }

    return AMI_NET_ERR_NONAME;
}

LONG netstack_resolve(const char *name, ULONG *addr_out, ULONG timeout_ticks)
{
    return netstack_resolve_until(name, addr_out, timeout_ticks, NULL, NULL);
}

/* ------------------------------------------------------------- reverse --- */

LONG netstack_resolve_reverse_until(ULONG addr, char *name_out, ULONG name_len,
                                    ULONG timeout_ticks,
                                    AmiNetGiveUpFn give_up, VOID *give_up_arg)
{
    const AmiNetdbEntry *entry;

    (VOID)timeout_ticks;
    (VOID)give_up;
    (VOID)give_up_arg;

    if (name_out == NULL || name_len == 0)
        return AMI_NET_ERR_CONFIG;

    entry = ami_netdb_host_by_addr(addr);
    if (entry != NULL)
    {
        ami_ns_copy_name(name_out, entry->name, name_len);
        return AMI_NET_OK;
    }

    return AMI_NET_ERR_NONAME;
}

LONG netstack_resolve_reverse(ULONG addr, char *name_out, ULONG name_len,
                              ULONG timeout_ticks)
{
    return netstack_resolve_reverse_until(addr, name_out, name_len,
                                          timeout_ticks, NULL, NULL);
}

/* ---------------------------------------------------------------- AAAA --- */

#ifdef AMINETXDUO_IPV6
/*
 * DEVS:Internet/hosts is parsed by ami_config_parse_ip() into one ULONG, so it
 * holds no AAAA record to answer out of.  With no resolver there is nothing
 * else to ask.
 */
LONG netstack_resolve6_until(const char *name, ULONG addr_out[4],
                             ULONG timeout_ticks, AmiNetGiveUpFn give_up,
                             VOID *give_up_arg)
{
    (VOID)timeout_ticks;
    (VOID)give_up;
    (VOID)give_up_arg;

    if (name == NULL || *name == '\0' || addr_out == NULL)
        return AMI_NET_ERR_CONFIG;

    return AMI_NET_ERR_NONAME;
}

LONG netstack_resolve6(const char *name, ULONG addr_out[4], ULONG timeout_ticks)
{
    return netstack_resolve6_until(name, addr_out, timeout_ticks, NULL, NULL);
}

/*
 * The two Router Advertisement options that carry resolver settings, RFC 8106.
 * netstack_ra.c installs these with the NetX Duo IPv6 stack whether or not a
 * resolver exists, so they have to be here to take the callback and drop it.
 */
VOID ami_ns6_rdnss(NX_IP *ip_ptr, UINT interface_index, ULONG *dns_address,
                   ULONG lifetime)
{
    (VOID)ip_ptr;
    (VOID)interface_index;
    (VOID)dns_address;
    (VOID)lifetime;
}

VOID ami_ns6_dnssl(NX_IP *ip_ptr, UINT interface_index, UCHAR *domains,
                   UINT length, ULONG lifetime)
{
    (VOID)ip_ptr;
    (VOID)interface_index;
    (VOID)domains;
    (VOID)length;
    (VOID)lifetime;
}

LONG netstack_dns_server6_add(const ULONG address[AMI_CFG_IP6_WORDS])
{
    (VOID)address;
    return AMI_NET_ERR_STATE;
}

LONG netstack_dns_server6_remove(const ULONG address[AMI_CFG_IP6_WORDS])
{
    (VOID)address;
    return AMI_NET_ERR_STATE;
}
#endif /* AMINETXDUO_IPV6 */

/* ------------------------------------------------------------ lifetime --- */

LONG ami_netstack_dns_start(AmiNetStack *ns)
{
    if (ns == NULL || !ns->ns_IpCreated)
        return AMI_NET_ERR_STATE;

    return AMI_NET_OK;
}

VOID ami_netstack_dns_stop(AmiNetStack *ns)
{
    (VOID)ns;
}

VOID netstack_dns_absorb_pending(VOID)
{
}

/* ---------------------------------------------------------------- DHCP --- */

/*
 * A lease can still carry option 6.  Nothing here can act on it, and saying so
 * is the point: reconcile reports "nothing changed" so the caller does not go
 * looking for a resolver that was reconfigured.
 */
BOOL ami_netstack_dns_dhcp_reconcile(AmiNetStack *ns, UWORD interface_index)
{
    (VOID)ns;
    (VOID)interface_index;
    return FALSE;
}

VOID ami_netstack_dns_dhcp_changed(AmiNetStack *ns, UWORD interface_index)
{
    (VOID)ns;
    (VOID)interface_index;
}

/* -------------------------------------------------- reported settings --- */

/*
 * ShowNetStatus, ObtainDomainNameServerList and CheckNetConfig read the
 * configured resolver whether or not one runs, and the configuration file is
 * still parsed, so these keep reporting it.  What they cannot do is change it:
 * add and remove have no client to program.
 */
LONG netstack_resolver_snapshot(AmiResolverConfig *out)
{
    AmiNetStack *ns = netstack_get();

    if (out == NULL)
        return AMI_NET_ERR_CONFIG;
    if (ns == NULL)
        return AMI_NET_ERR_STATE;

    Forbid();
    *out = ns->ns_Config.resolver;
    Permit();

    return AMI_NET_OK;
}

LONG netstack_domain_name_get(char *out, ULONG out_size)
{
    AmiNetStack *ns = netstack_get();
    LONG         result = AMI_NET_OK;
    ULONG        len;

    if (out == NULL || out_size == 0)
        return AMI_NET_ERR_CONFIG;

    out[0] = '\0';
    if (ns == NULL)
        return AMI_NET_ERR_STATE;

    Forbid();

    for (len = 0; ns->ns_Config.resolver.domain[len] != '\0'; len++)
        ;

    if (len == 0)
        result = AMI_NET_ERR_NONAME;
    else if (len >= out_size)
        result = AMI_NET_ERR_CONFIG;
    else
        ami_ns_copy_name(out, ns->ns_Config.resolver.domain, out_size);

    Permit();

    return result;
}

LONG netstack_dns_server_add(ULONG address)
{
    (VOID)address;
    return AMI_NET_ERR_STATE;
}

LONG netstack_dns_server_remove(ULONG address)
{
    (VOID)address;
    return AMI_NET_ERR_STATE;
}

LONG netstack_set_domain_name(const char *name)
{
    (VOID)name;
    return AMI_NET_ERR_STATE;
}
