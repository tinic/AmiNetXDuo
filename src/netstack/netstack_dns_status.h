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
 * Map a NetX Duo DNS status onto an actionable error. The distinction is
 * between "no resolver configured or reachable" and "that name does not
 * exist". Reporting the second as a device failure makes the reader check
 * cables when the host name was mistyped.
 */
/*
 * Keep the entry point in the linked symbol table under LTO.  Several DNS
 * callers tail-call this function.  A 68020 build spells a sufficiently long
 * tail call as bra.l, and the post-link relocation check can prove its target
 * only when the target remains a named function.  Without externally_visible
 * GCC internalizes this cross-file helper, emits the correct branch, then
 * removes the symbol and makes the safety check reject the valid binary.
 * `used` also keeps the contract intact if LTO clones the small switch.
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
