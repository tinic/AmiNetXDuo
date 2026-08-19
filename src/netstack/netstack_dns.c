/*
 * AmiNetXDuo, name resolution.
 *
 * DEVS:Internet/hosts wins over the network, as in any BSD resolver.  Only
 * then does the query go to addons/dns. The DNS client creates its own
 * packet pool (NX_DNS_CLIENT_USER_CREATE_PACKET_POOL is not defined upstream),
 * so nothing here competes with the stack pool.
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
 * ns_Config.resolver is live configuration: Roadshow entry points can change
 * it while other tasks resolve or report it. The ThreadX caller bracket
 * serialises access to the DNS client, but an Exec task that only reads the
 * configuration never enters that bracket. Short Forbid()/Permit() sections
 * cover copies and mutations of the stored half. They never surround a NetX
 * call: an Exec Wait while holding the ThreadX baton stops the stack.
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

static BOOL ami_ns_domain_valid(const char *name)
{
    ULONG start = 0;
    ULONG i;

    if (name == NULL || name[0] == '\0')
        return FALSE;

    for (i = 0; ; i++)
    {
        ULONG j;

        if (i >= (ULONG)AMI_CFG_DOMAIN_LEN)
            return FALSE;
        if (name[i] != '.' && name[i] != '\0')
            continue;
        if (i == start || i - start > 63UL || name[start] == '-' ||
            name[i - 1UL] == '-')
            return FALSE;

        for (j = start; j < i; j++)
            if (!((name[j] >= 'a' && name[j] <= 'z') ||
                  (name[j] >= 'A' && name[j] <= 'Z') ||
                  (name[j] >= '0' && name[j] <= '9') || name[j] == '-'))
                return FALSE;

        if (name[i] == '\0')
            return TRUE;
        start = i + 1UL;
    }
}

static BOOL ami_ns_dhcp_domain_option(AmiNetStack *ns, UWORD iface,
                                      char out[AMI_CFG_DOMAIN_LEN])
{
    UCHAR raw[AMI_CFG_DOMAIN_LEN];
    UINT  size = (UINT)sizeof(raw);
    UINT  status;
    UINT  i;

    out[0] = '\0';
    status = nx_dhcp_interface_user_option_retrieve(
        &ns->ns_Dhcp, (UINT)iface, AMI_DHCP_OPTION_DOMAIN, raw, &size);
    if (status == NX_DHCP_PARSE_ERROR)
        return TRUE;            /* this lease carries no option 15 */
    if (status != NX_SUCCESS)
        return FALSE;
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
    if (!ami_ns_domain_valid(out))
        out[0] = '\0';
    return TRUE;
}

static BOOL ami_ns_dhcp_hostname_option(AmiNetStack *ns, UWORD iface,
                                        char out[AMI_CFG_NAME_LEN])
{
    UCHAR raw[AMI_CFG_NAME_LEN];
    UINT  size = (UINT)sizeof(raw);
    UINT  status;

    out[0] = '\0';
    status = nx_dhcp_interface_user_option_retrieve(
        &ns->ns_Dhcp, (UINT)iface, NX_DHCP_OPTION_HOST_NAME, raw, &size);
    if (status == NX_DHCP_PARSE_ERROR || status == NX_DHCP_DEST_TO_SMALL)
        return TRUE;            /* absent or too long is no usable offer */
    if (status != NX_SUCCESS)
        return FALSE;

    /* A malformed value is a coherent empty offer: it withdraws any older
       option-12 ownership instead of preserving or truncating it. */
    (VOID)ami_ns_dhcp_hostname_decode(out, raw, (ULONG)size);
    return TRUE;
}

static BOOL ami_ns_search_array_has(
    const char domain[AMI_CFG_MAX_SEARCH][AMI_CFG_NAME_LEN], UWORD count,
    const char *value);

#ifdef AMINETXDUO_IPV6

