/*
 * bsdsocket.library, interface lookup shared between interfaces.c and its
 * callers.
 *
 * interfaces.c holds the naming rule: the name from DEVS:NetInterfaces if
 * there is one, otherwise the name NetX Duo gave the slot, compared
 * case-insensitively. addralloc.c needs the same lookup,
 * CreateAddrAllocMessageA() returns CAAME_Interface_not_found for an unknown
 * name, so the rule is implemented once and declared here.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_BSDSOCKET_INTERFACES_H
#define AMINETXDUO_BSDSOCKET_INTERFACES_H

#include "bsdsocket_internal.h"

/*
 * QueryInterfaceTagList() and AddInterfaceTagList() both cap interface names
 * at 15 characters, so 15 plus a NUL covers everything this API stores.
 * struct AddressAllocationMessage's aam_InterfaceName is 16 bytes for the same
 * reason.
 */
#define BSD_IFNAME_SIZE     16

/* The physical slot called `name`, or -1. */
LONG bsd_if_index_of(NX_IP *ip, const char *name);

/* The name of interface `index` counting from 1, as RFC 3493 and rtm_index do.
   Sets no errno: display paths call it, and a caller checking errno after a
   call that succeeded must not find it moved. FALSE and out[0]=0 if there is
   no such interface. */
BOOL bsd_if_name_by_index(NX_IP *ip, ULONG index, char *out, ULONG outlen);

/*
 * SIOCGIFCONF and the SIOCGIF* family, for options.c's IoctlSocket().
 * Implemented in interfaces.c because answering them needs the naming rule and
 * the interface gather. Returns 0, or -1 with errno set.
 */
LONG bsd_if_ioctl(ULONG req, APTR argp, struct AmiSocketBase *SocketBase);

/*
 * From addralloc.c: TRUE while an address allocation Process is still running.
 * Declared here rather than in its own header because it has one caller,
 * bsd_lib_expunge(), which must decline while a worker is executing out of the
 * segment it would hand to UnLoadSeg(). Same reason as
 * bsd_tcp_handler_alive().
 */
BOOL bsd_aam_busy(VOID);

#endif /* AMINETXDUO_BSDSOCKET_INTERFACES_H */
