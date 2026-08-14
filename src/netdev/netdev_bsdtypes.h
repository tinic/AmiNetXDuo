/*
 * dp8390reg.h and ne2000reg.h beside this file are NetBSD's, verbatim, so
 * they spell their one structure in NetBSD's integer types.  Supplying the
 * two names is cheaper than editing an imported header, and keeping the
 * import byte-identical is what lets a future NetBSD revision be diffed
 * against it.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETDEV_BSDTYPES_H
#define AMINETXDUO_NETDEV_BSDTYPES_H

#include <exec/types.h>

typedef UBYTE u_int8_t;
typedef UWORD u_int16_t;

#endif /* AMINETXDUO_NETDEV_BSDTYPES_H */
