/*
 * bsdsocket.library -- what the interface API publishes to its neighbours.
 *
 * interfaces.c owns the answer to "which slot is the interface called X",
 * because it owns the naming rule: the name from DEVS:NetInterfaces if there
 * is one, otherwise the name NetX Duo gave the slot, compared without regard
 * to case. addralloc.c has to ask the same question -- CreateAddrAllocMessageA()
 * returns CAAME_Interface_not_found for a name nothing answers to -- and a
 * second implementation of that rule is a second place for the two APIs to
 * disagree about what an interface is called.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_BSDSOCKET_INTERFACES_H
#define AMINETXDUO_BSDSOCKET_INTERFACES_H

#include "bsdsocket_internal.h"

/*
 * "This name cannot be longer than 15 characters" -- QueryInterfaceTagList()
 * and AddInterfaceTagList() both say so, so 15 plus a NUL is the whole name
 * storage this API ever needs. struct AddressAllocationMessage's
 * aam_InterfaceName is sixteen bytes for the same reason.
 */
#define BSD_IFNAME_SIZE     16

/* The physical slot called `name`, or -1. */
LONG bsd_if_index_of(NX_IP *ip, const char *name);

#endif /* AMINETXDUO_BSDSOCKET_INTERFACES_H */
