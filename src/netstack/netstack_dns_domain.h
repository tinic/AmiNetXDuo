/*
 * AmiNetXDuo, ownership of the default domain derived from RFC 8106 DNSSL.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETSTACK_DNS_DOMAIN_H
#define AMINETXDUO_NETSTACK_DNS_DOMAIN_H

#include "aminetxduo/config.h"

VOID ami_ns_dns_ra_default_reconcile(
    AmiResolverConfig *resolver,
    char owner[AMI_CFG_NAME_LEN],
    const char applied[AMI_CFG_MAX_SEARCH][AMI_CFG_NAME_LEN],
    UWORD applied_count);

#endif /* AMINETXDUO_NETSTACK_DNS_DOMAIN_H */
