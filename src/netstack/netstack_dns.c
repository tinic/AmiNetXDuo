/*
 * AmiNetXDuo, name resolution.  DEVS:Internet/hosts wins over the network,
 * as in any BSD resolver.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_internal.h"
#include "netstack_dns_domain.h"
#include "netstack_dns_status.h"
#include "netstack_retry.h"

#include <proto/exec.h>
#include <stddef.h>

/* RFC 1035 2.3.4: 255 octets of domain name, plus the NUL. */
#define AMI_DNS_NAME_MAX    256

/*
 * The absorb runs on the caller's stack -- a bsdsocket.library vector runs on
 * its caller's, and an AmigaDOS Shell hands a command 4096 bytes with no MMU
 * -- so the option buffers live in one AllocMem() block per absorb instead.
 */
typedef struct AmiNsDnsScratch {
    AmiResolverConfig offered;                  /* the lease's search list  */
    UCHAR             search_raw[256];          /* option 119, RFC 3397     */
    char              search_text[AMI_CFG_DOMAIN_LEN];
    UCHAR             domain_raw[AMI_CFG_DOMAIN_LEN];   /* option 15        */
    char              domain_text[AMI_CFG_DOMAIN_LEN];
    UCHAR             host_raw[AMI_CFG_NAME_LEN];       /* option 12        */
    char              host_text[AMI_CFG_NAME_LEN];
    ULONG             offered_dns[AMI_CFG_MAX_NAMESERVERS];
    ULONG             raw_dns[NX_DNS_MAX_SERVERS];      /* option 6         */
} AmiNsDnsScratch;

static VOID *ami_ns_dns_scratch_alloc(ULONG size)
{
    VOID *block = AllocMem(size, MEMF_PUBLIC | MEMF_CLEAR);

    if (block != NULL)
        AMI_CENSUS_ADD(block, size);

    return block;
}

static VOID ami_ns_dns_scratch_free(VOID *block, ULONG size)
{
    if (block == NULL)
        return;

    AMI_CENSUS_DROP(block);
    FreeMem(block, size);
}

/*
 * ns_Config.resolver is live configuration, so short Forbid()/Permit() sections
 * cover copies and mutations of the stored half.  They must never surround a
 * NetX call: an Exec Wait while holding the ThreadX baton stops the stack.
 */
static VOID ami_ns_resolver_forbid(VOID)
{
    Forbid();
}

static VOID ami_ns_resolver_permit(VOID)
{
    Permit();
}

static BOOL ami_ns_domain_same(const char *a, const char *b)
{
    char ca;
    char cb;

    do
    {
        ca = *a++;
        cb = *b++;
        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z')
            cb = (char)(cb + ('a' - 'A'));
    } while (ca == cb && ca != '\0');

    return (BOOL)(ca == cb);
}

/*
 * Only NX_DHCP_NOT_BOUND / NX_DHCP_INTERFACE_NOT_ENABLED can read differently
 * on a later pass; the buffer and the option size do not change within a
 * lease, so NX_DHCP_DEST_TO_SMALL is a refusal and not a retry.
 */
static AmiNsDnsOptionRead ami_ns_dhcp_option_read(UINT status)
{
    if (status == NX_SUCCESS)
        return AMI_NS_DNS_OPTION_READ;
    if (status == NX_DHCP_PARSE_ERROR)
        return AMI_NS_DNS_OPTION_ABSENT;
    if (status == NX_DHCP_DEST_TO_SMALL)
        return AMI_NS_DNS_OPTION_REFUSED;

    return AMI_NS_DNS_OPTION_FAILED;
}

/*
 * Option 15.  TRUE means out[] is this lease's coherent answer, empty or not;
 * FALSE means nothing was read and the interface wants another pass.
 */
static BOOL ami_ns_dhcp_domain_option(AmiNetStack *ns, UWORD iface,
                                      AmiNsDnsScratch *sc,
                                      char out[AMI_CFG_DOMAIN_LEN])
{
    UCHAR             *raw = sc->domain_raw;
    UINT               size = (UINT)sizeof(sc->domain_raw);
    AmiNsDnsOptionRead read;
    UINT               i;

    out[0] = '\0';
    read = ami_ns_dhcp_option_read(nx_dhcp_interface_user_option_retrieve(
        &ns->ns_Dhcp, (UINT)iface, AMI_DHCP_OPTION_DOMAIN, raw, &size));

    if (read == AMI_NS_DNS_OPTION_REFUSED)
        return TRUE;            /* longer than a name we would use */
    if (!ami_ns_dns_option_usable(read))
        return FALSE;
    if (read == AMI_NS_DNS_OPTION_ABSENT)
        return TRUE;            /* this lease carries no option 15 */
    if (size == 0U || size >= (UINT)AMI_CFG_DOMAIN_LEN)
        return TRUE;            /* invalid means no usable offer */

    for (i = 0; i < size; i++)
    {
        if (raw[i] == 0U)
        {
            out[0] = '\0';
            return TRUE;        /* embedded NUL is not option-15 text */
        }
        out[i] = (char)raw[i];
    }
    out[size] = '\0';
    if (!ami_ns_domain_canonicalize(out))
        out[0] = '\0';
    return TRUE;
}

/* Option 12, with the same contract as the option 15 reader above. */
static BOOL ami_ns_dhcp_hostname_option(AmiNetStack *ns, UWORD iface,
                                        AmiNsDnsScratch *sc,
                                        char out[AMI_CFG_NAME_LEN])
{
    UCHAR             *raw = sc->host_raw;
    UINT               size = (UINT)sizeof(sc->host_raw);
    AmiNsDnsOptionRead read;

    out[0] = '\0';
    read = ami_ns_dhcp_option_read(nx_dhcp_interface_user_option_retrieve(
        &ns->ns_Dhcp, (UINT)iface, NX_DHCP_OPTION_HOST_NAME, raw, &size));
    if (read == AMI_NS_DNS_OPTION_REFUSED)
        return TRUE;            /* too long for a host name is no offer */
    if (!ami_ns_dns_option_usable(read))
        return FALSE;
    if (read == AMI_NS_DNS_OPTION_ABSENT)
        return TRUE;            /* this lease carries no option 12 */

    (VOID)ami_ns_dhcp_hostname_decode(out, raw, (ULONG)size);
    return TRUE;
}

static BOOL ami_ns_search_array_has(
    const char domain[AMI_CFG_MAX_SEARCH][AMI_CFG_NAME_LEN], UWORD count,
    const char *value);

#ifdef AMINETXDUO_IPV6

/*
 * The RFC 8106 half of the resolver.  The advertisement arrives on the IP
 * thread, which must not call the DNS client -- nxd_dns_server_add() waits on
 * a mutex a query holds -- so the callback records and the next lookup absorbs.
 */
static BOOL ami_ns6_same(const ULONG a[4], const ULONG b[4])
{
    return (BOOL)(a[0] == b[0] && a[1] == b[1] &&
                  a[2] == b[2] && a[3] == b[3]);
}

static VOID ami_ns6_nxd(NXD_ADDRESS *out, const ULONG addr[AMI_CFG_IP6_WORDS])
{
    out->nxd_ip_version       = NX_IP_VERSION_V6;
    out->nxd_ip_address.v6[0] = addr[0];
    out->nxd_ip_address.v6[1] = addr[1];
    out->nxd_ip_address.v6[2] = addr[2];
    out->nxd_ip_address.v6[3] = addr[3];
}

/*
 * OWNERSHIP OF AN IPv6 NAME SERVER, the pair ami_ns_dns_reference_add() and
 * ami_ns_dns_reference_remove() below are for the IPv4 half.
 *
 * A router advertisement, DHCPv6 and a Roadshow caller can each name the same
 * server, and the entry has to outlive any one of them leaving.  The count is
 * nameserver6_use[], which ObtainDomainNameServerList() reports as
 * dnsn_UseCount: the first owner creates the entry, later owners deepen the
 * count, and only the last one to go takes the entry out of the DNS client.
 *
 * `nx_status` and `dropped` may be NULL.  `dropped` says the entry left the
 * list rather than losing one of several owners, which is what the callers log.
 */
