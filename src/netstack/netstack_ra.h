/*
 * AmiNetXDuo, router-advertisement resolver handoff.
 *
 * The NetX IP thread records RFC 8106 options here.  A caller task takes a
 * coherent snapshot before it calls the DNS client, which the IP thread must
 * never wait on.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETSTACK_RA_H
#define AMINETXDUO_NETSTACK_RA_H

#include "tx_api.h"
#include "nx_api.h"

#include <exec/types.h>

#include "aminetxduo/config.h"

#define AMI_RDNSS_MAX               4
#define AMI_DNSSL_MAX               256

typedef struct AmiNsRdnssEntry
{
    NXD_ADDRESS address;
    ULONG       lifetime;
    ULONG       received;
} AmiNsRdnssEntry;

typedef struct AmiNsDnsslEntry
{
    char  domain[AMI_CFG_NAME_LEN];
    ULONG lifetime;
    ULONG received;
} AmiNsDnsslEntry;

typedef struct AmiNsRaPending
{
    AmiNsRdnssEntry rdnss[AMI_CFG_MAX_ATTACHED][AMI_RDNSS_MAX];
    UWORD         rdnss_count[AMI_CFG_MAX_ATTACHED];
    volatile BOOL rdnss_pending;

    AmiNsDnsslEntry dnssl[AMI_CFG_MAX_ATTACHED][AMI_CFG_MAX_SEARCH];
    UWORD         dnssl_count[AMI_CFG_MAX_ATTACHED];
    volatile BOOL dnssl_pending;
} AmiNsRaPending;

typedef struct AmiNsRaSnapshot
{
    NXD_ADDRESS rdnss[AMI_RDNSS_MAX];
    UWORD       rdnss_count;
    BOOL        rdnss_pending;

    char        dnssl[AMI_CFG_MAX_SEARCH][AMI_CFG_NAME_LEN];
    UWORD       dnssl_count;
    BOOL        dnssl_pending;
} AmiNsRaSnapshot;

VOID ami_ns_ra_rdnss(AmiNsRaPending *pending, UWORD interface_index,
                     const ULONG address[4], ULONG lifetime, ULONG now);
VOID ami_ns_ra_dnssl(AmiNsRaPending *pending, UWORD interface_index,
                     const UCHAR *domains, UINT length, ULONG lifetime,
                     ULONG now);
BOOL ami_ns_ra_snapshot(AmiNsRaPending *pending, AmiNsRaSnapshot *snapshot,
                        ULONG now);
BOOL ami_ns_ra_needs_snapshot(AmiNsRaPending *pending, ULONG now);
BOOL ami_ns_ra_rdnss_has(AmiNsRaPending *pending,
                         const ULONG address[AMI_CFG_IP6_WORDS], ULONG now);

#endif /* AMINETXDUO_NETSTACK_RA_H */
