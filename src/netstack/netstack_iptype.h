/*
 * AmiNetXDuo, what the interface files' IPTYPE lines add up to.
 *
 * The address bring-up in netstack.c asks three questions of the configured
 * interfaces as a set: does anything want DHCP, does anything want an IPv4
 * address at all, and which slot the RFC 3927 fallback should serve.  The
 * answers decide what is started and what is waited for, so they are here
 * where a host test can put a table of interfaces in front of them.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETSTACK_IPTYPE_H
#define AMINETXDUO_NETSTACK_IPTYPE_H

#include <exec/types.h>

#include "aminetxduo/config.h"

/* Whether any of the first `count` interfaces has this IPTYPE. */
BOOL ami_ns_iptype_any(const AmiIfConfig *iface, UWORD count, AmiIpType type);

/* Whether any of the first `count` interfaces wants an IPv4 address: DHCP,
   link-local, or a static one with an address given. */
BOOL ami_ns_iptype_any_ipv4(const AmiIfConfig *iface, UWORD count);

/*
 * The slot the AutoIP client should serve.  A `requested` slot of 0 or more
 * is taken as it is, provided it exists and wants IPv4; -1 means pick one:
 * the first interface configured LINKLOCAL, else the first that wants IPv4
 * at all.  -1 back when there is no such interface.
 */
LONG ami_ns_iptype_autoip_slot(const AmiIfConfig *iface, UWORD count,
                               LONG requested);

/* 169.254.0.0/16, the RFC 3927 range. */
BOOL ami_ns_iptype_linklocal(ULONG addr);

#endif /* AMINETXDUO_NETSTACK_IPTYPE_H */