static BOOL ami_ns_dns6_reference_add(AmiNetStack *ns,
                                      const ULONG server[AMI_CFG_IP6_WORDS],
                                      UINT *nx_status)
{
    AmiResolverConfig *r = &ns->ns_Config.resolver;
    NXD_ADDRESS        address;
    UINT               status;
    UWORD              i;

    if (nx_status != NULL)
        *nx_status = NX_SUCCESS;

    for (i = 0; i < r->nameserver6_count; i++)
        if (ami_ns6_same(r->nameserver6[i], server))
        {
            ami_ns_resolver_forbid();
            r->nameserver6_use[i] =
                ami_ns_dns_use_deepen(r->nameserver6_use[i]);
            ami_ns_resolver_permit();
            return TRUE;
        }

    if (r->nameserver6_count >= (UWORD)AMI_CFG_MAX_NAMESERVERS)
        return FALSE;

    ami_ns6_nxd(&address, server);

    status = nxd_dns_server_add(&ns->ns_Dns, &address);
    if (nx_status != NULL)
        *nx_status = status;
    if (status != NX_SUCCESS && status != NX_DNS_DUPLICATE_ENTRY)
        return FALSE;

    ami_ns_resolver_forbid();
    (VOID)ami_config_nameserver6_offer(r, server);
    ami_ns_resolver_permit();
    return TRUE;
}

static BOOL ami_ns_dns6_reference_remove(AmiNetStack *ns,
                                         const ULONG server[AMI_CFG_IP6_WORDS],
                                         BOOL *dropped, UINT *nx_status)
{
    AmiResolverConfig *r = &ns->ns_Config.resolver;
    NXD_ADDRESS        address;
    UINT               status;
    UWORD              at;
    LONG               use;

    if (nx_status != NULL)
        *nx_status = NX_SUCCESS;
    if (dropped != NULL)
        *dropped = FALSE;

    for (at = 0; at < r->nameserver6_count; at++)
        if (ami_ns6_same(r->nameserver6[at], server))
            break;

    /* Nothing of ours to give back.  Not a failure: a source withdrawing a
       server it never got installed is the ordinary case after a rejection. */
    if (at == r->nameserver6_count)
        return TRUE;

    use = ami_ns_dns_use_shallow(r->nameserver6_use[at]);
    if (use != 0)
    {
        ami_ns_resolver_forbid();
        r->nameserver6_use[at] = use;
        ami_ns_resolver_permit();
        return TRUE;
    }

    ami_ns6_nxd(&address, server);

    status = nxd_dns_server_remove(&ns->ns_Dns, &address);
    if (nx_status != NULL)
        *nx_status = status;
    if (status != NX_SUCCESS && status != NX_DNS_SERVER_NOT_FOUND)
        return FALSE;

    ami_ns_resolver_forbid();
    (VOID)ami_config_nameserver6_withdraw(r, server);
    ami_ns_resolver_permit();

    if (dropped != NULL)
        *dropped = TRUE;
    return TRUE;
}

VOID ami_ns6_rdnss(NX_IP *ip_ptr, UINT interface_index, ULONG *dns_address,
                   ULONG lifetime)
{
    AmiNetStack *ns = ami_netstack_raw();

    (VOID)ip_ptr;
    if (ns == NULL || dns_address == NULL)
        return;

    ami_ns_ra_rdnss(&ns->ns_Ra, (UWORD)interface_index, dns_address,
                    lifetime, tx_time_get());
}

/*
 * The RFC 8106 5.2 half, the suffixes rather than the servers.  Same thread and
 * the same reason for not acting here.  The bytes rather than the names, so a
 * parser reached straight off the network does not run on the IP thread.
 */
VOID ami_ns6_dnssl(NX_IP *ip_ptr, UINT interface_index, UCHAR *domains,
                   UINT length, ULONG lifetime)
{
    AmiNetStack *ns = ami_netstack_raw();

    (VOID)ip_ptr;
    if (ns == NULL || domains == NULL || length == 0)
        return;

    ami_ns_ra_dnssl(&ns->ns_Ra, (UWORD)interface_index, domains, length,
                    lifetime, tx_time_get());
}

/*
 * DHCPv6 servers go ahead of RFC 8106 RDNSS ones: the RFC 4861 4.2 O flag is
 * the router delegating configuration to DHCPv6.  Both are kept, and neither
 * source may withdraw the other's -- hence the ns_Dhcpv6Dns[] cross-checks.
 */
static BOOL ami_ns_dns_v6_list_names(const NXD_ADDRESS *servers, UWORD count,
                                     const ULONG addr[AMI_CFG_IP6_WORDS])
{
    UWORD i;

    for (i = 0; i < count; i++)
    {
        if (ami_ns6_same(addr, servers[i].nxd_ip_address.v6))
            return TRUE;
    }

    return FALSE;
}

static BOOL ami_ns_dns_dhcpv6_names(const AmiNetStack *ns,
                                    const ULONG addr[AMI_CFG_IP6_WORDS])
{
    return ami_ns_dns_v6_list_names(ns->ns_Dhcpv6Dns,
                                    ns->ns_Dhcpv6DnsCount, addr);
}

/*
 * One scratch block serves both absorbs: they run on the caller's stack, each
 * carries an AmiResolverConfig, and they are called one after the other.
 */
typedef struct AmiNsDns6Scratch {
    NXD_ADDRESS       offered[AMI_RDNSS_MAX];
    UCHAR             names[NX_DHCPV6_DOMAIN_NAME_BUFFER_SIZE];
    AmiResolverConfig search;
    AmiNsRaSnapshot   ra;
    char              text[AMI_CFG_IP6_STRLEN];
} AmiNsDns6Scratch;

/*
 * A Reply has landed.  Read on a caller thread and not on the client's own:
 * the DNS client holds its mutex across a query and that query needs the IP
 * thread.
 */
