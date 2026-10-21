/*
 * RFC 3493 section 4.  Indices are 1-based and must stay the same numbers
 * GetRouteInfo() reports in rtm_index. Callers must establish AmiNetXDuo
 * identity and support before using these extension vectors; see capabilities.h.
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_IFINDEX_H
#define AMINETXDUO_IFINDEX_H

#include <exec/types.h>
#include <net/if.h>
#include <aminetxduo/capabilities.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The revision that introduced these four. */
#define AMI_IFINDEX_MIN_REVISION    3

#define AMI_IF_NAMETOINDEX_LVO      (-0x372)
#define AMI_IF_INDEXTONAME_LVO      (-0x378)
#define AMI_IF_NAMEINDEX_LVO        (-0x37e)
#define AMI_IF_FREENAMEINDEX_LVO    (-0x384)

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_IFINDEX_H */