/*
 * The RFC 8106 half of the resolver.  An IPv6-only link configures addresses
 * from a router advertisement and nothing else, so without this the machine
 * comes up routable and cannot resolve a name: there is no DHCPv6 in this
 * build and DEVS:Internet/name_resolution takes a dotted quad only.
 *
 * The advertisement arrives on the IP thread, which must not call the DNS
 * client: nxd_dns_server_add() waits on the mutex a query holds, and that
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

    (VOID)ip_ptr;
    if (ns == NULL || dns_address == NULL)
        return;

    ami_ns_ra_rdnss(&ns->ns_Ra, (UWORD)interface_index, dns_address,
                    lifetime, tx_time_get());
}

/*
 * The RFC 8106 5.2 half, the suffixes rather than the servers.  Same thread
 * and the same reason for not acting here: the list it feeds is read by every
 * resolver call, so the option is copied and the next lookup decodes it.
 *
 * The bytes rather than the names, because the decoder for this encoding
 * belongs to the configuration (DHCP option 119 carries the same one) and
 * running it here puts a parser reached straight off the network on the IP
 * thread.
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
 * WHO WINS WHEN BOTH A ROUTER ADVERTISEMENT AND DHCPv6 NAME A NAME SERVER.
 *
 * DHCPv6 does, and this is the reason: the router is the one that said so.
 * RFC 4861 4.2's O flag -- and M, which implies it -- is the router
 * delegating the rest of the configuration to a DHCPv6 server, and a router
 * that sets the flag and also advertises RFC 8106 RDNSS has said two things.
 * Honouring the flag it set is the resolution that follows from the router's
 * own statement rather than from a preference of ours.  On a link where the
 * router sets neither flag no DHCPv6 exchange ever happens, so the question
 * does not arise and RDNSS is the only answer, which is what it was before.
 *
 * "Wins" means goes first in the list the resolver tries, not "the other one
 * is discarded".  Both are kept, because a name server that answers is worth
 * having whoever named it, and because the file's own NAMESERVER lines are
 * already ahead of both -- they were added at ami_netstack_dns_start() and
 * nxd_dns_server_add() appends.  So the order is: name_resolution, then
 * DHCPv6, then the advertisement.
 *
 * NEITHER MAY WITHDRAW THE OTHER'S.  The reconciliation below removes a
 * configured server that its own source no longer names; it must not remove
 * one the other source names, or an RDNSS option with a lifetime of zero
 * would silently take away a server DHCPv6 supplied, and a Reply naming a
 * shorter list would take away one the router still advertises.  That is what
 * the ns_Dhcpv6Dns[] cross-check in each loop is for.
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
 * A Reply has landed. Read what it carried out of the client and put it into
 * the resolver, on a caller thread and not on the client's own, for the reason
 * stated above ns_Ra in netstack_internal.h: the DNS client holds its mutex
 * across a query and that query needs the IP thread.
 */
