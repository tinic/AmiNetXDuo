/*
 * AmiNetXDuo, per-interface DHCP DNS ownership.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETSTACK_DNS_LEASE_H
#define AMINETXDUO_NETSTACK_DNS_LEASE_H

#include <exec/types.h>

#include "aminetxduo/config.h"

typedef struct AmiNsDhcpDnsLease
{
    ULONG server[AMI_CFG_MAX_ATTACHED][AMI_CFG_MAX_NAMESERVERS];
    UWORD count[AMI_CFG_MAX_ATTACHED];
} AmiNsDhcpDnsLease;

typedef struct AmiNsDhcpSearchLease
{
    char  domain[AMI_CFG_MAX_ATTACHED][AMI_CFG_MAX_SEARCH]
                [AMI_CFG_NAME_LEN];
    UWORD count[AMI_CFG_MAX_ATTACHED];
} AmiNsDhcpSearchLease;

BOOL  ami_ns_dhcp_dns_lease_has(const AmiNsDhcpDnsLease *lease,
                                UWORD interface_index, ULONG server);
BOOL  ami_ns_dhcp_dns_lease_add(AmiNsDhcpDnsLease *lease,
                                UWORD interface_index, ULONG server);
BOOL  ami_ns_dhcp_dns_lease_remove(AmiNsDhcpDnsLease *lease,
                                   UWORD interface_index, ULONG server);
UWORD ami_ns_dhcp_dns_lease_count(const AmiNsDhcpDnsLease *lease,
                                  UWORD interface_index);
ULONG ami_ns_dhcp_dns_lease_at(const AmiNsDhcpDnsLease *lease,
                               UWORD interface_index, UWORD at);

BOOL ami_ns_dhcp_search_lease_has(const AmiNsDhcpSearchLease *lease,
                                  UWORD interface_index, const char *domain);
BOOL ami_ns_dhcp_search_lease_add(AmiNsDhcpSearchLease *lease,
                                  UWORD interface_index, const char *domain);
BOOL ami_ns_dhcp_search_lease_remove(AmiNsDhcpSearchLease *lease,
                                     UWORD interface_index,
                                     const char *domain);
UWORD ami_ns_dhcp_search_lease_count(const AmiNsDhcpSearchLease *lease,
                                     UWORD interface_index);
const char *ami_ns_dhcp_search_lease_at(const AmiNsDhcpSearchLease *lease,
                                        UWORD interface_index, UWORD at);

/* Signed as Roadshow reports it: negative for a static entry, positive for a
   dynamic one.  A legacy zero in an occupied slot means one static owner. */
LONG ami_ns_dns_use_deepen(LONG stored);
LONG ami_ns_dns_use_shallow(LONG stored);

#endif /* AMINETXDUO_NETSTACK_DNS_LEASE_H */