static VOID ami_ns_dns_absorb_dhcpv6(AmiNetStack *ns, AmiNsDns6Scratch *sc)
{
    AmiResolverConfig *r;
    NXD_ADDRESS       *offered = sc->offered;
    UWORD              offered_count = 0;
    UCHAR             *names = sc->names;
    char              *text = sc->text;
    UINT               index;
    UINT               option_status = NX_SUCCESS;
    ULONG              now;
    BOOL               options_valid;
    BOOL               names_valid = TRUE;

    if (!ns->ns_Dhcpv6DnsPending)
        return;

    ns->ns_Dhcpv6DnsPending = FALSE;

    /*
     * ns_Dhcpv6Created as well as the options flag: the mutex below belongs to
     * the client object, so absorbing after a delete would wait on a mutex that
     * is not there.
     */
    options_valid = (BOOL)(ns->ns_Dhcpv6OptionsValid && ns->ns_Dhcpv6Created);

    r = &ns->ns_Config.resolver;
    now = tx_time_get();
    memset(names, 0, sizeof(sc->names));

    /*
     * Snapshot both option families under one client lock, so a renewal cannot
     * splice the servers from one Reply to the domains from another.
     */
    if (options_valid)
    {
        if (tx_mutex_get(&ns->ns_Dhcpv6.nx_dhcpv6_client_mutex,
                         TX_WAIT_FOREVER) != TX_SUCCESS)
        {
            /*
             * TX_WAIT_ABORTED says nothing about the next wait.  Nothing has been
             * read and nothing changed, so the whole absorb is asked for again.
             */
            ns->ns_Dhcpv6DnsPending = TRUE;
            return;
        }

        options_valid = ns->ns_Dhcpv6OptionsValid;
        if (options_valid)
        {
            for (index = 0; index < (UINT)NX_DHCPV6_NUM_DNS_SERVERS;
                 index++)
            {
                NXD_ADDRESS server;

                if (nx_dhcpv6_get_DNS_server_address(
                        &ns->ns_Dhcpv6, index, &server) != NX_SUCCESS)
                    continue;

                if ((server.nxd_ip_address.v6[0] |
                     server.nxd_ip_address.v6[1] |
                     server.nxd_ip_address.v6[2] |
                     server.nxd_ip_address.v6[3]) == 0UL)
                    continue;

                if (offered_count < (UWORD)AMI_RDNSS_MAX &&
                    !ami_ns_dns_v6_list_names(offered, offered_count,
                                               server.nxd_ip_address.v6))
                    offered[offered_count++] = server;
            }

            option_status = nx_dhcpv6_get_other_option_data(
                &ns->ns_Dhcpv6, NX_DHCPV6_DOMAIN_NAME_OPTION,
                names, sizeof(sc->names));
        }

        (VOID)tx_mutex_put(&ns->ns_Dhcpv6.nx_dhcpv6_client_mutex);

        /*
         * The domain list is the only thing this can have lost, so it is the only
         * thing skipped.  names[] stays as memset() left it: an empty list would be
         * read as a withdrawal.
         */
        if (option_status != NX_SUCCESS)
        {
            names_valid = FALSE;
            AMI_WARN("netstack: DHCPv6 domain list not readable (%ld), "
                     "keeping the search list already in use",
                     (long)option_status);
        }
    }

    /* Out: a later valid Reply may shorten the list or omit the option. A
       server still owned by RDNSS remains in both resolver views. */
    for (index = ns->ns_Dhcpv6DnsCount; index-- != 0U; )
    {
        NXD_ADDRESS server = ns->ns_Dhcpv6Dns[index];
        UINT        status = NX_SUCCESS;
        BOOL        dropped = FALSE;

        if (ami_ns_dns_v6_list_names(offered, offered_count,
                                     server.nxd_ip_address.v6) ||
            ami_ns_ra_rdnss_has(&ns->ns_Ra, server.nxd_ip_address.v6, now))
            continue;

        ami_config_format_ip6(server.nxd_ip_address.v6, text,
                              sizeof(sc->text));

        if (!ami_ns_dns6_reference_remove(ns, server.nxd_ip_address.v6,
                                          &dropped, &status))
        {
            AMI_WARN("netstack: DHCPv6 name server %s withdrawal failed "
                     "(%ld)", text, (long)status);
            ns->ns_Dhcpv6DnsPending = TRUE;
            return;
        }

        /* Kept, because a Roadshow caller still holds it.  Its dnsn_UseCount
           went down by one and the server keeps answering. */
        if (dropped)
            AMI_INFO("netstack: DHCPv6 name server %s withdrawn", text);
    }

    memset(ns->ns_Dhcpv6Dns, 0, sizeof(ns->ns_Dhcpv6Dns));
    for (index = 0; index < offered_count; index++)
        ns->ns_Dhcpv6Dns[index] = offered[index];
    ns->ns_Dhcpv6DnsCount = offered_count;

    /* In: the DNS client first, the configuration only if that worked. */
    for (index = 0; index < offered_count; index++)
    {
        NXD_ADDRESS server = offered[index];
        UINT        status;

        status = nxd_dns_server_add(&ns->ns_Dns, &server);

        ami_config_format_ip6(server.nxd_ip_address.v6, text,
                              sizeof(sc->text));

        if (status != NX_SUCCESS && status != NX_DNS_DUPLICATE_ENTRY)
        {
            AMI_WARN("netstack: DHCPv6 name server %s rejected (%ld)", text,
                     (long)status);
            continue;
        }

        {
            BOOL offered;

            ami_ns_resolver_forbid();
            offered = ami_config_nameserver6_offer(r,
                                                    server.nxd_ip_address.v6);
            ami_ns_resolver_permit();

            if (offered)
                AMI_INFO("netstack: DHCPv6 name server %s", text);
        }
    }

    /*
     * The client has already decoded the RFC 1035 label sequences, so this is not
     * the ami_config_search_from_rfc3397() path.  Skipped whole when the list
     * could not be read: the loops below are the withdrawal as well as the offer.
     */
    if (names_valid)
    {
        AmiResolverConfig *offered = &sc->search;
        ULONG             pos = 0;
        UWORD             added = 0;
        UWORD             removed = 0;
        UWORD             i;

        memset(offered, 0, sizeof(*offered));

        while (pos < (ULONG)sizeof(sc->names) && names[pos] != '\0')
        {
            (VOID)ami_config_search_offer(offered,
                                          (const char *)&names[pos]);
            while (pos < (ULONG)sizeof(sc->names) && names[pos] != '\0')
                pos++;
            pos++;
        }

        /* Out: release only DHCPv6's reference. An identical static, DHCPv4
           or RA suffix remains live through its own reference. */
        for (i = ns->ns_Dhcpv6SearchAppliedCount; i-- != 0U; )
        {
            UWORD j;

            if (ami_ns_search_array_has(offered->search,
                                        offered->search_count,
                                        ns->ns_Dhcpv6SearchApplied[i]))
                continue;

            ami_ns_resolver_forbid();
            if (ami_config_search_reference_remove(
                    r, ns->ns_Dhcpv6SearchApplied[i]))
                removed++;
            ami_ns_resolver_permit();

            for (j = (UWORD)(i + 1U);
                 j < ns->ns_Dhcpv6SearchAppliedCount; j++)
                ami_ns_copy_name(ns->ns_Dhcpv6SearchApplied[j - 1U],
                                 ns->ns_Dhcpv6SearchApplied[j],
                                 AMI_CFG_NAME_LEN);
            ns->ns_Dhcpv6SearchAppliedCount--;
            ns->ns_Dhcpv6SearchApplied
                [ns->ns_Dhcpv6SearchAppliedCount][0] = '\0';
        }

        /* In: acquire one reference for each suffix in the replacement set. */
        for (i = 0; i < offered->search_count; i++)
        {
            BOOL accepted;

            if (ami_ns_search_array_has(ns->ns_Dhcpv6SearchApplied,
                                        ns->ns_Dhcpv6SearchAppliedCount,
                                        offered->search[i]))
                continue;

            ami_ns_resolver_forbid();
            accepted = ami_config_search_reference_add(r,
                                                       offered->search[i]);
            ami_ns_resolver_permit();
            if (!accepted)
                continue;

            ami_ns_copy_name(
                ns->ns_Dhcpv6SearchApplied
                    [ns->ns_Dhcpv6SearchAppliedCount],
                offered->search[i], AMI_CFG_NAME_LEN);
            ns->ns_Dhcpv6SearchAppliedCount++;
            added++;
        }

        ami_ns_dns_dhcpv6_default_update(
            &ns->ns_DhcpDomain,
            ns->ns_Dhcpv6SearchAppliedCount != 0U
                ? ns->ns_Dhcpv6SearchApplied[0] : NULL);
        ami_ns_resolver_forbid();
        ami_ns_dns_dhcp_default_reconcile(
            r, &ns->ns_DhcpDomain, ns->ns_DnsslDefault,
            ns->ns_DnsslApplied, ns->ns_DnsslAppliedCount);
        ami_ns_resolver_permit();

        if (removed != 0)
            AMI_INFO("netstack: DHCPv6 search list withdrawn, %ld domain(s)",
                     (long)removed);
        if (added != 0)
            AMI_INFO("netstack: DHCPv6 search list, %ld domain(s)",
                     (long)added);
    }
}

/*
 * Take what the callbacks recorded and make the resolver agree with it.
 * Called from a caller thread on the way into a lookup, which is where it is
 * safe: the DNS client holds its mutex across a query the IP thread must run.
 */
