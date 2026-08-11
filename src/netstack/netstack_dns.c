/*
 * AmiNetXDuo, name resolution.
 *
 * DEVS:Internet/hosts wins over the network, as in any BSD resolver; only then
 * does the query go to NetX Duo's addons/dns. The DNS client creates its own
 * packet pool (NX_DNS_CLIENT_USER_CREATE_PACKET_POOL is not defined upstream),
 * so nothing here competes with the stack pool.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_internal.h"
#include "netstack_dns_status.h"
#include "netstack_retry.h"

#include <proto/exec.h>
#include <stddef.h>

/* RFC 1035 2.3.4: 255 octets of domain name, plus the NUL. */
#define AMI_DNS_NAME_MAX    256

#ifdef AMINETXDUO_IPV6

/*
 * The RFC 8106 half of the resolver.  An IPv6-only link configures addresses
 * from a router advertisement and nothing else, so without this the machine
 * comes up routable and cannot resolve a name: there is no DHCPv6 in this
 * build and DEVS:Internet/name_resolution takes a dotted quad only.
 *
 * The advertisement arrives on the IP thread, which may not call the DNS
 * client, nxd_dns_server_add() waits on the mutex a query holds, and that
 * query is waiting on this thread.  So the callback records and the next
 * lookup absorbs.
 */
static BOOL ami_ns6_same(const ULONG a[4], const ULONG b[4])
{
    return (BOOL)(a[0] == b[0] && a[1] == b[1] &&
                  a[2] == b[2] && a[3] == b[3]);
}

VOID ami_ns6_rdnss(NX_IP *ip_ptr, UINT interface_index, ULONG *dns_address,
                   ULONG lifetime)
{
    AmiNetStack *ns = ami_netstack_raw();
    UWORD        i;

    (VOID)ip_ptr;
    (VOID)interface_index;

    if (ns == NULL || dns_address == NULL)
        return;

    for (i = 0; i < ns->ns_RdnssCount; i++)
        if (ami_ns6_same(ns->ns_Rdnss[i].nxd_ip_address.v6, dns_address))
            break;

    /* RFC 8106 5.1: a lifetime of zero withdraws the server.  Dropping it from
       this array is what makes the absorb step take it out of the resolver;
       returning here left it answering queries after the router had said to
       stop using it, for as long as the machine stayed up. */
    if (lifetime == 0UL)
    {
        if (i == ns->ns_RdnssCount)
            return;

        for (; (UWORD)(i + 1) < ns->ns_RdnssCount; i++)
            ns->ns_Rdnss[i] = ns->ns_Rdnss[i + 1];

        ns->ns_RdnssCount--;
        ns->ns_RdnssPending = TRUE;

        return;
    }

    /* Already known.  Every advertisement repeats the option, so this is the
       ordinary case and not a change. */
    if (i != ns->ns_RdnssCount)
        return;

    if (ns->ns_RdnssCount >= (UWORD)AMI_RDNSS_MAX)
        return;

    ns->ns_Rdnss[i].nxd_ip_version       = NX_IP_VERSION_V6;
    ns->ns_Rdnss[i].nxd_ip_address.v6[0] = dns_address[0];
    ns->ns_Rdnss[i].nxd_ip_address.v6[1] = dns_address[1];
    ns->ns_Rdnss[i].nxd_ip_address.v6[2] = dns_address[2];
    ns->ns_Rdnss[i].nxd_ip_address.v6[3] = dns_address[3];

    ns->ns_RdnssCount   = (UWORD)(i + 1);
    ns->ns_RdnssPending = TRUE;
}

/*
 * The RFC 8106 5.2 half, the suffixes rather than the servers.  Same thread
 * and the same reason for not acting here: the list it feeds is read by every
 * resolver call, so the option is copied and the next lookup decodes it.
 *
 * The bytes rather than the names, because the decoder for this encoding
 * belongs to the configuration (DHCP option 119 carries the same one) and
 * running it here would put a parser reached straight off the network on the
 * IP thread.
 */
