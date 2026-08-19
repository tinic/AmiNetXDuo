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
    ULONG server[AMI_CFG_MAX_INTERFACES][AMI_CFG_MAX_NAMESERVERS];
    UWORD count[AMI_CFG_MAX_INTERFACES];
} AmiNsDhcpDnsLease;

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

/* Signed as Roadshow reports it: negative for a static entry, positive for a
   dynamic one.  A legacy zero in an occupied slot means one static owner. */
LONG ami_ns_dns_use_deepen(LONG stored);
LONG ami_ns_dns_use_shallow(LONG stored);

#endif /* AMINETXDUO_NETSTACK_DNS_LEASE_H */
