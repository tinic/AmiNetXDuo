/*
 * AmiNetXDuo, the three primitives RFC 6724 source address selection is built
 * from, exposed so tests/ipv6/host can drive them one at a time.
 *
 * _nxd_ipv6_interface_find() itself needs no header: it replaces a NetX Duo
 * symbol and is declared by nx_ipv6.h.  These three are here because each one
 * is a table or a definition out of the RFC that is worth checking against the
 * RFC directly, rather than only through the address the whole routine
 * happens to pick.  The §2.1 policy table in particular has rows -- the
 * IPv4-mapped and IPv4-compatible ones -- whose effect on selection is
 * invisible on a node that holds no address of those kinds, so reading them
 * back is the only way they can be tested at all.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_IPV6_SRCSEL_H
#define AMINETXDUO_IPV6_SRCSEL_H

#include "nx_api.h"

#ifdef FEATURE_NX_IPV6

#ifdef __cplusplus
extern "C" {
#endif

/*
 * RFC 6724 §2.2 CommonPrefixLen without the cap: how many leading bits the two
 * addresses agree on, 0..128.  Both are host byte order, four longwords, the
 * shape everything in NX_IP uses.
 */
UINT anx6_common_prefix_len(const ULONG *a, const ULONG *b);

/*
 * RFC 4007 §4 scope, as the value it would carry in a multicast address:
 * 1 interface-local, 2 link-local, 5 site-local, 0xE global.
 */
UINT anx6_scope(const ULONG *addr);

/*
 * The RFC 6724 §2.1 policy table row whose prefix is the longest match for
 * addr.  Returns the row's prefix length; precedence and label are written
 * through when not NX_NULL.
 */
UINT anx6_policy_lookup(const ULONG *addr, UINT *precedence, UINT *label);

#ifdef __cplusplus
}
#endif

#endif /* FEATURE_NX_IPV6 */

#endif /* AMINETXDUO_IPV6_SRCSEL_H */