VOID ami_ns6_dnssl(NX_IP *ip_ptr, UINT interface_index, UCHAR *domains,
                   UINT length, ULONG lifetime)
{
    AmiNetStack *ns = ami_netstack_raw();
    UWORD        i;

    (VOID)ip_ptr;
    (VOID)interface_index;

    if (ns == NULL || domains == NULL || length == 0)
        return;

    /* A list longer than this can hold is one no search list could hold
       either; taking the front of it would be taking an arbitrary prefix of
       somebody's domain list, so it is refused whole. */
    if (length > (UINT)AMI_DNSSL_MAX)
    {
        ns->ns_DnsslPending = FALSE;
        return;
    }

    for (i = 0; i < (UWORD)length; i++)
        ns->ns_Dnssl[i] = (UBYTE)domains[i];

    ns->ns_DnsslLen      = (UWORD)length;
    ns->ns_DnsslLifetime = lifetime;
    ns->ns_DnsslPending  = TRUE;
}

/*
 * Take what the callbacks recorded and make the resolver agree with it.
 * Called from a caller thread on the way into a lookup, which is where it is
 * safe: the DNS client holds its mutex across a query and the IP thread the
 * advertisement arrived on is what that query is waiting for.
 *
 * The servers are reconciled rather than added, because a withdrawal is a
 * change too: anything in the configuration that the router no longer names
 * leaves the DNS client and the reported list, and anything it names that is
 * not there yet joins both.
 */
static VOID ami_ns_dns_absorb_rdnss(AmiNetStack *ns)
{
    AmiResolverConfig *r;
    UWORD              i;
    UWORD              j;

    if (ns == NULL || !ns->ns_DnsCreated)
        return;

    r = &ns->ns_Config.resolver;

    if (ns->ns_RdnssPending)
    {
        char text[AMI_CFG_IP6_STRLEN];

        ns->ns_RdnssPending = FALSE;

        /* Out: in the configuration, not in the advertisement.  Backwards, so
           the compaction inside the withdraw does not skip an entry. */
        for (i = r->nameserver6_count; i-- != 0; )
        {
            ULONG gone[AMI_CFG_IP6_WORDS];

            for (j = 0; j < ns->ns_RdnssCount; j++)
                if (ami_ns6_same(r->nameserver6[i],
                                 ns->ns_Rdnss[j].nxd_ip_address.v6))
                    break;

            if (j != ns->ns_RdnssCount)
                continue;

            gone[0] = r->nameserver6[i][0];
            gone[1] = r->nameserver6[i][1];
            gone[2] = r->nameserver6[i][2];
            gone[3] = r->nameserver6[i][3];

            if (!ami_config_nameserver6_withdraw(r, gone))
                continue;

            {
                NXD_ADDRESS address;

                address.nxd_ip_version       = NX_IP_VERSION_V6;
                address.nxd_ip_address.v6[0] = gone[0];
                address.nxd_ip_address.v6[1] = gone[1];
                address.nxd_ip_address.v6[2] = gone[2];
                address.nxd_ip_address.v6[3] = gone[3];

                (VOID)nxd_dns_server_remove(&ns->ns_Dns, &address);
            }

            ami_config_format_ip6(gone, text, sizeof(text));
            AMI_INFO("netstack: advertised name server %s withdrawn", text);
        }

        /*
         * In: the DNS client first, and the configuration only if that worked.
         * ShowNetStatus, ObtainDomainNameServerList() and CheckNetConfig all
         * report from the configuration, so the two must not disagree; the
         * DHCP path records its servers for the same reason, and this one
         * recorded none at all, which is the visible half of this defect.
         */
        for (i = 0; i < ns->ns_RdnssCount; i++)
        {
            UINT status = nxd_dns_server_add(&ns->ns_Dns, &ns->ns_Rdnss[i]);

            ami_config_format_ip6(ns->ns_Rdnss[i].nxd_ip_address.v6, text,
                                  sizeof(text));

            if (status != NX_SUCCESS && status != NX_DNS_DUPLICATE_ENTRY)
            {
                AMI_WARN("netstack: advertised name server %s rejected (%ld)",
                         text, (long)status);
                continue;
            }

            if (ami_config_nameserver6_offer(r, ns->ns_Rdnss[i].nxd_ip_address.v6))
                AMI_INFO("netstack: advertised name server %s", text);
        }
    }

    if (ns->ns_DnsslPending)
    {
        UWORD n;

        ns->ns_DnsslPending = FALSE;

        /* RFC 8106 5.2, as 5.1: zero withdraws what the option names. */
        if (ns->ns_DnsslLifetime == 0UL)
        {
            n = ami_config_search_withdraw_rfc3397(r, ns->ns_Dnssl,
                                                   (ULONG)ns->ns_DnsslLen);
            if (n != 0)
                AMI_INFO("netstack: advertised search list withdrawn, %ld "
                         "domain(s)", (long)n);
        }
        else
        {
            n = ami_config_search_from_rfc3397(r, ns->ns_Dnssl,
                                               (ULONG)ns->ns_DnsslLen);
            if (n != 0)
            {
                AMI_INFO("netstack: advertised search list, %ld domain(s), "
                         "first '%s'", (long)n,
                         r->search[r->search_count - n]);

                /* What GetDefaultDomainName() reports and what a name with no
                   dot is qualified with when nothing else named a domain, the
                   same standing DHCP option 15 is given.  A router that names
                   several names only the first, since there is one default. */
                if (r->domain[0] == '\0')
                    ami_ns_copy_name(r->domain, r->search[r->search_count - n],
                                     sizeof(r->domain));
            }
        }
    }
}