static VOID ami_ns_dns_absorb_dhcpv6(AmiNetStack *ns)
{
    AmiResolverConfig *r;
    NXD_ADDRESS        offered[AMI_RDNSS_MAX];
    UWORD              offered_count = 0;
    char               text[AMI_CFG_IP6_STRLEN];
    UINT               index;
    ULONG              now;
    BOOL               options_valid;

    if (!ns->ns_Dhcpv6DnsPending)
        return;

    ns->ns_Dhcpv6DnsPending = FALSE;
    options_valid = ns->ns_Dhcpv6OptionsValid;

    r = &ns->ns_Config.resolver;
    now = tx_time_get();

    /* Read the replacement set before changing the applied set: the old one
       is the ownership record needed to identify withdrawals. */
    if (options_valid)
        for (index = 0; index < (UINT)NX_DHCPV6_NUM_DNS_SERVERS; index++)
        {
            NXD_ADDRESS server;

            if (nx_dhcpv6_get_DNS_server_address(&ns->ns_Dhcpv6, index,
                                                  &server) != NX_SUCCESS)
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

    /* Out: a later valid Reply may shorten the list or omit the option. A
       server still owned by RDNSS remains in both resolver views. */
    for (index = ns->ns_Dhcpv6DnsCount; index-- != 0U; )
    {
        NXD_ADDRESS server = ns->ns_Dhcpv6Dns[index];
        UINT        status;

        if (ami_ns_dns_v6_list_names(offered, offered_count,
                                     server.nxd_ip_address.v6) ||
            ami_ns_ra_rdnss_has(&ns->ns_Ra, server.nxd_ip_address.v6, now))
            continue;

        status = nxd_dns_server_remove(&ns->ns_Dns, &server);
        ami_config_format_ip6(server.nxd_ip_address.v6, text, sizeof(text));

        if (status != NX_SUCCESS && status != NX_DNS_SERVER_NOT_FOUND)
        {
            AMI_WARN("netstack: DHCPv6 name server %s withdrawal failed "
                     "(%ld)", text, (long)status);
            /* Keep the applied ownership record and try again on the next
               caller pass; otherwise RDNSS could withdraw this still-live
               DNS-client entry as though DHCPv6 no longer owned it. */
            ns->ns_Dhcpv6DnsPending = TRUE;
            return;
        }

        ami_ns_resolver_forbid();
        (VOID)ami_config_nameserver6_withdraw(r,
                                              server.nxd_ip_address.v6);
        ami_ns_resolver_permit();
        AMI_INFO("netstack: DHCPv6 name server %s withdrawn", text);
    }

    memset(ns->ns_Dhcpv6Dns, 0, sizeof(ns->ns_Dhcpv6Dns));
    for (index = 0; index < offered_count; index++)
        ns->ns_Dhcpv6Dns[index] = offered[index];
    ns->ns_Dhcpv6DnsCount = offered_count;

    /* In: the DNS client first, the configuration only if that worked -- the
       invariant the advertisement path below states. */
    for (index = 0; index < offered_count; index++)
    {
        NXD_ADDRESS server = offered[index];
        UINT        status;

        status = nxd_dns_server_add(&ns->ns_Dns, &server);

        ami_config_format_ip6(server.nxd_ip_address.v6, text, sizeof(text));

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
     * OPTION_DOMAIN_LIST. The client has already decoded the RFC 1035 label
     * sequences into a run of NUL-terminated names, so this is not the
     * ami_config_search_from_rfc3397() path the advertisement and DHCP option
     * 119 share -- each name goes to ami_config_search_offer() as it stands,
     * which is where it is checked against RFC 1123 2.1 before anything is
     * pasted onto a query.
     */
    {
        AmiResolverConfig offered = {0};
        UCHAR             names[NX_DHCPV6_DOMAIN_NAME_BUFFER_SIZE];
        ULONG             pos = 0;
        UWORD             added = 0;
        UWORD             removed = 0;
        UWORD             i;

        if (options_valid)
        {
            if (nx_dhcpv6_get_other_option_data(&ns->ns_Dhcpv6,
                                                NX_DHCPV6_DOMAIN_NAME_OPTION,
                                                names, sizeof(names))
                    != NX_SUCCESS)
                return;         /* preserve the last coherent option set */
        }
        else
            memset(names, 0, sizeof(names));

        while (pos < (ULONG)sizeof(names) && names[pos] != '\0')
        {
            (VOID)ami_config_search_offer(&offered,
                                          (const char *)&names[pos]);
            while (pos < (ULONG)sizeof(names) && names[pos] != '\0')
                pos++;
            pos++;
        }

        /* Out: release only DHCPv6's reference. An identical static, DHCPv4
           or RA suffix remains live through its own reference. */
        for (i = ns->ns_Dhcpv6SearchAppliedCount; i-- != 0U; )
        {
            UWORD j;

            if (ami_ns_search_array_has(offered.search,
                                        offered.search_count,
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
        for (i = 0; i < offered.search_count; i++)
        {
            BOOL accepted;

            if (ami_ns_search_array_has(ns->ns_Dhcpv6SearchApplied,
                                        ns->ns_Dhcpv6SearchAppliedCount,
                                        offered.search[i]))
                continue;

            ami_ns_resolver_forbid();
            accepted = ami_config_search_reference_add(r,
                                                       offered.search[i]);
            ami_ns_resolver_permit();
            if (!accepted)
                continue;

            ami_ns_copy_name(
                ns->ns_Dhcpv6SearchApplied
                    [ns->ns_Dhcpv6SearchAppliedCount],
                offered.search[i], AMI_CFG_NAME_LEN);
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
    AmiNsRaSnapshot     ra;
    UWORD              i;
    UWORD              j;

    if (ns == NULL || !ns->ns_DnsCreated)
        return;

    r = &ns->ns_Config.resolver;

    if (!ami_ns_ra_snapshot(&ns->ns_Ra, &ra, tx_time_get()))
        return;

    if (ra.rdnss_pending)
    {
        char text[AMI_CFG_IP6_STRLEN];

        /* Out: in the configuration, not in the advertisement.  Backwards, so
           the compaction inside the withdraw does not skip an entry. */
        for (i = r->nameserver6_count; i-- != 0; )
        {
            ULONG gone[AMI_CFG_IP6_WORDS];
            BOOL  withdrawn;

            for (j = 0; j < ra.rdnss_count; j++)
                if (ami_ns6_same(r->nameserver6[i],
                                 ra.rdnss[j].nxd_ip_address.v6))
                    break;

            if (j != ra.rdnss_count)
                continue;

            /* Named by DHCPv6 as well, so it is not the advertisement's to
               take away.  See the note above ami_ns_dns_absorb_dhcpv6(). */
            if (ami_ns_dns_dhcpv6_names(ns, r->nameserver6[i]))
                continue;

            gone[0] = r->nameserver6[i][0];
            gone[1] = r->nameserver6[i][1];
            gone[2] = r->nameserver6[i][2];
            gone[3] = r->nameserver6[i][3];

            ami_ns_resolver_forbid();
            withdrawn = ami_config_nameserver6_withdraw(r, gone);
            ami_ns_resolver_permit();

            if (!withdrawn)
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
         * report from the configuration, so the two must not disagree.  The
         * DHCP path records its servers for the same reason, and this one
         * recorded none at all, which is the visible half of this defect.
         */
        for (i = 0; i < ra.rdnss_count; i++)
        {
            UINT status = nxd_dns_server_add(&ns->ns_Dns, &ra.rdnss[i]);

            ami_config_format_ip6(ra.rdnss[i].nxd_ip_address.v6, text,
                                  sizeof(text));

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
                    r, ra.rdnss[i].nxd_ip_address.v6);
                ami_ns_resolver_permit();

                if (offered)
                    AMI_INFO("netstack: advertised name server %s", text);
            }
        }
    }

    if (ra.dnssl_pending)
    {
        UWORD added = 0;
        UWORD removed = 0;

        /* Out: domains no interface still advertises.  Each one releases
           only the RA repository's reference; DHCP or static configuration
           keeps an identical suffix alive. */
        for (i = ns->ns_DnsslAppliedCount; i-- != 0; )
        {
            for (j = 0; j < ra.dnssl_count; j++)
                if (ami_ns_domain_same(ns->ns_DnsslApplied[i],
                                       ra.dnssl[j]))
                    break;

            if (j != ra.dnssl_count)
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

        /* In: acquire one RA reference for each domain in the interface
           union.  The handoff already folds duplicates case-insensitively. */
        for (i = 0; i < ra.dnssl_count; i++)
        {
            BOOL accepted;

            for (j = 0; j < ns->ns_DnsslAppliedCount; j++)
                if (ami_ns_domain_same(ns->ns_DnsslApplied[j],
                                       ra.dnssl[i]))
                    break;

            if (j != ns->ns_DnsslAppliedCount)
                continue;

            ami_ns_resolver_forbid();
            accepted = ami_config_search_reference_add(r, ra.dnssl[i]);
            ami_ns_resolver_permit();

            if (!accepted)
                continue;

            ami_ns_copy_name(ns->ns_DnsslApplied[ns->ns_DnsslAppliedCount],
                             ra.dnssl[i], AMI_CFG_NAME_LEN);
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
                     "first '%s'", (long)added, ra.dnssl[0]);
    }
}

/* Called with the ThreadX caller bracket held. Each stored mutation below is
   a short Forbid section; the NetX calls between them must stay outside it. */
static VOID ami_ns_dns_absorb_ipv6_pending(AmiNetStack *ns)
{
    ami_ns_dns_absorb_dhcpv6(ns);
    ami_ns_dns_absorb_rdnss(ns);
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
        if ((pending & (1UL << iface)) != 0UL)
            ami_netstack_dns_dhcp_reconcile(ns, iface);

#ifdef AMINETXDUO_IPV6
    /* DHCPv6 first, so its servers are ahead of the advertisement's in the
       list the resolver tries. */
    ami_ns_dns_absorb_ipv6_pending(ns);
#endif
}

/*
 * The report-only half of the handoff.  Lookups absorb while already inside
 * their caller bracket; a report has no such bracket, so it takes one here.
 */
VOID netstack_dns_absorb_pending(VOID)
{
    AmiNetStack  *ns = ami_netstack_raw();
    AmiNetCaller *caller;

    if (ns == NULL || !ns->ns_DnsCreated)
        return;

    /* Nothing pending is the ordinary case, and it must not cost a report a
       trip into ThreadX. */
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

    /* The stored lease and configuration should agree.  If they do not, heal
       the lease side rather than retaining an owner of a nonexistent entry. */
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


static VOID ami_ns_dhcp_search_reconcile(AmiNetStack *ns, UWORD iface)
{
    AmiResolverConfig offered = {0};
    UCHAR             raw[256];
    char              text[AMI_CFG_DOMAIN_LEN] = {0};
    UINT              size = (UINT)sizeof(raw);
    UINT              status;
    UWORD             i;

    if (ns == NULL || !ns->ns_DhcpStarted || iface >= ns->ns_IfaceCount)
        return;

    if (ns->ns_DhcpState[iface] >= (UBYTE)NX_DHCP_STATE_BOUND)
    {
        status = nx_dhcp_interface_user_option_retrieve(
            &ns->ns_Dhcp, (UINT)iface, AMI_DHCP_OPTION_SEARCH, raw, &size);
        if (status == NX_SUCCESS)
            (VOID)ami_config_search_from_rfc3397(&offered, raw, (ULONG)size);
        else if (status != NX_DHCP_PARSE_ERROR)
            return;             /* preserve the last coherent option set */

        if (!ami_ns_dhcp_domain_option(ns, iface, text))
            return;
        if (text[0] != '\0' &&
            text[AMI_CFG_NAME_LEN - 1U] == '\0')
            (VOID)ami_config_search_offer(&offered, text);
    }

    for (i = ami_ns_dhcp_search_lease_count(&ns->ns_DhcpSearchLease, iface);
         i-- != 0U; )
    {
        const char *domain = ami_ns_dhcp_search_lease_at(
            &ns->ns_DhcpSearchLease, iface, i);

        if (ami_ns_search_array_has(offered.search, offered.search_count,
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

    for (i = 0; i < offered.search_count; i++)
    {
        BOOL accepted;

        if (ami_ns_dhcp_search_lease_has(&ns->ns_DhcpSearchLease, iface,
                                         offered.search[i]))
            continue;

        ami_ns_resolver_forbid();
        accepted = ami_config_search_reference_add(&ns->ns_Config.resolver,
                                                   offered.search[i]);
        ami_ns_resolver_permit();
        if (!accepted)
            continue;

        (VOID)ami_ns_dhcp_search_lease_add(&ns->ns_DhcpSearchLease, iface,
                                           offered.search[i]);
        AMI_INFO("netstack: interface %ld DHCP search suffix '%s'",
                 (long)iface, offered.search[i]);
    }
}


static VOID ami_ns_dhcp_domain_reconcile(AmiNetStack *ns, UWORD iface)
{
    char text[AMI_CFG_DOMAIN_LEN];

    if (ns == NULL || !ns->ns_DhcpStarted || iface >= ns->ns_IfaceCount)
        return;

    text[0] = '\0';
    if (ns->ns_DhcpState[iface] >= (UBYTE)NX_DHCP_STATE_BOUND)
    {
        if (!ami_ns_dhcp_domain_option(ns, iface, text))
            return;
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
}


static VOID ami_ns_dhcp_hostname_reconcile_iface(AmiNetStack *ns, UWORD iface)
{
    char text[AMI_CFG_NAME_LEN];

    text[0] = '\0';
    if (ns->ns_DhcpState[iface] >= (UBYTE)NX_DHCP_STATE_BOUND)
    {
        if (!ami_ns_dhcp_hostname_option(ns, iface, text))
            return;
    }

    ami_ns_dhcp_hostname_update(&ns->ns_DhcpHostname, iface, text);
    if (ami_ns_dhcp_hostname_reconcile(&ns->ns_Config,
                                       &ns->ns_DhcpHostname))
        AMI_INFO("netstack: host name is now '%s' after DHCP interface %ld",
                 ns->ns_Config.hostname, (long)iface);
}


/*
 * Reconcile resolver options from one DHCP interface.
 *
 * Called once for every interface already bound when the DNS client starts,
 * and from a caller task after the DHCP state callback marks an interface.
 * A renewal can replace or omit option 6, 12, 15, or 119, and a stopped/lost
 * lease owns no servers, host name, or suffixes. The per-interface sets make
 * those withdrawals precise.
 */
VOID ami_netstack_dns_dhcp_reconcile(AmiNetStack *ns, UWORD iface)
{
    ULONG offered[AMI_CFG_MAX_NAMESERVERS];
    ULONG raw[NX_DNS_MAX_SERVERS];
    UINT  size = (UINT)sizeof(raw);
    UINT  status;
    UWORD offered_count = 0;
    UWORD i;

    if (ns == NULL || !ns->ns_DnsCreated || !ns->ns_DhcpStarted ||
        iface >= ns->ns_IfaceCount)
        return;

    ami_ns_dhcp_hostname_reconcile_iface(ns, iface);
    ami_ns_dhcp_domain_reconcile(ns, iface);
    ami_ns_dhcp_search_reconcile(ns, iface);

    if (ns->ns_DhcpState[iface] >= (UBYTE)NX_DHCP_STATE_BOUND)
    {
        status = nx_dhcp_interface_user_option_retrieve(
            &ns->ns_Dhcp, (UINT)iface, NX_DHCP_OPTION_DNS_SVR,
            (UCHAR *)raw, &size);

        /* Not carrying option 6 is a valid empty set.  Other failures leave
           the last coherent lease set in place. */
        if (status != NX_SUCCESS && status != NX_DHCP_PARSE_ERROR)
            return;

        if (status == NX_SUCCESS)
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

    /* mDNS starts after this function even when nx_dns_create() fails. Import
       the current option 12 first so the responder and gethostname() still see
       the lease's name; the full resolver reconciliation follows creation. */
    if (ns->ns_DhcpStarted)
        for (i = 0U; i < ns->ns_IfaceCount; i++)
            ami_ns_dhcp_hostname_reconcile_iface(ns, i);

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
     * NX_DNS_CACHE_ENABLE only compiles the code in.  nx_dns_create() leaves
     * nx_dns_cache NULL and every path checks for NULL, so without this call
     * the feature is inert. See AMI_DNS_CACHE_BYTES for the size.
     *
     * Failure is not fatal: lookups still work, and go to the wire.
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
     * instance rather than to this module, so they are picked out of the
     * lease.
     */
    if (ns->ns_DhcpStarted)
    {
        UWORD iface;

        /* The non-interface retrieve API stops at the first bound DHCP
           record. A second card can have a different reachable resolver and
           search list, so collect every interface holding a lease. */
        for (iface = 0; iface < ns->ns_IfaceCount; iface++)
            ami_netstack_dns_dhcp_reconcile(ns, iface);
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
 * rather than sharing one across the ladder, so the give_up() of the caller,
 * an exec SetSignal() for bsdsocket.library, is asked outside it.
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
 * Test whether `name` is in the .local domain.
 *
 * Case-insensitive, as DNS is (RFC 4343). A trailing dot is accepted:
 * "amiga.local." is the fully-qualified spelling of the same name.
 *
 * The bare name "local" is not in the .local domain. It is a single label with
 * no domain, and sending it to mDNS claims a top-level name.
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

    /* A PTR lookup can be the first resolver operation after an RA.  Without
       absorbing here, an IPv6-only link has an advertised server waiting in
       ns_Ra but the DNS client still reports NX_DNS_NO_SERVER. */
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
     * RFC 6762 6.7 requires a name ending in ".local" to be sent to
     * 224.0.0.251 and never to a unicast DNS server. Many home routers answer
     * any name with their own NXDOMAIN-substitute search page, and some
     * forward .local to the internet where another server answers, so the
     * branch is exclusive: no mDNS answer means the name does not exist.
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

    /* A trailing dot on the domain gives "host..", and an empty domain gives
       "host.".  Neither is the name the caller meant. */
    if (dst[n - 1] == '.')
        return FALSE;

    dst[n] = '\0';

    return TRUE;
}

/* Copy one current search suffix into the qualified query while holding the
   configuration lock. No pointer into the live list escapes the lock or is
   kept across the network lookup that follows. `count_out` is the number of
   suffixes in this snapshot, even when `at` is out of range. */
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
     * "If no domain name is part of a host name, a default domain name can be
     * added to it if the host name lookup fails", GetDefaultDomainName().
     * So `ping fileserver` reaches fileserver.lan.
     *
     * Only after a definite no: TIMEOUT and NOSERVER say nothing about the
     * name, and a second query only doubles the wait. ERR_STATE is worth a
     * retry even so, because with the stack down the retry never reaches the
     * network and can only hit DEVS:Internet/hosts, which costs nothing.
     * ABORTED is the caller leaving, and must not start another lookup.
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
     * because nobody is authoritative for the self-assigned address of another
     * machine, so the query only tells that server which link-local addresses
     * this machine is talking to, and then times out.
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
 * so the alignment of this struct is 2 and an instance of it on the stack
 * lands 2 mod 4 whenever the frame does.
 *
 * That is what it did.  getaddrinfo() asks AAAA first and appends it ahead of
 * the A record, so the ordering was never the problem.  The AAAA lookup was
 * refused by argument checking, the caller had no way to tell that apart from
 * "the name has no AAAA", and every name resolved IPv4 on a machine with a
 * working global IPv6 address.  nslookup builds its own query and never came
 * through here, which is why it reported the AAAA the resolver said did not
 * exist.
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
     * DEVS:Internet/hosts is not consulted here: src/config/netdb.c parses the
     * address of a hosts entry with ami_config_parse_ip(), which only
     * understands dotted quads, so the store cannot hold an IPv6 address.
     * Fixing that means a netdb schema change (a second value field, or a
     * family tag per entry) touching get{host,net}by* as well. Until then an
     * IPv6 literal in DEVS:Internet/hosts is ignored and an IPv6-only name
     * must be resolvable by DNS.
     */
    if (ns == NULL || !ns->ns_DnsCreated)
        return AMI_NET_ERR_STATE;

    /*
     * The IPv4 path routes a .local name to mDNS.  This one had no test at all
     * and handed it straight to the configured server.  Refused rather than
     * routed: the IPv6 half of the vendored module is not enabled in this build
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

/* ------------------------------------------- changing the server list --- */

/*
 * Roadshow lets a program add and remove name servers while the stack is
 * running (AddDomainNameServer() and the related calls), and its own
 * AddNetInterface uses those to pass on the servers from a lease it obtained
 * itself. Without
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
 * resolver of the other with it.
 *
 * nameserver_use[] carries the count, signed as ObtainDomainNameServerList()
 * reports it, negative for a server from DEVS:Internet/name_resolution,
 * positive for one DHCP or this call put there. Adding to a static entry keeps
 * it static and deepens it (-1 -> -2).  The entry only leaves the list when
 * the count reaches zero. The list in NetX Duo does not count, so it is
 * touched only on the first add and the last remove.
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

    /* Already known: count the reference and leave the resolver alone. */
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

    /* Still referenced by somebody else: drop one and stop. */
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

    /* Close the gap: the order of the rest is the order they were added in. */
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

    /* Truncating a domain name silently produces wrong lookups, so the length
       is checked before anything is stored.  Writing the truncated form and
       then reporting failure left the resolver on a domain the caller was told
       had been refused. */
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
    /* The caller owns this value even when it deliberately chose the same
       spelling as the current advertisement. */
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
