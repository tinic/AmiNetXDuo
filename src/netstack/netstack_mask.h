/*
 * AmiNetXDuo, the mask a static interface carries when its file omits NETMASK.
 *
 * nx_ip_interface_attach() takes the mask as a whole word.  A static
 * interface whose file has no NETMASK line arrives as zero, and a zero mask
 * makes NetX Duo answer "is this destination on my network?" as yes to
 * everything (mask 0, network 0).  The start-up pass already substitutes /24
 * for that case, so the runtime add path must too, or the same file behaves
 * differently at boot and at `AddNetInterface`.  Split out of netstack.c on
 * the netstack_gateway.c rule: a file with no NetX Duo call in it can be run
 * by a host test.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETSTACK_MASK_H
#define AMINETXDUO_NETSTACK_MASK_H

#include <exec/types.h>

/*
 * The mask a static interface uses: the configured one, or /24 when the file
 * omitted NETMASK, matching what the start-up pass substitutes.  DHCP must
 * keep a zero mask so the client can read a server's empty mask back, so call
 * this for a static interface only.
 */
ULONG ami_ns_static_netmask(ULONG netmask);

#ifdef AMINETXDUO_PTP
/* The same for an interface that may be point-to-point: such a link has no
   network beyond its own address, so its omitted mask is /32. */
ULONG ami_ns_static_netmask_ptp(ULONG netmask, BOOL ptp);
#endif

#endif /* AMINETXDUO_NETSTACK_MASK_H */