#endif /* AMINETXDUO_IPV6 */

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

/*
 * What the lease said about naming, which nothing acted on until now: option
 * 12 was requested and reported per interface but never became the machine's
 * name, and option 15 never became its domain, so a DHCP machine had no
 * default domain at all and netstack_resolve()'s qualifying step never ran.
 *
 * The name is offered rather than assigned: a HOSTNAME in name_resolution
 * outranks it, and an option 12 that is not a host name is refused (see
 * AmiHostnameSource).
 *
 * The lease's domains are appended to the search list rather than weighed
 * against the file's. A DOMAIN= somebody wrote is still the default domain
 * GetDefaultDomainName() reports and still the first suffix tried, but it no
 * longer stops the lease being used: the machine this was reported from had
 * `domain localdomain` in its file and local.tinic.net in its lease, and only
 * the file's was ever tried, so `ssh playhouse2` did not resolve on the
 * network the lease describes.
 *
 * From the first interface holding a lease, not always interface 0: a machine
 * with a static interface 0 and a DHCP interface 1 has its lease on the one
 * that asked for it.
 */
static VOID ami_ns_dhcp_naming(AmiNetStack *ns)
{
    char  text[AMI_CFG_NAME_LEN];
    UCHAR raw[256];
    UINT  size;
    UWORD index;

    for (index = 0; index < ns->ns_IfaceCount; index++)
    {
        if (ns->ns_DhcpState[index] != (UBYTE)NX_DHCP_STATE_BOUND)
            continue;

        ami_ns_dhcp_text(ns, index, NX_DHCP_OPTION_HOST_NAME, text,
                         sizeof(text));
        if (ami_config_hostname_offer(&ns->ns_Config, (UWORD)AMI_HOSTNAME_DHCP,
                                      text))
            AMI_INFO("netstack: DHCP names this machine '%s'",
                     ns->ns_Config.hostname);

        /* Option 119 first: RFC 3397 1 calls it the list to search, and
           option 15 the single domain to fall back on. */
        size = (UINT)sizeof(raw);
        if (nx_dhcp_interface_user_option_retrieve(&ns->ns_Dhcp, (UINT)index,
                                                   AMI_DHCP_OPTION_SEARCH, raw,
                                                   &size) == NX_SUCCESS)
        {
            UWORD added = ami_config_search_from_rfc3397(
                &ns->ns_Config.resolver, (const UBYTE *)raw, (ULONG)size);

            if (added != 0)
                AMI_INFO("netstack: DHCP search list, %ld domain(s), first "
                         "'%s'", (long)added,
                         ns->ns_Config.resolver
                             .search[ns->ns_Config.resolver.search_count -
                                     added]);
        }

        ami_ns_dhcp_text(ns, index, AMI_DHCP_OPTION_DOMAIN, text,
                         sizeof(text));
        if (text[0] != '\0')
        {
            if (ami_config_search_offer(&ns->ns_Config.resolver, text))
                AMI_INFO("netstack: DHCP domain '%s'", text);

            /* Still the default domain when the file named none, which is what
               GetDefaultDomainName() reports and what SetDefaultDomainName()
               would replace. */
            if (ns->ns_Config.resolver.domain[0] == '\0')
                ami_ns_copy_name(ns->ns_Config.resolver.domain, text,
                                 sizeof(ns->ns_Config.resolver.domain));
        }

        break;
    }
}

