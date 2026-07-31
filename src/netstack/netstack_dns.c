/*
 * AmiNetXDuo -- name resolution.
 *
 * DEVS:Internet/hosts wins over the network, as in any BSD resolver; only then
 * does the query go to NetX Duo's addons/dns. The DNS client creates its own
 * packet pool (NX_DNS_CLIENT_USER_CREATE_PACKET_POOL is not defined upstream),
 * so nothing here competes with the stack pool.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_internal.h"

#include <proto/exec.h>

/* RFC 1035 2.3.4: 255 octets of domain name, plus the NUL. */
#define AMI_DNS_NAME_MAX    256

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

#ifdef NX_DNS_CACHE_ENABLE
    /*
     * NX_DNS_CACHE_ENABLE only compiles the code in; nx_dns_create() leaves
     * nx_dns_cache NULL and every path checks for NULL, so without this call
     * the feature is inert. See AMI_DNS_CACHE_BYTES for the size.
     *
     * Failure is not fatal: lookups still work, they just go to the wire.
     */
    status = nx_dns_cache_initialize(&ns->ns_Dns, ns->ns_DnsCache,
                                     (UINT)sizeof(ns->ns_DnsCache));
    if (status != NX_SUCCESS)
        AMI_WARN("netstack: DNS cache not initialised (%ld) -- every lookup "
                 "will go to the network", (long)status);
    else
        AMI_INFO("netstack: DNS cache %lu bytes",
                 (unsigned long)sizeof(ns->ns_DnsCache));
#endif

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

                if (nx_dns_server_add(&ns->ns_Dns, server) != NX_SUCCESS)
                    continue;

                AMI_INFO("netstack: DHCP name server %lu.%lu.%lu.%lu",
                         (unsigned long)((server >> 24) & 0xFFUL),
                         (unsigned long)((server >> 16) & 0xFFUL),
                         (unsigned long)((server >>  8) & 0xFFUL),
                         (unsigned long)(server & 0xFFUL));

                /*
                 * Record it in the configuration as well as in the DNS client.
                 * ShowNetStatus and ObtainDomainNameServerList() report from
                 * the configuration, so without this a DHCP machine lists the
                 * servers from the file (or none) while resolving through the
                 * ones the lease supplied.
                 */
                {
                    AmiResolverConfig *r     = &ns->ns_Config.resolver;
                    BOOL               known = FALSE;
                    UWORD              n;

                    for (n = 0; n < r->nameserver_count; n++)
                    {
                        if (r->nameserver[n] == server)
                            known = TRUE;
                    }

                    if (!known && r->nameserver_count < AMI_CFG_MAX_NAMESERVERS)
                    {
                        /* Positive: the lease put it here, not the file, so
                           the count is the real one. */
                        r->nameserver_use[r->nameserver_count] = 1;
                        r->nameserver[r->nameserver_count++]   = server;
                    }
                }
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

/*
 * Map a NetX Duo DNS status onto an actionable error. The distinction is
 * between "no resolver configured or reachable" and "that name does not
 * exist"; reporting the second as a device failure sends the reader to check
 * cables over a mistyped host name.
 */
static LONG ami_ns_dns_error(UINT status)
{
    switch (status)
    {
        case NX_DNS_NO_SERVER:
        case NX_DNS_EMPTY_DNS_SERVER_LIST:
        case NX_DNS_SERVER_NOT_FOUND:
            return AMI_NET_ERR_NOSERVER;

        case NX_DNS_TIMEOUT:
            return AMI_NET_ERR_TIMEOUT;

        case NX_DNS_QUERY_FAILED:
        case NX_DNS_MISMATCHED_RESPONSE:
        case NX_DNS_BAD_ID_ERROR:
        case NX_DNS_SERVER_AUTH_ERROR:
            /* The servers were asked and none has the name; a mistyped name
               is the likeliest cause. */
            return AMI_NET_ERR_NONAME;

        case NX_DNS_PARAM_ERROR:
        case NX_DNS_BAD_ADDRESS_ERROR:
        case NX_DNS_SIZE_ERROR:
            return AMI_NET_ERR_CONFIG;

        default:
            return AMI_NET_ERR_NONAME;
    }
}

/* -------------------------------------------------------------- public API */

