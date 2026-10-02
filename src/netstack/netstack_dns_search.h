/*
 * AmiNetXDuo, the resolver's search-list walk: the bare name, then the name
 * under each search domain, inside one budget.
 *
 * Its own translation unit, like netstack_dns_status.c, so a host test drives
 * it with a scripted lookup and a fake clock
 * (tests/netstack/host/test_dns_search_host.c).  It knows nothing about DNS.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETSTACK_DNS_SEARCH_H
#define AMINETXDUO_NETSTACK_DNS_SEARCH_H

/* tx_api.h before any exec header: <exec/types.h> turns VOID into a macro and
   that breaks the ThreadX typedefs. */
#include "tx_api.h"

#include <exec/types.h>

#include "aminetxduo/netstack.h"

/* RFC 1035 2.3.4: 255 octets of domain name, plus the NUL. */
#define AMI_DNS_NAME_MAX    256

typedef struct
{
    /* One name, DNS included, in at most `ticks` (0 = no query, see
       netstack.h). */
    LONG (*once)(const char *name, VOID *out, ULONG ticks,
                 AmiNetGiveUpFn give_up, VOID *give_up_arg);

    /* DEVS:Internet/hosts only.  NONAME on a miss. */
    LONG (*local)(const char *name, VOID *out);

    /* "name.suffix[at]" into `qualified`; `count_out`, when not NULL, is the
       number of suffixes now configured. */
    BOOL (*suffix_at)(const char *name, UWORD at, char *qualified,
                      ULONG qualified_size, UWORD *count_out);
} AmiNsSearchOps;

/*
 * The bare name, then, for a name with no dot, each search suffix.
 *
 * `timeout_ticks` is the whole walk: the clock is read once and each suffix
 * gets what is left.  A suffix reached with nothing left is looked up in
 * DEVS:Internet/hosts only.  give_up() is asked before every suffix and
 * AMI_NET_ERR_ABORTED is returned as is; any other failure reports the bare
 * name's.
 */
LONG ami_ns_search(const AmiNsSearchOps *ops, const char *name, VOID *out,
                   ULONG timeout_ticks, AmiNetGiveUpFn give_up,
                   VOID *give_up_arg);

#endif /* AMINETXDUO_NETSTACK_DNS_SEARCH_H */
