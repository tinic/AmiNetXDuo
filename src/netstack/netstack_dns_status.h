/*
 * AmiNetXDuo -- what a NetX Duo DNS status means.
 *
 * Its own translation unit, and its own header, because the two questions below
 * decide user-visible behaviour that no lookup against a real name server can
 * demonstrate on demand: whether a lookup is worth repeating, and whether a
 * failure is reported as "no such host" or "try again". A host test can ask them
 * directly (tests/netstack/host/test_dns_status_host.c); netstack_dns.c cannot
 * be compiled off the Amiga.
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
 * "nothing came back" arrive as the same status and both land here. They are
 * separated by how long the attempt took instead -- see netstack_retry.h.
 */
BOOL ami_ns_dns_again(UINT status);

/*
 * Map a NetX Duo DNS status onto an actionable error. The distinction is
 * between "no resolver configured or reachable" and "that name does not
 * exist"; reporting the second as a device failure sends the reader to check
 * cables over a mistyped host name.
 */
LONG ami_ns_dns_error(UINT status);

#endif /* AMINETXDUO_NETSTACK_DNS_STATUS_H */
