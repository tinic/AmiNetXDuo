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

/*
 * What one option retrieve said, in the only terms the pending mark cares
 * about.  The DHCPv4 and DHCPv6 status codes are mapped onto this where they
 * are read; the policy below is here so it is one table and so a host test
 * can drive it.
 */
typedef enum AmiNsDnsOptionRead
{
    AMI_NS_DNS_OPTION_READ,     /* the option is here and it fits */
    AMI_NS_DNS_OPTION_ABSENT,   /* this lease does not carry it: an empty set */
    AMI_NS_DNS_OPTION_REFUSED,  /* refused by the buffer and the option, so
                                   refused identically on every later pass */
    AMI_NS_DNS_OPTION_FAILED    /* something else, which may differ next pass */
} AmiNsDnsOptionRead;

BOOL ami_ns_dns_option_usable(AmiNsDnsOptionRead read);
BOOL ami_ns_dns_option_retry(AmiNsDnsOptionRead read);

#endif /* AMINETXDUO_NETSTACK_DNS_HANDOFF_H */