/* One lookup of exactly the name given. */
static LONG ami_ns_resolve_once(const char *name, ULONG *addr_out,
                                ULONG timeout_ticks)
{
    AmiNetStack         *ns = ami_netstack_raw();
    const AmiNetdbEntry *entry;
    AmiNetCaller         *caller;
    ULONG                address = 0;
    UINT                 status;

    /* DEVS:Internet/hosts first -- it must work with the network down. */
    entry = ami_netdb_host_by_name(name);
    if (entry != NULL)
    {
        *addr_out = entry->value;
        return AMI_NET_OK;
    }

    if (ns == NULL || !ns->ns_DnsCreated)
        return AMI_NET_ERR_STATE;

    caller = ami_netstack_enter_alloc();
    if (caller == NULL)
        return AMI_NET_ERR_KERNEL;

#ifdef AMINETXDUO_MDNS
    /*
     * RFC 6762 6.7 requires a name ending in ".local" to be sent to
     * 224.0.0.251 and never to a unicast DNS server. Many home routers answer
     * any name with their own NXDOMAIN-substitute search page, and some
     * forward .local to the internet where somebody else's server answers, so
     * the branch is exclusive: no mDNS answer means the name does not exist.
     *
     * The check lives here rather than in a new command because every name an
     * AmigaOS program looks up arrives at this function -- gethostbyname() and
     * getaddrinfo() in src/bsdsocket/ both route through it -- so
     * `host amiga.local`, `ping amiga.local` and `fetch http://amiga.local/`
     * work unchanged, as does any Roadshow-era program.
     *
     * The hosts file above still wins: a name pinned in DEVS:Internet/hosts
     * outranks anything the network claims, .local included.
     */
    if (ami_netstack_mdns_is_local(name))
    {
        LONG err = ami_netstack_mdns_resolve(name, &address, timeout_ticks);

        ami_netstack_leave_free(caller);

        if (err != AMI_NET_OK)
        {
            AMI_INFO("netstack: nothing on this network answers to '%s'", name);
            return AMI_NET_ERR_NONAME;
        }

        *addr_out = address;
        return AMI_NET_OK;
    }
#endif

    status = nx_dns_host_by_name_get(&ns->ns_Dns, (UCHAR *)name, &address,
                                     timeout_ticks);

    ami_netstack_leave_free(caller);

    if (status != NX_SUCCESS)
    {
        AMI_INFO("netstack: '%s' not resolved (DNS status %ld)", name,
                 (long)status);
        return ami_ns_dns_error(status);
    }

    *addr_out = address;

    return AMI_NET_OK;
}

/* A name with no dot in it carries no domain, so the default domain applies. */
static BOOL ami_ns_unqualified(const char *name)
{
    ULONG i;

    for (i = 0; name[i] != '\0'; i++)
        if (name[i] == '.')
            return FALSE;

    return TRUE;
}

/* "name" "." "domain", or FALSE if that does not fit. */
static BOOL ami_ns_join_domain(char *dst, ULONG size, const char *name,
                               const char *domain)
{
    ULONG n = 0;
    ULONG i;

    for (i = 0; name[i] != '\0'; i++)
    {
        if (n + 2 >= size)
            return FALSE;
        dst[n++] = name[i];
    }

    if (n + 2 >= size)
        return FALSE;
    dst[n++] = '.';

    for (i = 0; domain[i] != '\0'; i++)
    {
        if (n + 1 >= size)
            return FALSE;
        dst[n++] = domain[i];
    }

    /* A trailing dot on the domain would give "host..", and an empty domain
       would give "host." -- neither is the name the caller meant. */
    if (dst[n - 1] == '.')
        return FALSE;

    dst[n] = '\0';

    return TRUE;
}

LONG netstack_resolve(const char *name, ULONG *addr_out, ULONG timeout_ticks)
{
    AmiNetStack *ns = ami_netstack_raw();
    char         qualified[AMI_DNS_NAME_MAX];
    LONG         err;

    if (name == NULL || *name == '\0' || addr_out == NULL)
        return AMI_NET_ERR_CONFIG;

    err = ami_ns_resolve_once(name, addr_out, timeout_ticks);
    if (err == AMI_NET_OK)
        return err;

    /*
     * "If no domain name is part of a host name, a default domain name can be
     * added to it if the host name lookup fails" -- GetDefaultDomainName().
     * So `ping fileserver` reaches fileserver.lan.
     *
     * Only after a definite no: TIMEOUT and NOSERVER say nothing about the
     * name, and a second query would just double the wait. ERR_STATE is worth
     * a retry even so, because with the stack down the retry never reaches the
     * network -- it can only hit DEVS:Internet/hosts, which costs nothing.
     */
    if (err != AMI_NET_ERR_NONAME && err != AMI_NET_ERR_STATE)
        return err;

    if (ns == NULL || ns->ns_Config.resolver.domain[0] == '\0')
        return err;
    if (!ami_ns_unqualified(name))
        return err;
    if (!ami_ns_join_domain(qualified, (ULONG)sizeof(qualified), name,
                            ns->ns_Config.resolver.domain))
        return err;

    if (ami_ns_resolve_once(qualified, addr_out, timeout_ticks) == AMI_NET_OK)
        return AMI_NET_OK;

    /* The caller asked about the bare name. Whatever the speculative retry ran
       into is not an answer about that name, so report the first failure. */
    return err;
}

