/*
 * AmiNetXDuo, DHCP resolver producer/consumer handoff.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETSTACK_DNS_HANDOFF_H
#define AMINETXDUO_NETSTACK_DNS_HANDOFF_H

#include "tx_api.h"

#include <exec/types.h>

typedef struct AmiNsDnsPending
{
    volatile ULONG interfaces;
} AmiNsDnsPending;

VOID  ami_ns_dns_pending_mark(AmiNsDnsPending *pending, UWORD interface_index);
ULONG ami_ns_dns_pending_take(AmiNsDnsPending *pending);
BOOL  ami_ns_dns_pending_any(const AmiNsDnsPending *pending);

#endif /* AMINETXDUO_NETSTACK_DNS_HANDOFF_H */
