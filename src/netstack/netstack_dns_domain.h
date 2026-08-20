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
    char lease[AMI_CFG_MAX_INTERFACES + 1U][AMI_CFG_DOMAIN_LEN];
    char owner[AMI_CFG_DOMAIN_LEN];
} AmiNsDhcpDomainState;

BOOL ami_ns_domain_valid(const char *name);

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

#endif /* AMINETXDUO_NETSTACK_DNS_DOMAIN_H */
