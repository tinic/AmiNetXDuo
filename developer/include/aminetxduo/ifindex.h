/*
 * RFC 3493 section 4 -- interface identification.
 *
 * The first extension to the bsdsocket LVO table since the NDK stopped moving
 * in 2006. Four vectors at -0x372 .. -0x384, continuing past the end of
 * Commodore's SFD rather than into its ==reserve 6 block at -0x33c..-0x35a:
 * that block is what a regenerated SFD would fill first, and it holds six
 * slots against an extension that will outgrow them. docs/NDK-ADDENDUM.md has
 * the reasoning and the LVO arithmetic.
 *
 * Present from bsdsocket.library revision 3. A caller that may meet an older
 * one checks lib_Revision before calling; there is no other way to tell, and
 * the slots are past the end of every published table, so an older library
 * has MakeLibrary()'s (APTR)-1 terminator there and jumping to it is a guru.
 *
 *     struct Library *b = OpenLibrary("bsdsocket.library", 4);
 *     if (b != NULL && b->lib_Revision >= AMI_IFINDEX_MIN_REVISION)
 *         ... if_nametoindex() and friends are safe to call ...
 *
 * This header is the types and the constants; the glue that reaches the
 * vectors is generated from developer/sfd/aminetxduo_lib.sfd, and a caller
 * outside this tree includes <proto/aminetxduo.h>, which brings both. The
 * two ship together in the archive's Developer drawer.
 *
 * Indices are 1-based, as RFC 3493 requires ("1, 2, ..."), and are the same
 * numbers GetRouteInfo() reports in rtm_index. The two conventions are one
 * decision: a caller matching a route to an interface compares them directly,
 * so neither moves without the other.
 *
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
 * The array if_nameindex() returns is terminated by an entry whose if_index is
 * 0 and whose if_name is NULL, and is freed only by if_freenameindex(): the
 * names live inside the same allocation as the structures, so freeing a name
 * separately would free the middle of a block.
 */
struct if_nameindex
{
    ULONG   if_index;       /* 1, 2, ...                                    */
    char   *if_name;        /* null terminated: "eth0", ...                 */
};

/*
 * if_nametoindex(): the index of `ifname`, or 0. RFC 3493 defines no errors
 * for it, so errno is not set -- 0 is the whole answer.
 *
 * if_indextoname(): writes into `ifname`, which must have room for
 * IF_NAMESIZE bytes, and returns it. NULL with errno ENXIO when no interface
 * has that index, ENOMEM on a system error.
 *
 * if_nameindex(): every interface, or NULL. if_freenameindex() takes what it
 * returned, and does nothing with NULL.
 *
 * Loopback is in all three, called "lo0" and numbered last. It is the index an
 * arriving datagram over ::1 or 127.0.0.1 reports in an RFC 3542 PKTINFO, and
 * the one a send may name to leave that way. It is not in
 * ObtainInterfaceList() or QueryInterfaceTagList(), which are about SANA-II
 * interfaces a caller can configure.
 */
ULONG                if_nametoindex(const char *ifname);
char                *if_indextoname(ULONG ifindex, char *ifname);
struct if_nameindex *if_nameindex(VOID);
VOID                 if_freenameindex(struct if_nameindex *ptr);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_IFINDEX_H */
