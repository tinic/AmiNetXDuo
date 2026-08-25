/*
 * RFC 3493 section 4.  Indices are 1-based and must stay the same numbers
 * GetRouteInfo() reports in rtm_index.  A caller must check lib_Revision >=
 * AMI_IFINDEX_MIN_REVISION first: an older library has (APTR)-1 in these slots.
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_IFINDEX_H
#define AMINETXDUO_IFINDEX_H

#include <exec/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* "IF_NAMESIZE ... includes space for a terminating null byte." */
#define IF_NAMESIZE                 16

/* The revision that introduced these four. */
#define AMI_IFINDEX_MIN_REVISION    3

#define AMI_IF_NAMETOINDEX_LVO      (-0x372)
#define AMI_IF_INDEXTONAME_LVO      (-0x378)
#define AMI_IF_NAMEINDEX_LVO        (-0x37e)
#define AMI_IF_FREENAMEINDEX_LVO    (-0x384)

/*
 * Terminated by an entry with if_index 0 and if_name NULL, and freed only by
 * if_freenameindex(): the names live inside the same allocation.
 */
struct if_nameindex
{
    ULONG   if_index;       /* 1, 2, ...                                    */
    char   *if_name;        /* null terminated: "eth0", ...                 */
};

/*
 * if_nametoindex() returns 0 and sets no errno.  if_indextoname()'s `ifname`
 * needs room for IF_NAMESIZE bytes; NULL + ENXIO for an unknown index, ENOMEM
 * on a system error.  Loopback is in all three, "lo0", numbered last.
 */
ULONG                if_nametoindex(const char *ifname);
char                *if_indextoname(ULONG ifindex, char *ifname);
struct if_nameindex *if_nameindex(VOID);
VOID                 if_freenameindex(struct if_nameindex *ptr);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_IFINDEX_H */
