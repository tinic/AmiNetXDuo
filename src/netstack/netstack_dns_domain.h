/*
 * AmiNetXDuo, ownership of the default domain derived from RFC 8106 DNSSL.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETSTACK_DNS_DOMAIN_H
#define AMINETXDUO_NETSTACK_DNS_DOMAIN_H

#include "aminetxduo/config.h"

typedef struct AmiNsDhcpDomainState
{
    /* IPv4 leases occupy their interface slot. The one DHCPv6 client uses
       the final slot, after every interface, so both families share one
       ordered default-domain owner without colliding on an interface. */
    char lease[AMI_CFG_MAX_ATTACHED + 1U][AMI_CFG_DOMAIN_LEN];
    char owner[AMI_CFG_DOMAIN_LEN];
} AmiNsDhcpDomainState;

BOOL ami_ns_domain_valid(const char *name);
BOOL ami_ns_domain_canonicalize(char *name);
/* Compare non-NULL resolver names, folding ASCII letters only. */
BOOL ami_ns_domain_same(const char *a, const char *b);

VOID ami_ns_dns_ra_default_reconcile(
    AmiResolverConfig *resolver,
    char owner[AMI_CFG_NAME_LEN],
    const char applied[AMI_CFG_MAX_SEARCH][AMI_CFG_NAME_LEN],
    UWORD applied_count);

VOID ami_ns_dns_dhcp_default_update(AmiNsDhcpDomainState *state,
                                    UWORD interface_index,
                                    const char *domain);
VOID ami_ns_dns_dhcpv6_default_update(AmiNsDhcpDomainState *state,
                                      const char *domain);
VOID ami_ns_dns_dhcp_default_reconcile(
    AmiResolverConfig *resolver,
    AmiNsDhcpDomainState *dhcp,
    char ra_owner[AMI_CFG_NAME_LEN],
    const char ra_applied[AMI_CFG_MAX_SEARCH][AMI_CFG_NAME_LEN],
    UWORD ra_applied_count);

/*
 * The order the DNS client asks its servers in, for PREFER (F-093): order[]
 * gets the slots 0..count-1, the preferred kind first, each kind in the order
 * it had.  is_static[i] says whether slot i is a name_resolution server.
 */
VOID ami_ns_dns_prefer_order(const BOOL *is_static, UWORD count,
                             BOOL dynamic_first, UWORD *order);

#endif /* AMINETXDUO_NETSTACK_DNS_DOMAIN_H */
