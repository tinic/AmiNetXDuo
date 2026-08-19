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

#define AMI_RDNSS_MAX               4
#define AMI_DNSSL_MAX               256

typedef struct AmiNsRaPending
{
    NXD_ADDRESS   rdnss[AMI_RDNSS_MAX];
    UWORD         rdnss_count;
    volatile BOOL rdnss_pending;

    UBYTE         dnssl[AMI_DNSSL_MAX];
    UWORD         dnssl_len;
    ULONG         dnssl_lifetime;
    volatile BOOL dnssl_pending;
} AmiNsRaPending;

typedef struct AmiNsRaSnapshot
{
    NXD_ADDRESS rdnss[AMI_RDNSS_MAX];
    UWORD       rdnss_count;
    BOOL        rdnss_pending;

    UBYTE       dnssl[AMI_DNSSL_MAX];
    UWORD       dnssl_len;
    ULONG       dnssl_lifetime;
    BOOL        dnssl_pending;
} AmiNsRaSnapshot;

VOID ami_ns_ra_rdnss(AmiNsRaPending *pending, const ULONG address[4],
                     ULONG lifetime);
VOID ami_ns_ra_dnssl(AmiNsRaPending *pending, const UCHAR *domains,
                     UINT length, ULONG lifetime);
BOOL ami_ns_ra_snapshot(AmiNsRaPending *pending, AmiNsRaSnapshot *snapshot);

#endif /* AMINETXDUO_NETSTACK_RA_H */