LONG netstack_resolve_reverse(ULONG addr, char *name_out, ULONG name_len,
                              ULONG timeout_ticks)
{
    AmiNetStack         *ns = ami_netstack_raw();
    const AmiNetdbEntry *entry;
    AmiNetCaller         *caller;
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

    caller = ami_netstack_enter_alloc();
    if (caller == NULL)
        return AMI_NET_ERR_KERNEL;

    status = nx_dns_host_by_address_get(&ns->ns_Dns, addr, (UCHAR *)name_out,
                                        (UINT)name_len, timeout_ticks);

    ami_netstack_leave_free(caller);

    return (status == NX_SUCCESS) ? AMI_NET_OK : ami_ns_dns_error(status);
}

#ifdef AMINETXDUO_IPV6
LONG netstack_resolve6(const char *name, ULONG addr_out[4], ULONG timeout_ticks)
{
    AmiNetStack        *ns = ami_netstack_raw();
    AmiNetCaller        *caller;
    NX_DNS_IPV6_ADDRESS answer[1];
    UINT                count = 0;
    UINT                status;

    if (name == NULL || *name == '\0' || addr_out == NULL)
        return AMI_NET_ERR_CONFIG;

    /*
     * DEVS:Internet/hosts is not consulted here: src/config/netdb.c parses a
     * hosts entry's address with ami_config_parse_ip(), which only understands
     * dotted quads, so the store cannot hold an IPv6 address. Fixing that
     * means a netdb schema change (a second value field, or a family tag per
     * entry) touching get{host,net}by* as well. Until then an IPv6 literal in
     * DEVS:Internet/hosts is ignored and an IPv6-only name must be resolvable
     * by DNS.
     */
    if (ns == NULL || !ns->ns_DnsCreated)
        return AMI_NET_ERR_STATE;

    caller = ami_netstack_enter_alloc();
    if (caller == NULL)
        return AMI_NET_ERR_KERNEL;

    status = nxd_dns_ipv6_address_by_name_get(&ns->ns_Dns, (UCHAR *)name,
                                              answer, (UINT)sizeof(answer),
                                              &count, timeout_ticks);

    ami_netstack_leave_free(caller);

    if (status != NX_SUCCESS)
        return ami_ns_dns_error(status);
    if (count == 0)
        return AMI_NET_ERR_NONAME;

    addr_out[0] = answer[0].ipv6_address[0];
    addr_out[1] = answer[0].ipv6_address[1];
    addr_out[2] = answer[0].ipv6_address[2];
    addr_out[3] = answer[0].ipv6_address[3];

    return AMI_NET_OK;
}
#endif /* AMINETXDUO_IPV6 */

/* ------------------------------------------- changing the server list --- */

/*
 * Roadshow lets a program add and remove name servers while the stack is
 * running (AddDomainNameServer() and friends), and its own AddNetInterface
 * uses that to pass on the servers from a lease it obtained itself. Without
 * these, that command configured an interface and then failed on the last step
 * (docs/RESEARCH.md 55).
 *
 * A server has to land in two places: the NetX Duo DNS client, which resolves,
 * and ns_Config.resolver, which ShowNetStatus, ObtainDomainNameServerList and
 * CheckNetConfig read. The DHCP path above does the same.
 */

/*
 * AddDomainNameServer() nests: "adding the same address twice will require two
 * calls RemoveDomainNameServer() to remove it again" (autodoc). Two programs
 * can therefore share a server, and the first one to exit must not take the
 * other one's resolver with it.
 *
 * nameserver_use[] carries the count, signed as ObtainDomainNameServerList()
 * reports it -- negative for a server from DEVS:Internet/name_resolution,
 * positive for one DHCP or this call put there. Adding to a static entry keeps
 * it static and deepens it (-1 -> -2); the entry only leaves the list when the
 * count reaches zero. NetX Duo's own list does not count, so it is touched
 * only on the first add and the last remove.
 */

static LONG ami_ns_use_deepen(LONG use)
{
    return (use < 0) ? (use - 1) : (use + 1);
}

static LONG ami_ns_use_shallow(LONG use)
{
    return (use < 0) ? (use + 1) : (use - 1);
}