static VOID ami_ns_dns_absorb_rdnss(AmiNetStack *ns, AmiNsDns6Scratch *sc)
{
    AmiResolverConfig *r;
    AmiNsRaSnapshot   *ra = &sc->ra;
    UWORD              i;
    UWORD              j;

    if (ns == NULL || !ns->ns_DnsCreated)
        return;

    r = &ns->ns_Config.resolver;

    if (!ami_ns_ra_snapshot(&ns->ns_Ra, ra, tx_time_get()))
        return;

    if (ra->rdnss_pending)
    {
        char *text = sc->text;

        /* Out: in the configuration, not in the advertisement.  Backwards, so
           the compaction inside the withdraw does not skip an entry. */
        for (i = r->nameserver6_count; i-- != 0; )
        {
            ULONG gone[AMI_CFG_IP6_WORDS];
            BOOL  dropped = FALSE;

            for (j = 0; j < ra->rdnss_count; j++)
                if (ami_ns6_same(r->nameserver6[i],
                                 ra->rdnss[j].nxd_ip_address.v6))
                    break;

            if (j != ra->rdnss_count)
                continue;

            if (ami_ns_dns_dhcpv6_names(ns, r->nameserver6[i]))
                continue;

            gone[0] = r->nameserver6[i][0];
            gone[1] = r->nameserver6[i][1];
            gone[2] = r->nameserver6[i][2];
            gone[3] = r->nameserver6[i][3];

            (VOID)ami_ns_dns6_reference_remove(ns, gone, &dropped, NULL);

            if (!dropped)
                continue;

            ami_config_format_ip6(gone, text, sizeof(sc->text));
            AMI_INFO("netstack: advertised name server %s withdrawn", text);
        }

        /*
         * In: the DNS client first, and the configuration only if that worked --
         * ShowNetStatus, ObtainDomainNameServerList() and CheckNetConfig all report
         * from the configuration, so the two must not disagree.
         */
        for (i = 0; i < ra->rdnss_count; i++)
        {
            UINT status = nxd_dns_server_add(&ns->ns_Dns, &ra->rdnss[i]);

            ami_config_format_ip6(ra->rdnss[i].nxd_ip_address.v6, text,
                                  sizeof(sc->text));

            if (status != NX_SUCCESS && status != NX_DNS_DUPLICATE_ENTRY)
            {
                AMI_WARN("netstack: advertised name server %s rejected (%ld)",
                         text, (long)status);
                continue;
            }

            {
                BOOL offered;

                ami_ns_resolver_forbid();
                offered = ami_config_nameserver6_offer(
                    r, ra->rdnss[i].nxd_ip_address.v6);
                ami_ns_resolver_permit();

                if (offered)
                    AMI_INFO("netstack: advertised name server %s", text);
            }
        }
    }

    if (ra->dnssl_pending)
    {
        UWORD added = 0;
        UWORD removed = 0;

        for (i = ns->ns_DnsslAppliedCount; i-- != 0; )
        {
            for (j = 0; j < ra->dnssl_count; j++)
                if (ami_ns_domain_same(ns->ns_DnsslApplied[i],
                                       ra->dnssl[j]))
                    break;

            if (j != ra->dnssl_count)
                continue;

            ami_ns_resolver_forbid();
            if (ami_config_search_reference_remove(
                    r, ns->ns_DnsslApplied[i]))
                removed++;
            ami_ns_resolver_permit();

            for (j = (UWORD)(i + 1U); j < ns->ns_DnsslAppliedCount; j++)
                ami_ns_copy_name(ns->ns_DnsslApplied[j - 1U],
                                 ns->ns_DnsslApplied[j], AMI_CFG_NAME_LEN);
            ns->ns_DnsslAppliedCount--;
        }

        for (i = 0; i < ra->dnssl_count; i++)
        {
            BOOL accepted;

            for (j = 0; j < ns->ns_DnsslAppliedCount; j++)
                if (ami_ns_domain_same(ns->ns_DnsslApplied[j],
                                       ra->dnssl[i]))
                    break;

            if (j != ns->ns_DnsslAppliedCount)
                continue;

            ami_ns_resolver_forbid();
            accepted = ami_config_search_reference_add(r, ra->dnssl[i]);
            ami_ns_resolver_permit();

            if (!accepted)
                continue;

            ami_ns_copy_name(ns->ns_DnsslApplied[ns->ns_DnsslAppliedCount],
                             ra->dnssl[i], AMI_CFG_NAME_LEN);
            ns->ns_DnsslAppliedCount++;
            added++;
        }

        ami_ns_resolver_forbid();
        ami_ns_dns_ra_default_reconcile(r, ns->ns_DnsslDefault,
                                        ns->ns_DnsslApplied,
                                        ns->ns_DnsslAppliedCount);
        ami_ns_resolver_permit();

        if (removed != 0)
            AMI_INFO("netstack: advertised search list withdrawn, %ld "
                     "domain(s)", (long)removed);
        if (added != 0)
            AMI_INFO("netstack: advertised search list, %ld domain(s), "
                     "first '%s'", (long)added, ra->dnssl[0]);
    }
}

/* Called with the ThreadX caller bracket held. Each stored mutation below is
   a short Forbid section; the NetX calls between them must stay outside it. */
static VOID ami_ns_dns_absorb_ipv6_pending(AmiNetStack *ns)
{
    AmiNsDns6Scratch *sc;

    if (!ns->ns_Dhcpv6DnsPending &&
        !ami_ns_ra_needs_snapshot(&ns->ns_Ra, tx_time_get()))
        return;

    sc = (AmiNsDns6Scratch *)ami_ns_dns_scratch_alloc((ULONG)sizeof(*sc));
    if (sc == NULL)
    {
        AMI_WARN("netstack: no memory to absorb the IPv6 resolver handoff");
        return;
    }

    ami_ns_dns_absorb_dhcpv6(ns, sc);
    ami_ns_dns_absorb_rdnss(ns, sc);

    ami_ns_dns_scratch_free(sc, (ULONG)sizeof(*sc));
}

#endif /* AMINETXDUO_IPV6 */

/* Called with the ThreadX caller bracket held.  The DHCP callback only marks
   an interface; option retrieval, DNS-client calls and resolver mutation all
   happen here on the caller task. */
static VOID ami_ns_dns_absorb_pending(AmiNetStack *ns)
{
    ULONG pending = ami_ns_dns_pending_take(&ns->ns_DhcpDnsPending);
    UWORD iface;

    for (iface = 0; iface < ns->ns_IfaceCount; iface++)
    {
        if ((pending & (1UL << iface)) == 0UL)
            continue;

        /*
         * The reconcile keeps the last coherent option set rather than acting on a
         * partial read, and says whether that wants another look.  Re-mark, or a
         * lease sitting at BOUND carries no resolver state until T1.
         */
        if (!ami_netstack_dns_dhcp_reconcile(ns, iface))
            ami_ns_dns_pending_mark(&ns->ns_DhcpDnsPending, iface);
    }

#ifdef AMINETXDUO_IPV6
    /* DHCPv6 first, so its servers are ahead of the advertisement's in the
       list the resolver tries. */
    ami_ns_dns_absorb_ipv6_pending(ns);
#endif
}

/*
 * The report-only half of the handoff.  A report has no caller bracket, so it
 * takes one here.
 */
VOID netstack_dns_absorb_pending(VOID)
{
    AmiNetStack  *ns = ami_netstack_raw();
    AmiNetCaller *caller;

    if (ns == NULL || !ns->ns_DnsCreated)
        return;

    if (!ami_ns_dns_pending_any(&ns->ns_DhcpDnsPending)
#ifdef AMINETXDUO_IPV6
        && !ami_ns_ra_needs_snapshot(&ns->ns_Ra, tx_time_get()) &&
        !ns->ns_Dhcpv6DnsPending
#endif
       )
        return;

    caller = ami_netstack_enter_alloc();
    if (caller == NULL)
        return;

    ami_ns_dns_absorb_pending(ns);

    ami_netstack_leave_free(caller);
}

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

/* Add one source's ownership of a server.  The first owner creates the DNS
   entry; later owners only deepen the signed count in reported configuration. */