LONG ami_netstack_dns_start(AmiNetStack *ns)
{
    UINT  status;
    UWORD i;

    if (ns == NULL || !ns->ns_IpCreated)
        return AMI_NET_ERR_STATE;

    if (ns->ns_DnsCreated)
        return AMI_NET_OK;

    /* Before nx_dns_create(), which is handed the domain. */
    if (ns->ns_DhcpStarted)
        ami_ns_dhcp_naming(ns);

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
        AMI_WARN("netstack: DNS cache not initialised (%ld), every lookup "
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

/* --------------------------------------------------------------- one query
 *
 * Each of these is one attempt for the ladder in netstack_retry.c, which
 * decides whether there is another. They take the ThreadX bracket themselves
 * rather than sharing one across the ladder, so the caller's give_up(), an
 * exec SetSignal(), for bsdsocket.library, is asked outside it.
 *
 * "Nobody answered" and "answered, but not with an address" are different
 * outcomes and only the first is worth repeating. On the unicast path that
 * distinction is thin: a blocking query in addons/dns folds every per-server
 * failure into NX_DNS_QUERY_FAILED before returning, so a name server saying
 * NXDOMAIN and a name server saying nothing arrive here identically. The
 * ladder separates them by how long the attempt took instead.
 */

typedef struct
{
    AmiNetStack *ns;
    const char  *name;
    ULONG        address;
    UINT         status;
    BOOL         nocaller;      /* the ThreadX bracket could not be taken */
} AmiNsNameAsk;

typedef struct
{
    AmiNetStack *ns;
    ULONG        address;
    char        *name_out;
    ULONG        name_len;
    UINT         status;
    BOOL         nocaller;
} AmiNsAddrAsk;

/*
 * Test whether `name` is in the .local domain.
 *
 * Case-insensitive, as DNS is (RFC 4343). A trailing dot is accepted:
 * "amiga.local." is the fully-qualified spelling of the same name.
 *
 * The bare name "local" is not in the .local domain, it is a single label
 * with no domain, and sending it to mDNS would claim a top-level name.
 */
static BOOL ami_netstack_mdns_is_local(const char *name)
{
    static const char suffix[] = ".local";
    ULONG             len;
    ULONG             slen = (ULONG)(sizeof(suffix) - 1);
    ULONG             i;

    if (name == NULL)
        return FALSE;

    for (len = 0; name[len] != '\0'; len++)
        ;

    /* One trailing dot is the root label, and is not part of the comparison. */
    if (len > 0 && name[len - 1] == '.')
        len--;

    /* Strictly longer: ".local" alone is a name with an empty label. */
    if (len <= slen)
        return FALSE;

    for (i = 0; i < slen; i++)
    {
        char a = name[len - slen + i];
        char b = suffix[i];

        if (a >= 'A' && a <= 'Z')
            a = (char)(a - 'A' + 'a');

        if (a != b)
            return FALSE;
    }

    return TRUE;
}

static AmiNetAskResult ami_ns_ask_name(VOID *arg, ULONG wait)
{
    AmiNsNameAsk *ask = (AmiNsNameAsk *)arg;
    AmiNetCaller *caller;

    caller = ami_netstack_enter_alloc();
    if (caller == NULL)
    {
        ask->nocaller = TRUE;
        return AMI_NET_ASK_REFUSED;
    }

#ifdef AMINETXDUO_IPV6
    ami_ns_dns_absorb_rdnss(ask->ns);
#endif

    ask->status = nx_dns_host_by_name_get(&ask->ns->ns_Dns,
                                          (UCHAR *)ask->name, &ask->address,
                                          wait);

    ami_netstack_leave_free(caller);

    if (ask->status == NX_SUCCESS)
        return AMI_NET_ASK_ANSWERED;

    return ami_ns_dns_again(ask->status) ? AMI_NET_ASK_SILENT
                                         : AMI_NET_ASK_REFUSED;
}

static AmiNetAskResult ami_ns_ask_addr(VOID *arg, ULONG wait)
{
    AmiNsAddrAsk *ask = (AmiNsAddrAsk *)arg;
    AmiNetCaller *caller;

    caller = ami_netstack_enter_alloc();
    if (caller == NULL)
    {
        ask->nocaller = TRUE;
        return AMI_NET_ASK_REFUSED;
    }

    ask->status = nx_dns_host_by_address_get(&ask->ns->ns_Dns, ask->address,
                                             (UCHAR *)ask->name_out,
                                             (UINT)ask->name_len, wait);

    ami_netstack_leave_free(caller);

    if (ask->status == NX_SUCCESS)
        return AMI_NET_ASK_ANSWERED;

    return ami_ns_dns_again(ask->status) ? AMI_NET_ASK_SILENT
                                         : AMI_NET_ASK_REFUSED;
}

#ifdef AMINETXDUO_MDNS
static AmiNetAskResult ami_ns_ask_mdns(VOID *arg, ULONG wait)
{
    AmiNsNameAsk *ask = (AmiNsNameAsk *)arg;
    AmiNetCaller *caller;
    LONG          err;

    caller = ami_netstack_enter_alloc();
    if (caller == NULL)
    {
        ask->nocaller = TRUE;
        return AMI_NET_ASK_REFUSED;
    }

    err = ami_netstack_mdns_resolve(ask->name, &ask->address, wait);

    ami_netstack_leave_free(caller);

    if (err == AMI_NET_OK)
        return AMI_NET_ASK_ANSWERED;

    /* Multicast has no server to refuse, so silence is the only failure the
       wire can produce and re-asking is right. A responder that is not running
       at all fails immediately, which the ladder reads as an answer. */
    return AMI_NET_ASK_SILENT;
}
#endif

/* -------------------------------------------------------------- public API */

/* One lookup of exactly the name given, with as many queries as it takes. */
static LONG ami_ns_resolve_once(const char *name, ULONG *addr_out,
                                ULONG timeout_ticks, AmiNetGiveUpFn give_up,
                                VOID *give_up_arg)
{
    AmiNetStack         *ns = ami_netstack_raw();
    const AmiNetdbEntry *entry;
    AmiNsNameAsk         ask;
    AmiNetLadderResult   done;

    /* DEVS:Internet/hosts first, it must work with the network down. */
    entry = ami_netdb_host_by_name(name);
    if (entry != NULL)
    {
        *addr_out = entry->value;
        return AMI_NET_OK;
    }

    if (ns == NULL || !ns->ns_DnsCreated)
        return AMI_NET_ERR_STATE;

    ask.ns       = ns;
    ask.name     = name;
    ask.address  = 0;
    ask.status   = NX_DNS_QUERY_FAILED;
    ask.nocaller = FALSE;

#ifdef AMINETXDUO_MDNS
    /*
     * RFC 6762 6.7 requires a name ending in ".local" to be sent to
     * 224.0.0.251 and never to a unicast DNS server. Many home routers answer
     * any name with their own NXDOMAIN-substitute search page, and some
     * forward .local to the internet where somebody else's server answers, so
     * the branch is exclusive: no mDNS answer means the name does not exist.
     *
     * The check lives here rather than in a new command because every name an
     * AmigaOS program looks up arrives at this function, gethostbyname() and
     * getaddrinfo() in src/bsdsocket/ both route through it, so
     * `host amiga.local`, `ping amiga.local` and `fetch http://amiga.local/`
     * work unchanged, as does any Roadshow-era program.
     *
     * The hosts file above still wins: a name pinned in DEVS:Internet/hosts
     * outranks anything the network claims, .local included.
     */
    if (ami_netstack_mdns_is_local(name))
    {
        done = ami_net_ask_until(ami_ns_ask_mdns, &ask, timeout_ticks, give_up,
                                 give_up_arg);

        if (done == AMI_NET_LADDER_ANSWERED)
        {
            *addr_out = ask.address;
            return AMI_NET_OK;
        }

        if (done == AMI_NET_LADDER_ABORTED)
            return AMI_NET_ERR_ABORTED;
        if (ask.nocaller)
            return AMI_NET_ERR_KERNEL;

        AMI_INFO("netstack: nothing on this network answers to '%s'", name);

        return AMI_NET_ERR_NONAME;
    }
#else
    /*
     * No responder in this build, so nothing can answer a .local name, but
     * RFC 6762 3 still forbids asking a unicast server, and that is what
     * happened: the whole test sat inside the #ifdef, so the floor tier
     * published every local host name it looked up to whatever server DHCP
     * handed it.
     */
    if (ami_netstack_mdns_is_local(name))
        return AMI_NET_ERR_NONAME;
#endif

    done = ami_net_ask_until(ami_ns_ask_name, &ask, timeout_ticks, give_up,
                             give_up_arg);

    if (done == AMI_NET_LADDER_ANSWERED)
    {
        *addr_out = ask.address;
        return AMI_NET_OK;
    }

    if (done == AMI_NET_LADDER_ABORTED)
        return AMI_NET_ERR_ABORTED;
    if (ask.nocaller)
        return AMI_NET_ERR_KERNEL;

    if (done == AMI_NET_LADDER_SILENT)
    {
        AMI_INFO("netstack: no name server answered about '%s'", name);
        return AMI_NET_ERR_TIMEOUT;
    }

    AMI_INFO("netstack: '%s' not resolved (DNS status %ld)", name,
             (long)ask.status);

    return ami_ns_dns_error(ask.status);
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
       would give "host.", neither is the name the caller meant. */
    if (dst[n - 1] == '.')
        return FALSE;

    dst[n] = '\0';

    return TRUE;
}

LONG netstack_resolve_until(const char *name, ULONG *addr_out,
                            ULONG timeout_ticks, AmiNetGiveUpFn give_up,
                            VOID *give_up_arg)
{
    AmiNetStack *ns = ami_netstack_raw();
    const char  *suffix[AMI_CFG_SEARCH_LIST_MAX];
    char         qualified[AMI_DNS_NAME_MAX];
    LONG         err;
    UWORD        count;
    UWORD        i;

    if (name == NULL || *name == '\0' || addr_out == NULL)
        return AMI_NET_ERR_CONFIG;

    err = ami_ns_resolve_once(name, addr_out, timeout_ticks, give_up,
                              give_up_arg);
    if (err == AMI_NET_OK)
        return err;

    /*
     * "If no domain name is part of a host name, a default domain name can be
     * added to it if the host name lookup fails", GetDefaultDomainName().
     * So `ping fileserver` reaches fileserver.lan.
     *
     * Only after a definite no: TIMEOUT and NOSERVER say nothing about the
     * name, and a second query would just double the wait. ERR_STATE is worth
     * a retry even so, because with the stack down the retry never reaches the
     * network, it can only hit DEVS:Internet/hosts, which costs nothing.
     * ABORTED is the caller leaving, and must not start another lookup.
     */
    if (err != AMI_NET_ERR_NONAME && err != AMI_NET_ERR_STATE)
        return err;

    if (ns == NULL || !ami_ns_unqualified(name))
        return err;

    count = ami_config_search_list(&ns->ns_Config.resolver, suffix,
                                   (UWORD)AMI_CFG_SEARCH_LIST_MAX);

    for (i = 0; i < count; i++)
    {
        LONG next;

        if (!ami_ns_join_domain(qualified, (ULONG)sizeof(qualified), name,
                                suffix[i]))
            continue;

        next = ami_ns_resolve_once(qualified, addr_out, timeout_ticks, give_up,
                                   give_up_arg);
        if (next == AMI_NET_OK)
            return AMI_NET_OK;

        /* Same rule that let the first suffix be tried at all, applied to the
           next one: only a definite no is worth spending another query on, so
           a search list costs one round trip per entry against a server that
           answers and nothing at all against one that does not. */
        if (next != AMI_NET_ERR_NONAME && next != AMI_NET_ERR_STATE)
            break;
    }

    /* The caller asked about the bare name. Whatever the speculative retries
       ran into is not an answer about that name, so report the first failure. */
    return err;
}

LONG netstack_resolve(const char *name, ULONG *addr_out, ULONG timeout_ticks)
{
    return netstack_resolve_until(name, addr_out, timeout_ticks, NULL, NULL);
}

LONG netstack_resolve_reverse_until(ULONG addr, char *name_out, ULONG name_len,
                                    ULONG timeout_ticks,
                                    AmiNetGiveUpFn give_up, VOID *give_up_arg)
{
    AmiNetStack         *ns = ami_netstack_raw();
    const AmiNetdbEntry *entry;
    AmiNsAddrAsk         ask;
    AmiNetLadderResult   done;

    if (name_out == NULL || name_len == 0)
        return AMI_NET_ERR_CONFIG;

    entry = ami_netdb_host_by_addr(addr);
    if (entry != NULL)
    {
        ami_ns_copy_name(name_out, entry->name, name_len);
        return AMI_NET_OK;
    }

    /*
     * RFC 6762 3, the other direction: 254.169.in-addr.arpa is reserved for
     * link-local multicast, so a reverse lookup of a 169.254/16 address must
     * not go to a unicast server.  It cannot be answered by one either,
     * nobody is authoritative for another machine's self-assigned address,
     * so the query only tells that server which link-local addresses this
     * machine is talking to, and then times out.
     *
     * The immediate negative is also the fast answer: ShowNetStatus NAMES and
     * netstat reverse every address on screen, and on a network that fell back
     * to link-local that was one full DNS timeout per line.
     */
    if ((addr & 0xFFFF0000UL) == 0xA9FE0000UL)
        return AMI_NET_ERR_NONAME;

    if (ns == NULL || !ns->ns_DnsCreated)
        return AMI_NET_ERR_STATE;

    ask.ns       = ns;
    ask.address  = addr;
    ask.name_out = name_out;
    ask.name_len = name_len;
    ask.status   = NX_DNS_QUERY_FAILED;
    ask.nocaller = FALSE;

    done = ami_net_ask_until(ami_ns_ask_addr, &ask, timeout_ticks, give_up,
                             give_up_arg);

    if (done == AMI_NET_LADDER_ANSWERED)
        return AMI_NET_OK;
    if (done == AMI_NET_LADDER_ABORTED)
        return AMI_NET_ERR_ABORTED;
    if (ask.nocaller)
        return AMI_NET_ERR_KERNEL;
    if (done == AMI_NET_LADDER_SILENT)
        return AMI_NET_ERR_TIMEOUT;

    return ami_ns_dns_error(ask.status);
}

LONG netstack_resolve_reverse(ULONG addr, char *name_out, ULONG name_len,
                              ULONG timeout_ticks)
{
    return netstack_resolve_reverse_until(addr, name_out, name_len,
                                          timeout_ticks, NULL, NULL);
}

#ifdef AMINETXDUO_IPV6
/*
 * answer is aligned by hand, and the attribute is the mechanism rather than a
 * hint.
 *
 * nxd_dns.h says of the record buffer: "The return_buffer must be 4-byte
 * aligned", and _nxde_dns_ipv6_address_by_name_get() enforces it -- an address
 * 2 mod 4 is refused with NX_PTR_ERROR before a query is built, let alone
 * sent.  On m68k nothing in the language reaches 4: __alignof__(ULONG) is 2,
 * so this struct's alignment is 2 and an instance of it on the stack lands
 * 2 mod 4 whenever the frame does.
 *
 * That is what it did.  getaddrinfo() asks AAAA first and appends it ahead of
 * the A record, so the ordering was never the problem; the AAAA lookup was
 * refused by argument checking, the caller could not tell that apart from "the
 * name has no AAAA", and every name resolved IPv4 on a machine with a working
 * global IPv6 address.  nslookup builds its own query and never came through
 * here, which is why it could report the AAAA the resolver said did not exist.
 *
 * docs/ALIGNMENT.md has the same defect twice before, in CMSG_BUFFER() and in
 * bsd_hostent_pack().
 */
typedef struct
{
    AmiNetStack        *ns;
    const char         *name;
    NX_DNS_IPV6_ADDRESS answer[1] __attribute__((aligned(4)));
    UINT                count;
    UINT                status;
    BOOL                nocaller;
} AmiNsName6Ask;

_Static_assert(__alignof__(AmiNsName6Ask) >= 4,
               "the DNS record buffer must be 4-byte aligned");
_Static_assert((offsetof(AmiNsName6Ask, answer) & 3U) == 0,
               "the DNS record buffer must be 4-byte aligned");

static AmiNetAskResult ami_ns_ask_name6(VOID *arg, ULONG wait)
{
    AmiNsName6Ask *ask = (AmiNsName6Ask *)arg;
    AmiNetCaller  *caller;

    caller = ami_netstack_enter_alloc();
    if (caller == NULL)
    {
        ask->nocaller = TRUE;
        return AMI_NET_ASK_REFUSED;
    }

    ami_ns_dns_absorb_rdnss(ask->ns);

    ask->count  = 0;
    ask->status = nxd_dns_ipv6_address_by_name_get(&ask->ns->ns_Dns,
                                                   (UCHAR *)ask->name,
                                                   ask->answer,
                                                   (UINT)sizeof(ask->answer),
                                                   &ask->count, wait);

    ami_netstack_leave_free(caller);

    if (ask->status == NX_SUCCESS)
        return (ask->count != 0) ? AMI_NET_ASK_ANSWERED : AMI_NET_ASK_REFUSED;

    return ami_ns_dns_again(ask->status) ? AMI_NET_ASK_SILENT
                                         : AMI_NET_ASK_REFUSED;
}

static LONG ami_ns_resolve6_once(const char *name, ULONG addr_out[4],
                                 ULONG timeout_ticks, AmiNetGiveUpFn give_up,
                                 VOID *give_up_arg)
{
    AmiNetStack       *ns = ami_netstack_raw();
    AmiNsName6Ask      ask;
    AmiNetLadderResult done;

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

    /*
     * The IPv4 path routes a .local name to mDNS; this one had no test at all
     * and handed it straight to the configured server.  Refused rather than
     * routed: the vendored module's IPv6 half is not enabled in this build
     * (ami_ns_mdns_lookup() says why), so there is nothing to ask.
     */
    if (ami_netstack_mdns_is_local(name))
        return AMI_NET_ERR_NONAME;

    ask.ns       = ns;
    ask.name     = name;
    ask.count    = 0;
    ask.status   = NX_DNS_QUERY_FAILED;
    ask.nocaller = FALSE;

    done = ami_net_ask_until(ami_ns_ask_name6, &ask, timeout_ticks, give_up,
                             give_up_arg);

    if (done == AMI_NET_LADDER_ABORTED)
        return AMI_NET_ERR_ABORTED;
    if (ask.nocaller)
        return AMI_NET_ERR_KERNEL;
    if (done == AMI_NET_LADDER_SILENT)
        return AMI_NET_ERR_TIMEOUT;
    if (done != AMI_NET_LADDER_ANSWERED)
        return (ask.status == NX_SUCCESS) ? AMI_NET_ERR_NONAME
                                          : ami_ns_dns_error(ask.status);

    addr_out[0] = ask.answer[0].ipv6_address[0];
    addr_out[1] = ask.answer[0].ipv6_address[1];
    addr_out[2] = ask.answer[0].ipv6_address[2];
    addr_out[3] = ask.answer[0].ipv6_address[3];

    return AMI_NET_OK;
}

/* The same search list as the IPv4 side, for the same reason: getaddrinfo()
   asks this first, and a short name that resolves to an AAAA record and not an
   A record was unreachable while only the IPv4 half qualified it. */
LONG netstack_resolve6_until(const char *name, ULONG addr_out[4],
                             ULONG timeout_ticks, AmiNetGiveUpFn give_up,
                             VOID *give_up_arg)
{
    AmiNetStack *ns = ami_netstack_raw();
    const char  *suffix[AMI_CFG_SEARCH_LIST_MAX];
    char         qualified[AMI_DNS_NAME_MAX];
    LONG         err;
    UWORD        count;
    UWORD        i;

    if (name == NULL || *name == '\0' || addr_out == NULL)
        return AMI_NET_ERR_CONFIG;

    err = ami_ns_resolve6_once(name, addr_out, timeout_ticks, give_up,
                               give_up_arg);
    if (err == AMI_NET_OK)
        return err;

    if (err != AMI_NET_ERR_NONAME && err != AMI_NET_ERR_STATE)
        return err;

    if (ns == NULL || !ami_ns_unqualified(name))
        return err;

    count = ami_config_search_list(&ns->ns_Config.resolver, suffix,
                                   (UWORD)AMI_CFG_SEARCH_LIST_MAX);

    for (i = 0; i < count; i++)
    {
        LONG next;

        if (!ami_ns_join_domain(qualified, (ULONG)sizeof(qualified), name,
                                suffix[i]))
            continue;

        next = ami_ns_resolve6_once(qualified, addr_out, timeout_ticks, give_up,
                                    give_up_arg);
        if (next == AMI_NET_OK)
            return AMI_NET_OK;

        if (next != AMI_NET_ERR_NONAME && next != AMI_NET_ERR_STATE)
            break;
    }

    return err;
}

LONG netstack_resolve6(const char *name, ULONG addr_out[4], ULONG timeout_ticks)
{
    return netstack_resolve6_until(name, addr_out, timeout_ticks, NULL, NULL);
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
 * reports it, negative for a server from DEVS:Internet/name_resolution,
 * positive for one DHCP or this call put there. Adding to a static entry keeps
 * it static and deepens it (-1 -> -2); the entry only leaves the list when the
 * count reaches zero. NetX Duo's own list does not count, so it is touched
 * only on the first add and the last remove.
 */

/* A slot in use never stores 0; one that does predates the count and is the
   file's own, as ObtainDomainNameServerList() also reads it. */
static LONG ami_ns_use(LONG stored)
{
    return (stored != 0) ? stored : -1;
}

static LONG ami_ns_use_deepen(LONG stored)
{
    LONG use = ami_ns_use(stored);

    return (use < 0) ? (use - 1) : (use + 1);
}

static LONG ami_ns_use_shallow(LONG stored)
{
    LONG use = ami_ns_use(stored);

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
       length is checked before anything is stored, writing the truncated
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