LONG netstack_dns_server_add(ULONG address)
{
    AmiNetStack  *ns = netstack_get();
    AmiNetCaller  *caller;
    UINT          status;
    UWORD         i;

    if (address == 0UL)
        return AMI_NET_ERR_CONFIG;
    if (ns == NULL)
        return AMI_NET_ERR_STATE;

    /* Already known: count the reference and leave the resolver alone. */
    for (i = 0; i < ns->ns_Config.resolver.nameserver_count; i++)
        if (ns->ns_Config.resolver.nameserver[i] == address)
        {
            ns->ns_Config.resolver.nameserver_use[i] =
                ami_ns_use_deepen(ns->ns_Config.resolver.nameserver_use[i]);
            return AMI_NET_OK;
        }

    if (ns->ns_Config.resolver.nameserver_count >= AMI_CFG_MAX_NAMESERVERS)
        return AMI_NET_ERR_NOMEM;

    caller = ami_netstack_enter_alloc();
    if (caller == NULL)
        return AMI_NET_ERR_STATE;
    status = nx_dns_server_add(&ns->ns_Dns, address);
    ami_netstack_leave_free(caller);

    if (status != NX_SUCCESS)
        return AMI_NET_ERR_CONFIG;

    ns->ns_Config.resolver.nameserver[ns->ns_Config.resolver.nameserver_count] =
        address;
    ns->ns_Config.resolver.nameserver_use[ns->ns_Config.resolver.nameserver_count] =
        1;
    ns->ns_Config.resolver.nameserver_count++;

    AMI_INFO("netstack: name server %lu.%lu.%lu.%lu added",
             (unsigned long)((address >> 24) & 0xFFUL),
             (unsigned long)((address >> 16) & 0xFFUL),
             (unsigned long)((address >>  8) & 0xFFUL),
             (unsigned long)(address & 0xFFUL));

    return AMI_NET_OK;
}

LONG netstack_dns_server_remove(ULONG address)
{
    AmiNetStack  *ns = netstack_get();
    AmiNetCaller  *caller;
    UINT          status;
    UWORD         i;
    UWORD         at;
    LONG          use;

    if (address == 0UL)
        return AMI_NET_ERR_CONFIG;
    if (ns == NULL)
        return AMI_NET_ERR_STATE;

    at = (UWORD)AMI_CFG_MAX_NAMESERVERS;
    for (i = 0; i < ns->ns_Config.resolver.nameserver_count; i++)
        if (ns->ns_Config.resolver.nameserver[i] == address)
        {
            at = i;
            break;
        }
    if (at >= (UWORD)AMI_CFG_MAX_NAMESERVERS)
        return AMI_NET_ERR_NONAME;

    /* Still referenced by somebody else: drop one and stop. */
    use = ami_ns_use_shallow(ns->ns_Config.resolver.nameserver_use[at]);
    if (use != 0)
    {
        ns->ns_Config.resolver.nameserver_use[at] = use;
        return AMI_NET_OK;
    }

    caller = ami_netstack_enter_alloc();
    if (caller == NULL)
        return AMI_NET_ERR_STATE;
    status = nx_dns_server_remove(&ns->ns_Dns, address);
    ami_netstack_leave_free(caller);

    if (status != NX_SUCCESS)
        return AMI_NET_ERR_CONFIG;

    /* Close the gap: the order of the rest is the order they were added in. */
    for (i = at; i + 1 < ns->ns_Config.resolver.nameserver_count; i++)
    {
        ns->ns_Config.resolver.nameserver[i] =
            ns->ns_Config.resolver.nameserver[i + 1];
        ns->ns_Config.resolver.nameserver_use[i] =
            ns->ns_Config.resolver.nameserver_use[i + 1];
    }
    ns->ns_Config.resolver.nameserver_count--;
    ns->ns_Config.resolver.nameserver[ns->ns_Config.resolver.nameserver_count] =
        0UL;
    ns->ns_Config.resolver.nameserver_use[ns->ns_Config.resolver.nameserver_count] =
        0;

    return AMI_NET_OK;
}

LONG netstack_set_domain_name(const char *name)
{
    AmiNetStack *ns = netstack_get();
    UWORD        i;

    if (ns == NULL)
        return AMI_NET_ERR_STATE;

    /* A NULL or empty name clears it, which is how Roadshow documents it. */
    if (name == NULL || name[0] == '\0')
    {
        ns->ns_Config.resolver.domain[0] = '\0';
        return AMI_NET_OK;
    }

    /* Truncating a domain name silently would produce wrong lookups, so the
       length is checked before anything is stored -- writing the truncated
       form and then reporting failure left the resolver on a domain the caller
       was told had been refused. */
    for (i = 0; name[i] != '\0'; i++)
    {
        if (i + 1 >= (UWORD)sizeof(ns->ns_Config.resolver.domain))
            return AMI_NET_ERR_CONFIG;
    }

    for (i = 0; name[i] != '\0'; i++)
        ns->ns_Config.resolver.domain[i] = name[i];
    ns->ns_Config.resolver.domain[i] = '\0';

    return AMI_NET_OK;
}