static BOOL ami_ns_dns_reference_add(AmiNetStack *ns, ULONG server)
{
    AmiResolverConfig *r = &ns->ns_Config.resolver;
    UINT               status;
    UWORD              i;

    for (i = 0; i < r->nameserver_count; i++)
        if (r->nameserver[i] == server)
        {
            ami_ns_resolver_forbid();
            r->nameserver_use[i] =
                ami_ns_dns_use_deepen(r->nameserver_use[i]);
            ami_ns_resolver_permit();
            return TRUE;
        }

    if (r->nameserver_count >= AMI_CFG_MAX_NAMESERVERS)
        return FALSE;

    status = nx_dns_server_add(&ns->ns_Dns, server);
    if (status != NX_SUCCESS && status != NX_DNS_DUPLICATE_ENTRY)
        return FALSE;

    ami_ns_resolver_forbid();
    r->nameserver[r->nameserver_count] = server;
    r->nameserver_use[r->nameserver_count] = 1;
    r->nameserver_count++;
    ami_ns_resolver_permit();
    return TRUE;
}


/* Drop one source's ownership.  Static configuration, Roadshow callers and a
   lease on the other interface keep the entry alive until their counts go. */
static BOOL ami_ns_dns_reference_remove(AmiNetStack *ns, ULONG server)
{
    AmiResolverConfig *r = &ns->ns_Config.resolver;
    UINT               status;
    UWORD              at;
    UWORD              i;
    LONG               use;

    for (at = 0; at < r->nameserver_count; at++)
        if (r->nameserver[at] == server)
            break;

    if (at == r->nameserver_count)
        return TRUE;

    use = ami_ns_dns_use_shallow(r->nameserver_use[at]);
    if (use != 0)
    {
        ami_ns_resolver_forbid();
        r->nameserver_use[at] = use;
        ami_ns_resolver_permit();
        return TRUE;
    }

    status = nx_dns_server_remove(&ns->ns_Dns, server);
    if (status != NX_SUCCESS && status != NX_DNS_SERVER_NOT_FOUND)
        return FALSE;

    ami_ns_resolver_forbid();
    for (i = at; i + 1U < r->nameserver_count; i++)
    {
        r->nameserver[i] = r->nameserver[i + 1U];
        r->nameserver_use[i] = r->nameserver_use[i + 1U];
    }
    r->nameserver_count--;
    r->nameserver[r->nameserver_count] = 0UL;
    r->nameserver_use[r->nameserver_count] = 0;
    ami_ns_resolver_permit();
    return TRUE;
}


static BOOL ami_ns_dns_array_has(const ULONG *server, UWORD count, ULONG value)
{
    UWORD i;

    for (i = 0; i < count; i++)
        if (server[i] == value)
            return TRUE;

    return FALSE;
}


static BOOL ami_ns_search_array_has(
    const char domain[AMI_CFG_MAX_SEARCH][AMI_CFG_NAME_LEN], UWORD count,
    const char *value)
{
    UWORD i;

    for (i = 0; i < count; i++)
        if (ami_ns_domain_same(domain[i], value))
            return TRUE;
    return FALSE;
}


/*
 * TRUE when this interface's search list is now what the lease says.  FALSE
 * keeps the last coherent list and asks the caller for another pass.
 */
static BOOL ami_ns_dhcp_search_reconcile(AmiNetStack *ns, UWORD iface,
                                         AmiNsDnsScratch *sc)
{
    AmiResolverConfig *offered = &sc->offered;
    UCHAR             *raw = sc->search_raw;
    char              *text = sc->search_text;
    UINT               size = (UINT)sizeof(sc->search_raw);
    AmiNsDnsOptionRead read;
    UWORD              i;

    if (ns == NULL || !ns->ns_DhcpStarted || iface >= ns->ns_IfaceCount)
        return TRUE;            /* nothing to do, not "try again" */

    memset(offered, 0, sizeof(*offered));
    text[0] = '\0';

    if (ns->ns_DhcpState[iface] >= (UBYTE)NX_DHCP_STATE_BOUND)
    {
        /* raw[] is 256 and an option's length is one octet, so option 119
           cannot answer AMI_NS_DNS_OPTION_REFUSED here. */
        read = ami_ns_dhcp_option_read(nx_dhcp_interface_user_option_retrieve(
            &ns->ns_Dhcp, (UINT)iface, AMI_DHCP_OPTION_SEARCH, raw, &size));
        if (!ami_ns_dns_option_usable(read))
            return (BOOL)(!ami_ns_dns_option_retry(read));
        if (read == AMI_NS_DNS_OPTION_READ)
            (VOID)ami_config_search_from_rfc3397(offered, raw, (ULONG)size);

        if (!ami_ns_dhcp_domain_option(ns, iface, sc, text))
            return FALSE;
        if (text[0] != '\0' &&
            text[AMI_CFG_NAME_LEN - 1U] == '\0')
            (VOID)ami_config_search_offer(offered, text);
    }

    for (i = ami_ns_dhcp_search_lease_count(&ns->ns_DhcpSearchLease, iface);
         i-- != 0U; )
    {
        const char *domain = ami_ns_dhcp_search_lease_at(
            &ns->ns_DhcpSearchLease, iface, i);

        if (ami_ns_search_array_has(offered->search, offered->search_count,
                                    domain))
            continue;

        ami_ns_resolver_forbid();
        (VOID)ami_config_search_reference_remove(&ns->ns_Config.resolver,
                                                 domain);
        ami_ns_resolver_permit();
        (VOID)ami_ns_dhcp_search_lease_remove(&ns->ns_DhcpSearchLease,
                                              iface, domain);
        AMI_INFO("netstack: interface %ld DHCP search suffix withdrawn",
                 (long)iface);
    }

    for (i = 0; i < offered->search_count; i++)
    {
        BOOL accepted;

        if (ami_ns_dhcp_search_lease_has(&ns->ns_DhcpSearchLease, iface,
                                         offered->search[i]))
            continue;

        ami_ns_resolver_forbid();
        accepted = ami_config_search_reference_add(&ns->ns_Config.resolver,
                                                   offered->search[i]);
        ami_ns_resolver_permit();
        if (!accepted)
            continue;

        (VOID)ami_ns_dhcp_search_lease_add(&ns->ns_DhcpSearchLease, iface,
                                           offered->search[i]);
        AMI_INFO("netstack: interface %ld DHCP search suffix '%s'",
                 (long)iface, offered->search[i]);
    }

    return TRUE;
}


/* Same contract as the search reconcile above. */
static BOOL ami_ns_dhcp_domain_reconcile(AmiNetStack *ns, UWORD iface,
                                         AmiNsDnsScratch *sc)
{
    char *text = sc->domain_text;

    if (ns == NULL || !ns->ns_DhcpStarted || iface >= ns->ns_IfaceCount)
        return TRUE;            /* nothing to do, not "try again" */

    text[0] = '\0';
    if (ns->ns_DhcpState[iface] >= (UBYTE)NX_DHCP_STATE_BOUND)
    {
        if (!ami_ns_dhcp_domain_option(ns, iface, sc, text))
            return FALSE;
    }

    ami_ns_dns_dhcp_default_update(&ns->ns_DhcpDomain, iface, text);

    ami_ns_resolver_forbid();
#ifdef AMINETXDUO_IPV6
    ami_ns_dns_dhcp_default_reconcile(&ns->ns_Config.resolver,
                                      &ns->ns_DhcpDomain,
                                      ns->ns_DnsslDefault,
                                      ns->ns_DnsslApplied,
                                      ns->ns_DnsslAppliedCount);
#else
    ami_ns_dns_dhcp_default_reconcile(&ns->ns_Config.resolver,
                                      &ns->ns_DhcpDomain,
                                      NULL, NULL, 0U);
#endif
    ami_ns_resolver_permit();

    return TRUE;
}


/* Same contract as the two reconciles above. */
static BOOL ami_ns_dhcp_hostname_reconcile_iface(AmiNetStack *ns, UWORD iface,
                                                 AmiNsDnsScratch *sc)
{
    char *text = sc->host_text;

    text[0] = '\0';
    if (ns->ns_DhcpState[iface] >= (UBYTE)NX_DHCP_STATE_BOUND)
    {
        if (!ami_ns_dhcp_hostname_option(ns, iface, sc, text))
            return FALSE;
    }

    ami_ns_dhcp_hostname_update(&ns->ns_DhcpHostname, iface, text);
    if (ami_ns_dhcp_hostname_reconcile(&ns->ns_Config,
                                       &ns->ns_DhcpHostname))
        AMI_INFO("netstack: host name is now '%s' after DHCP interface %ld",
                 ns->ns_Config.hostname, (long)iface);

    return TRUE;
}


