/*
 * AmiNetXDuo, what a NetX Duo DNS status means.
 *
 * Its own translation unit and its own header, because the two questions below
 * decide user-visible behaviour: whether a lookup is worth repeating, and
 * whether a failure is reported as "no such host" or "try again". No lookup
 * against a real name server demonstrates either on demand. A host test asks
 * them directly (tests/netstack/host/test_dns_status_host.c). netstack_dns.c
 * cannot be compiled off the Amiga.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETSTACK_DNS_STATUS_H
#define AMINETXDUO_NETSTACK_DNS_STATUS_H

/* tx_api.h and nx_api.h before any exec header: <exec/types.h> turns VOID into
   a macro and that breaks the ThreadX typedefs. */
#include "tx_api.h"
#include "nx_api.h"
#include "nxd_dns.h"

#include <exec/types.h>

#include "aminetxduo/netstack.h"

/*
 * Transient, as opposed to an answer: the query is worth sending again.
 *
 * A blocking query in addons/dns folds every per-server failure into
 * NX_DNS_QUERY_FAILED before it returns, so "the name server said NXDOMAIN" and
 * "nothing came back" arrive as the same status and both land here. How long
 * the attempt took separates them instead, see netstack_retry.h.
 */
BOOL ami_ns_dns_again(UINT status);

/*
 * Map a NetX Duo DNS status onto an actionable error: "no resolver configured
 * or reachable" versus "that name does not exist".
 *
 * used/externally_visible keeps the entry point a named symbol under LTO: the
 * post-link relocation check can prove a 68020 bra.l tail call's target only
 * when that target remains a named function.
 */
#if defined(__has_attribute)
# if __has_attribute(externally_visible)
#  define AMI_DNS_STATUS_LTO_ATTR __attribute__((used, externally_visible))
# else
#  define AMI_DNS_STATUS_LTO_ATTR __attribute__((used))
# endif
#elif defined(__GNUC__)
# define AMI_DNS_STATUS_LTO_ATTR __attribute__((used, externally_visible))
#else
# define AMI_DNS_STATUS_LTO_ATTR
#endif

LONG ami_ns_dns_error(UINT status) AMI_DNS_STATUS_LTO_ATTR;

#undef AMI_DNS_STATUS_LTO_ATTR

#endif /* AMINETXDUO_NETSTACK_DNS_STATUS_H */