/*
 * Reconcile resolver options from one DHCP interface.  Called once for every
 * interface already bound when the DNS client starts, and from a caller task
 * after the DHCP state callback marks an interface.
 */
static BOOL ami_ns_dhcp_reconcile_with(AmiNetStack *ns, UWORD iface,
                                       AmiNsDnsScratch *sc)
{
    ULONG             *offered = sc->offered_dns;
    ULONG             *raw = sc->raw_dns;
    UINT               size = (UINT)sizeof(sc->raw_dns);
    AmiNsDnsOptionRead read;
    BOOL               done = TRUE;
    UWORD              offered_count = 0;
    UWORD              i;

    /*
     * None of the four option families is skipped because an earlier one wants
     * another pass: they are independent and each is idempotent, so the pass
     * that FALSE asks for repeats them harmlessly.
     */
    if (!ami_ns_dhcp_hostname_reconcile_iface(ns, iface, sc))
        done = FALSE;
    if (!ami_ns_dhcp_domain_reconcile(ns, iface, sc))
        done = FALSE;
    if (!ami_ns_dhcp_search_reconcile(ns, iface, sc))
        done = FALSE;

    if (ns->ns_DhcpState[iface] >= (UBYTE)NX_DHCP_STATE_BOUND)
    {
        read = ami_ns_dhcp_option_read(nx_dhcp_interface_user_option_retrieve(
            &ns->ns_Dhcp, (UINT)iface, NX_DHCP_OPTION_DNS_SVR,
            (UCHAR *)raw, &size));

        /*
         * raw[] holds NX_DNS_MAX_SERVERS addresses, so a server offering more is
         * refused on every retrieve for the life of the lease: keep the servers
         * already installed and say so once rather than asking for another pass.
         */
        if (read == AMI_NS_DNS_OPTION_REFUSED)
            AMI_WARN("netstack: interface %ld offers more than %ld DNS "
                     "servers, keeping the ones already known",
                     (long)iface, (long)NX_DNS_MAX_SERVERS);

        if (!ami_ns_dns_option_usable(read))
            return (BOOL)(done && !ami_ns_dns_option_retry(read));

        if (read == AMI_NS_DNS_OPTION_READ)
        {
            UWORD raw_count = (UWORD)(size / (UINT)sizeof(ULONG));

            for (i = 0; i < raw_count &&
                        offered_count < AMI_CFG_MAX_NAMESERVERS; i++)
            {
                if (raw[i] != 0UL &&
                    !ami_ns_dns_array_has(offered, offered_count, raw[i]))
                    offered[offered_count++] = raw[i];
            }
        }
    }

    /* Withdraw backwards because each success compacts this interface's set. */
    for (i = ami_ns_dhcp_dns_lease_count(&ns->ns_DhcpDnsLease, iface);
         i-- != 0U; )
    {
        ULONG server = ami_ns_dhcp_dns_lease_at(&ns->ns_DhcpDnsLease,
                                                iface, i);

        if (ami_ns_dns_array_has(offered, offered_count, server))
            continue;

        if (ami_ns_dns_reference_remove(ns, server))
        {
            (VOID)ami_ns_dhcp_dns_lease_remove(&ns->ns_DhcpDnsLease,
                                               iface, server);
            AMI_INFO("netstack: interface %ld DHCP name server withdrawn",
                     (long)iface);
        }
    }

    for (i = 0; i < offered_count; i++)
    {
        ULONG server = offered[i];

        if (ami_ns_dhcp_dns_lease_has(&ns->ns_DhcpDnsLease, iface, server))
            continue;

        if (!ami_ns_dns_reference_add(ns, server))
        {
            AMI_WARN("netstack: interface %ld DHCP name server rejected",
                     (long)iface);
            continue;
        }

        (VOID)ami_ns_dhcp_dns_lease_add(&ns->ns_DhcpDnsLease, iface, server);

        AMI_INFO("netstack: interface %ld DHCP name server "
                 "%lu.%lu.%lu.%lu", (long)iface,
                 (unsigned long)((server >> 24) & 0xFFUL),
                 (unsigned long)((server >> 16) & 0xFFUL),
                 (unsigned long)((server >>  8) & 0xFFUL),
                 (unsigned long)(server & 0xFFUL));
    }

    return done;
}

BOOL ami_netstack_dns_dhcp_reconcile(AmiNetStack *ns, UWORD iface)
{
    AmiNsDnsScratch *sc;
    BOOL             done;

    if (ns == NULL || !ns->ns_DnsCreated || !ns->ns_DhcpStarted ||
        iface >= ns->ns_IfaceCount)
        return TRUE;    /* nothing to do, not "try again" */

    /*
     * The option buffers, off the caller's stack: see AmiNsDnsScratch.  A refusal
     * here keeps what is installed and asks for another pass.
     */
    sc = (AmiNsDnsScratch *)ami_ns_dns_scratch_alloc((ULONG)sizeof(*sc));
    if (sc == NULL)
    {
        AMI_WARN("netstack: no memory to read interface %ld DHCP options",
                 (long)iface);
        return FALSE;
    }

    done = ami_ns_dhcp_reconcile_with(ns, iface, sc);
    ami_ns_dns_scratch_free(sc, (ULONG)sizeof(*sc));

    return done;
}

/* The DHCP client invokes its state callback on its own ThreadX task and, on
   several paths, while holding its client mutex.  Do not enter the DNS client
   or mutate resolver configuration there. */
VOID ami_netstack_dns_dhcp_changed(AmiNetStack *ns, UWORD iface)
{
    if (ns == NULL || iface >= ns->ns_IfaceCount)
        return;

    ami_ns_dns_pending_mark(&ns->ns_DhcpDnsPending, iface);
}

LONG ami_netstack_dns_start(AmiNetStack *ns)
{
    UINT  status;
    UWORD i;

    if (ns == NULL || !ns->ns_IpCreated)
        return AMI_NET_ERR_STATE;

    if (ns->ns_DnsCreated)
        return AMI_NET_OK;

    /*
     * Import the current option 12 before nx_dns_create(), so the responder and
     * gethostname() still see the lease's name when creation fails.  Not marked
     * here -- the reconcile loop below marks the interfaces wanting another pass.
     */
    if (ns->ns_DhcpStarted)
    {
        AmiNsDnsScratch *sc =
            (AmiNsDnsScratch *)ami_ns_dns_scratch_alloc((ULONG)sizeof(*sc));

        if (sc != NULL)
        {
            for (i = 0U; i < ns->ns_IfaceCount; i++)
                (VOID)ami_ns_dhcp_hostname_reconcile_iface(ns, i, sc);
            ami_ns_dns_scratch_free(sc, (ULONG)sizeof(*sc));
        }
    }

    status = nx_dns_create(&ns->ns_Dns, &ns->ns_Ip,
                           (UCHAR *)ns->ns_Config.resolver.domain);
    if (status != NX_SUCCESS)
    {
        AMI_WARN("netstack: nx_dns_create failed (%ld)", (long)status);
        return AMI_NET_ERR_KERNEL;
    }

    ns->ns_DnsCreated = TRUE;

    /* The client has no pool of its own: NX_DNS_CLIENT_USER_CREATE_PACKET_POOL
       trades nx_dns_pool_area for ours.  Before any query. */
    status = nx_dns_packet_pool_set(&ns->ns_Dns, &ns->ns_Pool);
    if (status != NX_SUCCESS)
        AMI_ERROR("netstack: DNS could not take the shared packet pool (%ld)",
                  (long)status);

#ifdef NX_DNS_CACHE_ENABLE
    /*
     * NX_DNS_CACHE_ENABLE only compiles the code in: nx_dns_create() leaves
     * nx_dns_cache NULL, so without this call the feature is inert.  Failure is
     * not fatal; lookups still go to the wire.
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
     * nx_dhcp hands the servers to the IP instance rather than to this module,
     * so they are picked out of the lease.
     */
    if (ns->ns_DhcpStarted)
    {
        UWORD iface;

        /*
         * The non-interface retrieve API stops at the first bound DHCP record, so
         * every interface holding a lease is collected here.
         */
        for (iface = 0; iface < ns->ns_IfaceCount; iface++)
            if (!ami_netstack_dns_dhcp_reconcile(ns, iface))
                ami_ns_dns_pending_mark(&ns->ns_DhcpDnsPending, iface);
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
 * Each of these is one attempt for the ladder in netstack_retry.c.  They take
 * the ThreadX bracket themselves rather than sharing one, so the caller's
 * give_up() -- an Exec SetSignal() -- is asked outside it.
 */

typedef struct
{
    AmiNetStack *ns;
    const char  *name;
    ULONG        address;
    UINT         status;
    BOOL         nocaller;      /* the ThreadX bracket was not taken */
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
 * Test whether `name` is in the .local domain.  Case-insensitive, as DNS is
 * (RFC 4343), and a trailing dot is accepted.  The bare name "local" is not
 * in the .local domain: it is a single label with no domain.
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

    ami_ns_dns_absorb_pending(ask->ns);

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

    ami_ns_dns_absorb_pending(ask->ns);

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

    return AMI_NET_ASK_SILENT;
}
#endif

static LONG ami_ns_resolve_once(const char *name, ULONG *addr_out,
                                ULONG timeout_ticks, AmiNetGiveUpFn give_up,
                                VOID *give_up_arg)
{
    AmiNetStack         *ns = ami_netstack_raw();
    const AmiNetdbEntry *entry;
    AmiNsNameAsk         ask;
    AmiNetLadderResult   done;

    /* DEVS:Internet/hosts first: it must work with the network down. */
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
     * RFC 6762 6.7: a name ending in ".local" must be sent to 224.0.0.251 and
     * never to a unicast DNS server, so the branch is exclusive -- no mDNS answer
     * means the name does not exist.  DEVS:Internet/hosts above still wins.
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
     * No responder in this build, so nothing can answer a .local name -- but RFC
     * 6762 3 still forbids asking a unicast server.
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

    if (dst[n - 1] == '.')
        return FALSE;

    dst[n] = '\0';

    return TRUE;
}

/*
 * Copy one current search suffix while holding the configuration lock.  No
 * pointer into the live list escapes the lock.  `count_out` is the number of
 * suffixes in this snapshot, even when `at` is out of range.
 */
static BOOL ami_ns_join_search_at(AmiNetStack *ns, const char *name, UWORD at,
                                  char *qualified, ULONG qualified_size,
                                  UWORD *count_out)
{
    const char *suffix[AMI_CFG_SEARCH_LIST_MAX];
    UWORD       count;
    BOOL        joined = FALSE;

    ami_ns_resolver_forbid();
    count = ami_config_search_list(&ns->ns_Config.resolver, suffix,
                                   (UWORD)AMI_CFG_SEARCH_LIST_MAX);
    if (at < count)
        joined = ami_ns_join_domain(qualified, qualified_size, name,
                                    suffix[at]);
    ami_ns_resolver_permit();

    if (count_out != NULL)
        *count_out = count;

    return joined;
}

LONG netstack_resolve_until(const char *name, ULONG *addr_out,
                            ULONG timeout_ticks, AmiNetGiveUpFn give_up,
                            VOID *give_up_arg)
{
    AmiNetStack *ns = ami_netstack_raw();
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
     * Qualify with the default domain only after a definite no: TIMEOUT and
     * NOSERVER say nothing about the name, and ABORTED is the caller leaving and
     * must not start another lookup.
     */
    if (err != AMI_NET_ERR_NONAME && err != AMI_NET_ERR_STATE)
        return err;

    if (ns == NULL || !ami_ns_unqualified(name))
        return err;

    count = 0;
    (VOID)ami_ns_join_search_at(ns, name, 0, qualified,
                                (ULONG)sizeof(qualified), &count);

    for (i = 0; i < count; i++)
    {
        LONG next;

        if (!ami_ns_join_search_at(ns, name, i, qualified,
                                   (ULONG)sizeof(qualified), NULL))
            continue;

        next = ami_ns_resolve_once(qualified, addr_out, timeout_ticks, give_up,
                                   give_up_arg);
        if (next == AMI_NET_OK)
            return AMI_NET_OK;

        if (next != AMI_NET_ERR_NONAME && next != AMI_NET_ERR_STATE)
            break;
    }

    /* The caller asked about the bare name, so report the first failure. */
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
     * link-local multicast, so a reverse lookup of a 169.254/16 address must not
     * go to a unicast server.
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
 * nxd_dns.h requires the record buffer to be 4-byte aligned and
 * _nxde_dns_ipv6_address_by_name_get() enforces it with NX_PTR_ERROR.  On
 * m68k __alignof__(ULONG) is 2, so the attribute is the mechanism, not a hint.
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

    ami_ns_dns_absorb_pending(ask->ns);

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
     * hosts address with ami_config_parse_ip(), which understands dotted quads
     * only, so the store cannot hold an IPv6 address.
     */
    if (ns == NULL || !ns->ns_DnsCreated)
        return AMI_NET_ERR_STATE;

    /*
     * The IPv6 half of the vendored mDNS module is not enabled in this build, so
     * a .local name is refused here rather than routed to a unicast server.
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

LONG netstack_resolve6_until(const char *name, ULONG addr_out[4],
                             ULONG timeout_ticks, AmiNetGiveUpFn give_up,
                             VOID *give_up_arg)
{
    AmiNetStack *ns = ami_netstack_raw();
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

    count = 0;
    (VOID)ami_ns_join_search_at(ns, name, 0, qualified,
                                (ULONG)sizeof(qualified), &count);

    for (i = 0; i < count; i++)
    {
        LONG next;

        if (!ami_ns_join_search_at(ns, name, i, qualified,
                                   (ULONG)sizeof(qualified), NULL))
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

/*
 * A server has to land in two places: the NetX Duo DNS client, which resolves,
 * and ns_Config.resolver, which ShowNetStatus, ObtainDomainNameServerList and
 * CheckNetConfig read.
 */

/*
 * AddDomainNameServer() nests: two adds need two RemoveDomainNameServer()
 * calls.  nameserver_use[] carries the count, signed as the report API
 * expects; the NetX Duo list does not count, so only first add and last
 * remove touch it.
 */

LONG netstack_dns_server_add(ULONG address)
{
    AmiNetStack  *ns = netstack_get();
    AmiNetCaller  *caller;
    LONG          result = AMI_NET_OK;
    UINT          status;
    UWORD         i;

    if (address == 0UL)
        return AMI_NET_ERR_CONFIG;
    if (ns == NULL)
        return AMI_NET_ERR_STATE;

    /* The caller bracket serialises writers and the DNS client. Short Forbid
       sections make each stored mutation atomic to report-only Exec tasks. */
    caller = ami_netstack_enter_alloc();
    if (caller == NULL)
        return AMI_NET_ERR_STATE;

    for (i = 0; i < ns->ns_Config.resolver.nameserver_count; i++)
        if (ns->ns_Config.resolver.nameserver[i] == address)
        {
            ami_ns_resolver_forbid();
            ns->ns_Config.resolver.nameserver_use[i] =
                ami_ns_dns_use_deepen(
                    ns->ns_Config.resolver.nameserver_use[i]);
            ami_ns_resolver_permit();
            goto out;
        }

    if (ns->ns_Config.resolver.nameserver_count >= AMI_CFG_MAX_NAMESERVERS)
    {
        result = AMI_NET_ERR_NOMEM;
        goto out;
    }

    status = nx_dns_server_add(&ns->ns_Dns, address);

    if (status != NX_SUCCESS)
    {
        result = AMI_NET_ERR_CONFIG;
        goto out;
    }

    ami_ns_resolver_forbid();
    ns->ns_Config.resolver.nameserver[ns->ns_Config.resolver.nameserver_count] =
        address;
    ns->ns_Config.resolver.nameserver_use[ns->ns_Config.resolver.nameserver_count] =
        1;
    ns->ns_Config.resolver.nameserver_count++;
    ami_ns_resolver_permit();

    AMI_INFO("netstack: name server %lu.%lu.%lu.%lu added",
             (unsigned long)((address >> 24) & 0xFFUL),
             (unsigned long)((address >> 16) & 0xFFUL),
             (unsigned long)((address >>  8) & 0xFFUL),
             (unsigned long)(address & 0xFFUL));

out:
    ami_netstack_leave_free(caller);
    return result;
}

LONG netstack_dns_server_remove(ULONG address)
{
    AmiNetStack  *ns = netstack_get();
    AmiNetCaller  *caller;
    LONG          result = AMI_NET_OK;
    UINT          status;
    UWORD         i;
    UWORD         at;
    LONG          use;

    if (address == 0UL)
        return AMI_NET_ERR_CONFIG;
    if (ns == NULL)
        return AMI_NET_ERR_STATE;

    caller = ami_netstack_enter_alloc();
    if (caller == NULL)
        return AMI_NET_ERR_STATE;

    at = (UWORD)AMI_CFG_MAX_NAMESERVERS;
    for (i = 0; i < ns->ns_Config.resolver.nameserver_count; i++)
        if (ns->ns_Config.resolver.nameserver[i] == address)
        {
            at = i;
            break;
        }
    if (at >= (UWORD)AMI_CFG_MAX_NAMESERVERS)
    {
        result = AMI_NET_ERR_NONAME;
        goto out;
    }

    use = ami_ns_dns_use_shallow(
        ns->ns_Config.resolver.nameserver_use[at]);
    if (use != 0)
    {
        ami_ns_resolver_forbid();
        ns->ns_Config.resolver.nameserver_use[at] = use;
        ami_ns_resolver_permit();
        goto out;
    }

    status = nx_dns_server_remove(&ns->ns_Dns, address);

    if (status != NX_SUCCESS)
    {
        result = AMI_NET_ERR_CONFIG;
        goto out;
    }

    ami_ns_resolver_forbid();
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
    ami_ns_resolver_permit();

out:
    ami_netstack_leave_free(caller);
    return result;
}

#ifdef AMINETXDUO_IPV6

/*
 * The IPv6 half of AddDomainNameServer()/RemoveDomainNameServer().
 *
 * ObtainDomainNameServerList() has always reported the IPv6 servers, and until
 * these existed nothing could take one away again: both entry points parsed a
 * dotted quad and nothing else, so an advertised resolver was visible and
 * permanent.  The nesting is the IPv4 rule, and the owner count is shared with
 * the router advertisement and DHCPv6 paths.
 */
static BOOL ami_ns6_unspecified(const ULONG address[AMI_CFG_IP6_WORDS])
{
    return (BOOL)(address == NULL ||
                  (address[0] == 0UL && address[1] == 0UL &&
                   address[2] == 0UL && address[3] == 0UL));
}

LONG netstack_dns_server6_add(const ULONG address[AMI_CFG_IP6_WORDS])
{
    AmiNetStack  *ns = netstack_get();
    AmiNetCaller *caller;
    LONG          result = AMI_NET_OK;
    UINT          status = NX_SUCCESS;

    if (ami_ns6_unspecified(address))
        return AMI_NET_ERR_CONFIG;
    if (ns == NULL)
        return AMI_NET_ERR_STATE;

    caller = ami_netstack_enter_alloc();
    if (caller == NULL)
        return AMI_NET_ERR_STATE;

    /* A full list and a DNS client that refused the server are different
       answers: the autodoc has ENOBUFS for the first and EINVAL for the
       second. */
    if (!ami_ns_dns6_reference_add(ns, address, &status))
        result = (status == NX_SUCCESS) ? AMI_NET_ERR_NOMEM
                                        : AMI_NET_ERR_CONFIG;

    ami_netstack_leave_free(caller);
    return result;
}

LONG netstack_dns_server6_remove(const ULONG address[AMI_CFG_IP6_WORDS])
{
    AmiNetStack       *ns = netstack_get();
    AmiResolverConfig *r;
    AmiNetCaller      *caller;
    LONG               result = AMI_NET_OK;
    UINT               status = NX_SUCCESS;
    UWORD              at;

    if (ami_ns6_unspecified(address))
        return AMI_NET_ERR_CONFIG;
    if (ns == NULL)
        return AMI_NET_ERR_STATE;

    caller = ami_netstack_enter_alloc();
    if (caller == NULL)
        return AMI_NET_ERR_STATE;

    /* "[ENOENT] The IP address to remove was not found", so the absent case is
       told apart here rather than inside the reference count, which treats it
       as a source giving back what it never got. */
    r = &ns->ns_Config.resolver;
    for (at = 0; at < r->nameserver6_count; at++)
        if (ami_ns6_same(r->nameserver6[at], address))
            break;

    if (at == r->nameserver6_count)
        result = AMI_NET_ERR_NONAME;
    else if (!ami_ns_dns6_reference_remove(ns, address, NULL, &status))
        result = AMI_NET_ERR_CONFIG;

    ami_netstack_leave_free(caller);
    return result;
}

#endif /* AMINETXDUO_IPV6 */

LONG netstack_set_domain_name(const char *name)
{
    AmiNetStack  *ns = netstack_get();
    AmiNetCaller *caller;
    LONG          result = AMI_NET_OK;
    UWORD         i;

    if (ns == NULL)
        return AMI_NET_ERR_STATE;

    caller = ami_netstack_enter_alloc();
    if (caller == NULL)
        return AMI_NET_ERR_STATE;
    ami_ns_resolver_forbid();

    /* A NULL or empty name clears it, which is how Roadshow documents it. */
    if (name == NULL || name[0] == '\0')
    {
        ns->ns_Config.resolver.domain[0] = '\0';
        ns->ns_DhcpDomain.owner[0] = '\0';
#ifdef AMINETXDUO_IPV6
        ns->ns_DnsslDefault[0] = '\0';
#endif
        goto out;
    }

    /*
     * The length is checked before anything is stored: truncating a domain name
     * silently produces wrong lookups.
     */
    for (i = 0; name[i] != '\0'; i++)
    {
        if (i + 1 >= (UWORD)sizeof(ns->ns_Config.resolver.domain))
        {
            result = AMI_NET_ERR_CONFIG;
            goto out;
        }
    }

    for (i = 0; name[i] != '\0'; i++)
        ns->ns_Config.resolver.domain[i] = name[i];
    ns->ns_Config.resolver.domain[i] = '\0';
    ns->ns_DhcpDomain.owner[0] = '\0';
#ifdef AMINETXDUO_IPV6
    ns->ns_DnsslDefault[0] = '\0';
#endif

out:
    ami_ns_resolver_permit();
    ami_netstack_leave_free(caller);
    return result;
}

LONG netstack_resolver_snapshot(AmiResolverConfig *out)
{
    AmiNetStack *ns = netstack_get();

    if (out == NULL)
        return AMI_NET_ERR_CONFIG;
    if (ns == NULL)
        return AMI_NET_ERR_STATE;

    ami_ns_resolver_forbid();
    *out = ns->ns_Config.resolver;
    ami_ns_resolver_permit();

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

    ami_ns_resolver_forbid();

    for (len = 0; ns->ns_Config.resolver.domain[len] != '\0'; len++)
        ;

    if (len == 0)
        result = AMI_NET_ERR_NONAME;
    else if (len >= out_size)
        result = AMI_NET_ERR_CONFIG;
    else
        ami_ns_copy_name(out, ns->ns_Config.resolver.domain, out_size);

    ami_ns_resolver_permit();
    return result;
}
