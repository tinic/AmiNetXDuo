/*
 * AmiNetXDuo, RFC 6724 source address selection.
 *
 * WHAT THIS REPLACES.  The top-level CMakeLists drops
 * third_party/netxduo/common/src/nxd_ipv6_interface_find.c from the vendored
 * source glob, so _nxd_ipv6_interface_find() is undefined and every caller
 * inside NetX Duo -- UDP send, TCP connect, the raw send, ICMPv6 echo, error
 * message and router solicitation -- and every caller in src/bsdsocket and
 * src/netstack resolves to this one.  No vendored file is edited and no call
 * site is patched, the same mechanism src/net68k uses for the IP checksum.
 *
 * WHY.  The vendored routine calls itself an RFC 6724 selection and is a
 * per-interface walk that breaks on the first link-local hit when the
 * destination is link-local, on the first address whose own prefix covers the
 * destination otherwise, and on the first non-link-local address on a default
 * router's interface after that.  There is no candidate set, no policy table
 * and none of the rules: the answer depends on the order addresses were
 * configured in.  With one global and one link-local on one interface that is
 * usually the same answer this file gives.  With two globals, with a
 * deprecated address, or with two interfaces, it is not.
 *
 * WHAT THIS IS.  §4 builds the candidate set, §5 orders it.  The comparator
 * below is the rule list in the RFC's order, and each rule that is NOT
 * implemented says at its place in the order why the information it needs
 * does not exist on this machine, rather than being left out silently:
 *
 *   Rule 1  prefer same address                  implemented
 *   Rule 2  prefer appropriate scope             implemented
 *   Rule 3  avoid deprecated addresses           implemented
 *   Rule 4  prefer home addresses                NOT APPLICABLE, see below
 *   Rule 5  prefer outgoing interface            implemented
 *   Rule 5.5 prefer a prefix advertised by the
 *           next hop                             NOT APPLICABLE, see below
 *   Rule 6  prefer matching label                implemented, §2.1 table
 *   Rule 7  prefer temporary addresses           NOT APPLICABLE, see below
 *   Rule 8  use longest matching prefix          implemented
 *
 * The §2.1 policy table is here in full, all nine rows.  Source selection
 * reads the label column only.  The precedence column belongs to destination
 * address ordering (§6), which this machine does not do and cannot: the
 * resolver under getaddrinfo() answers with one address per family (see the
 * header of src/bsdsocket/addrinfo.c), so there is never a set to order.  The
 * column is carried because the table is the table, and because destination
 * ordering becomes possible the day the resolver returns more than one answer.
 *
 * THE CONTRACT IS UNCHANGED.  Same signature, same NX_SUCCESS /
 * NX_NO_INTERFACE_ADDRESS, same meaning for if_ptr (search that interface
 * only).  The one observable difference beyond the ordering is Rule 3: a
 * DEPRECATED address is now a candidate of last resort rather than invisible,
 * which is what §5 says it is.  Nothing in this tree deprecates an address
 * yet -- the preferred lifetime from a router advertisement is not tracked --
 * so today that arm is reached only by a caller that sets the state itself.
 * It is implemented now so that selection is already right when the lifetime
 * work lands, rather than being a second thing to remember then.
 *
 * SPDX-License-Identifier: MIT
 */

#define NX_SOURCE_CODE

#include "nx_api.h"
#include "nx_ipv6.h"

#include "ipv6_srcsel.h"

#ifdef FEATURE_NX_IPV6

/* RFC 4007 §4 scope values, as they appear in a multicast address. */
#define ANX6_SCOPE_INTERFACE    0x1U
#define ANX6_SCOPE_LINK         0x2U
#define ANX6_SCOPE_SITE         0x5U
#define ANX6_SCOPE_GLOBAL       0xEU

/*
 * RFC 6724 §2.1, the default policy table, longest matching prefix wins.
 *
 * The rows are in the RFC's order, which is not the lookup order: the lookup
 * below compares prefix lengths, so ::/0 sitting second is not a catch-all
 * that swallows the rows after it.  Written in the RFC's order so the table
 * can be read against §2.1 line by line.
 */
typedef struct ANX6_POLICY_STRUCT
{
    ULONG anx6_policy_prefix[4];
    UCHAR anx6_policy_length;
    UCHAR anx6_policy_precedence;
    UCHAR anx6_policy_label;
} ANX6_POLICY;

static const ANX6_POLICY anx6_policy_table[] =
{
    /*  prefix                              len  prec  label */
    { { 0x00000000UL, 0, 0, 0x00000001UL }, 128,   50,   0 },  /* ::1/128       */
    { { 0x00000000UL, 0, 0, 0x00000000UL },   0,   40,   1 },  /* ::/0          */
    { { 0x00000000UL, 0, 0x0000FFFFUL, 0 },  96,   35,   4 },  /* ::ffff:0:0/96 */
    { { 0x20020000UL, 0, 0, 0x00000000UL },  16,   30,   2 },  /* 2002::/16     */
    { { 0x20010000UL, 0, 0, 0x00000000UL },  32,    5,   5 },  /* 2001::/32     */
    { { 0xFC000000UL, 0, 0, 0x00000000UL },   7,    3,  13 },  /* fc00::/7      */
    { { 0x00000000UL, 0, 0, 0x00000000UL },  96,    1,   3 },  /* ::/96         */
    { { 0xFEC00000UL, 0, 0, 0x00000000UL },  10,    1,  11 },  /* fec0::/10     */
    { { 0x3FFE0000UL, 0, 0, 0x00000000UL },  16,    1,  12 }   /* 3ffe::/16     */
};

#define ANX6_POLICY_ROWS \
    (sizeof(anx6_policy_table) / sizeof(anx6_policy_table[0]))


/*
 * RFC 6724 §2.2 CommonPrefixLen, without the cap the caller applies: the
 * number of leading bits the two addresses agree on, 0..128.
 */
UINT anx6_common_prefix_len(const ULONG *a, const ULONG *b)
{
UINT  i;
UINT  bit;
ULONG diff;

    for (i = 0; i < 4; i++)
    {
        diff = a[i] ^ b[i];

        if (diff == 0)
        {
            continue;
        }

        for (bit = 0; bit < 32; bit++)
        {
            if (diff & (0x80000000UL >> bit))
            {
                break;
            }
        }

        return (i * 32) + bit;
    }

    return 128;
}


static UINT anx6_same_address(const ULONG *a, const ULONG *b)
{
    return (UINT)((a[0] == b[0]) && (a[1] == b[1]) &&
                  (a[2] == b[2]) && (a[3] == b[3]));
}


/*
 * Does `prefix`/`length` cover `addr`?  Masked longword compares rather than
 * anx6_common_prefix_len() >= length: this runs nine times per policy lookup
 * and once per address in the on-link walk, and every one of the §2.1 rows
 * but two is answered by the first longword.  The bit position inside the
 * longword, which is what makes CommonPrefixLen expensive, is never needed
 * here.
 */
static UINT anx6_prefix_covers(const ULONG *prefix, UINT length,
                               const ULONG *addr)
{
UINT  i;
UINT  bits;
ULONG mask;

    for (i = 0; (i < 4) && (length > 0); i++)
    {
        bits = (length >= 32) ? 32 : length;
        mask = (bits == 32) ? 0xFFFFFFFFUL : ~(0xFFFFFFFFUL >> bits);

        if (((prefix[i] ^ addr[i]) & mask) != 0)
        {
            return 0;
        }

        length -= bits;
    }

    return 1;
}


/*
 * RFC 4007 §4 scope of a unicast or multicast address.
 *
 * ::1 is link-local: RFC 4007 §4 describes the loopback address as the
 * link-local address of a virtual interface to a link that goes nowhere, and
 * that is what keeps it out of the candidate set for anything but itself.
 *
 * An IPv4-mapped address takes the scope of the address inside it, RFC 6724
 * §3.1: 169.254/16 and 127/8 are link-local, everything else global.  Nothing
 * here ever holds one -- this port does not put IPv4-mapped addresses on an
 * interface -- but the destination can be one, and its scope decides which of
 * our addresses Rule 2 will accept.
 */
UINT anx6_scope(const ULONG *addr)
{
    /* ff00::/8, scope is the low nibble of the second octet. */
    if ((addr[0] & 0xFF000000UL) == 0xFF000000UL)
    {
        return (UINT)((addr[0] >> 16) & 0xFUL);
    }

    /* fe80::/10 */
    if ((addr[0] & 0xFFC00000UL) == 0xFE800000UL)
    {
        return ANX6_SCOPE_LINK;
    }

    /* fec0::/10, deprecated by RFC 4291 and still scope 5 where it appears. */
    if ((addr[0] & 0xFFC00000UL) == 0xFEC00000UL)
    {
        return ANX6_SCOPE_SITE;
    }

    if ((addr[0] == 0) && (addr[1] == 0))
    {
        /* ::1 */
        if ((addr[2] == 0) && (addr[3] == 1))
        {
            return ANX6_SCOPE_LINK;
        }

        /* ::ffff:0:0/96 */
        if (addr[2] == 0x0000FFFFUL)
        {
            if (((addr[3] & 0xFFFF0000UL) == 0xA9FE0000UL) ||    /* 169.254/16 */
                ((addr[3] & 0xFF000000UL) == 0x7F000000UL))      /* 127/8      */
            {
                return ANX6_SCOPE_LINK;
            }
        }
    }

    return ANX6_SCOPE_GLOBAL;
}


/* The §2.1 row whose prefix is the longest match for addr. */
UINT anx6_policy_lookup(const ULONG *addr, UINT *precedence, UINT *label)
{
UINT i;
UINT best_row = 1;                          /* ::/0, matches everything */
UINT best_len = 0;

    for (i = 0; i < ANX6_POLICY_ROWS; i++)
    {
    UINT length = anx6_policy_table[i].anx6_policy_length;

        /* Strictly longer, so ::/0 at row 1 never displaces a row that
           already matched at length 0 -- there is none, ::/0 is the only
           zero-length row, but the test is written to be about length and
           not about which row happens to be first. */
        if ((length > best_len) &&
            anx6_prefix_covers(anx6_policy_table[i].anx6_policy_prefix,
                               length, addr))
        {
            best_row = i;
            best_len = length;
        }
    }

    if (precedence != NX_NULL)
    {
        *precedence = anx6_policy_table[best_row].anx6_policy_precedence;
    }

    if (label != NX_NULL)
    {
        *label = anx6_policy_table[best_row].anx6_policy_label;
    }

    return anx6_policy_table[best_row].anx6_policy_length;
}


static UINT anx6_label(const ULONG *addr)
{
UINT label = 0;

    (VOID)anx6_policy_lookup(addr, NX_NULL, &label);

    return label;
}


/*
 * §4, the candidate set.  An address is a candidate when it is one this node
 * holds and may send from:
 *
 *   * assigned and not TENTATIVE.  Duplicate address detection has not
 *     finished on a tentative address, so it is not this node's yet.  UNKNOWN
 *     is an address that failed detection or was withdrawn.
 *   * on an interface that is up.
 *
 * Multicast and unspecified never appear in nx_ipv6_address[]; the test is
 * here because a caller that writes the table by hand can put one there, and
 * §4 excludes both from the candidate set by name.
 */
static UINT anx6_usable(const NXD_IPV6_ADDRESS *addr)
{
UINT state;

    if (!addr -> nxd_ipv6_address_valid)
    {
        return 0;
    }

    state = addr -> nxd_ipv6_address_state;

    if ((state != NX_IPV6_ADDR_STATE_VALID) &&
        (state != NX_IPV6_ADDR_STATE_PREFERRED) &&
        (state != NX_IPV6_ADDR_STATE_DEPRECATED))
    {
        return 0;
    }

    if (addr -> nxd_ipv6_address_attached == NX_NULL)
    {
        return 0;
    }

    if (addr -> nxd_ipv6_address_attached -> nx_interface_link_up == NX_FALSE)
    {
        return 0;
    }

    /* ff00::/8 and :: */
    if ((addr -> nxd_ipv6_address[0] & 0xFF000000UL) == 0xFF000000UL)
    {
        return 0;
    }

    if ((addr -> nxd_ipv6_address[0] == 0) && (addr -> nxd_ipv6_address[1] == 0) &&
        (addr -> nxd_ipv6_address[2] == 0) && (addr -> nxd_ipv6_address[3] == 0))
    {
        return 0;
    }

    return 1;
}


/*
 * The interface the packet would leave by, which is Rule 5's question and is
 * what §4 means by "the outgoing interface".  RFC 6724 leaves it to the
 * routing table; this is that lookup, in the order the routing table would
 * answer it:
 *
 *   1. the destination is one of ours          -- it leaves by the interface
 *                                                 that address is on
 *   2. ::1                                     -- the loopback interface
 *   3. multicast                               -- the interface the group was
 *                                                 joined on, else the first
 *                                                 interface that is up
 *   4. link-local                              -- on-link everywhere, so the
 *                                                 first interface that is up
 *                                                 and holds an address
 *   5. on-link                                 -- the interface holding an
 *                                                 address whose prefix covers
 *                                                 the destination
 *   6. a default router                        -- its interface
 *
 * NX_NULL when none of those answers, which is a destination with no route and
 * is reported as NX_NO_INTERFACE_ADDRESS.  That refusal is deliberate and is
 * not part of §5: a source address cannot make a destination reachable, and
 * the callers turn the failure into ENETUNREACH, which is the answer a caller
 * can act on.
 */
static NX_INTERFACE *anx6_outgoing_interface(NX_IP *ip_ptr, const ULONG *dest)
{
UINT              i;
UINT              scope;
NXD_IPV6_ADDRESS *addr;
NX_INTERFACE     *first_up = NX_NULL;

    /* 1. a destination this node holds. */
    for (i = 0; i < (NX_MAX_IPV6_ADDRESSES + NX_LOOPBACK_IPV6_ENABLED); i++)
    {
        addr = &ip_ptr -> nx_ipv6_address[i];

        if (anx6_usable(addr) &&
            anx6_same_address(addr -> nxd_ipv6_address, dest))
        {
            return addr -> nxd_ipv6_address_attached;
        }
    }

    scope = anx6_scope(dest);

#ifndef NX_DISABLE_LOOPBACK_INTERFACE
    /* 2. ::1, and nothing else, reaches the loopback interface. */
    if ((dest[0] == 0) && (dest[1] == 0) && (dest[2] == 0) && (dest[3] == 1))
    {
        return &ip_ptr -> nx_ip_interface[NX_LOOPBACK_INTERFACE];
    }
#endif /* NX_DISABLE_LOOPBACK_INTERFACE */

    /*
     * The first physical interface that is up AND HOLDS AN ADDRESS.  Up is not
     * enough: an interface with no usable IPv6 address on it contributes no
     * candidate, so naming it as the outgoing interface for a link-local or
     * an unjoined multicast destination turns a machine that has a link-local
     * on its second card into one with no source at all.
     */
    for (i = 0; (i < (NX_MAX_IPV6_ADDRESSES + NX_LOOPBACK_IPV6_ENABLED)) &&
                (first_up == NX_NULL); i++)
    {
        addr = &ip_ptr -> nx_ipv6_address[i];

        if (!anx6_usable(addr))
        {
            continue;
        }

#ifndef NX_DISABLE_LOOPBACK_INTERFACE
        if (addr -> nxd_ipv6_address_attached ==
            &ip_ptr -> nx_ip_interface[NX_LOOPBACK_INTERFACE])
        {
            continue;
        }
#endif /* NX_DISABLE_LOOPBACK_INTERFACE */

        first_up = addr -> nxd_ipv6_address_attached;
    }

#ifdef NX_ENABLE_IPV6_MULTICAST
    /* 3. a group this node joined leaves by the interface it joined on. */
    if ((dest[0] & 0xFF000000UL) == 0xFF000000UL)
    {
        for (i = 0; i < NX_MAX_MULTICAST_GROUPS; i++)
        {
        NX_INTERFACE *joined =
            ip_ptr -> nx_ipv6_multicast_entry[i].nx_ip_mld_join_interface_list;

            if ((joined != NX_NULL) &&
                (joined -> nx_interface_link_up != NX_FALSE) &&
                anx6_same_address(ip_ptr -> nx_ipv6_multicast_entry[i].nx_ip_mld_join_list,
                                  dest))
            {
                return joined;
            }
        }
    }
#endif /* NX_ENABLE_IPV6_MULTICAST */

    /* 3b/4. multicast this node did not join, and link-local, are on-link on
       every interface, so the first one with an address carries them. */
    if (((dest[0] & 0xFF000000UL) == 0xFF000000UL) || (scope <= ANX6_SCOPE_LINK))
    {
        return first_up;
    }

    /*
     * 5. on-link.  The test is the destination against the prefix of each
     * address the interface holds, which is what covers both a static address
     * and one formed from an advertised prefix, because the address carries
     * the prefix length it was formed with.
     *
     * The prefix list (_nxd_ipv6_search_onlink()) is deliberately not consulted
     * here: its entries carry no interface, so it cannot answer "which one",
     * and a prefix advertised L=1 A=0 -- the only kind that is on the list and
     * has no address formed from it -- leaves nothing to select anyway.
     */
    for (i = 0; i < (NX_MAX_IPV6_ADDRESSES + NX_LOOPBACK_IPV6_ENABLED); i++)
    {
        addr = &ip_ptr -> nx_ipv6_address[i];

        if (!anx6_usable(addr))
        {
            continue;
        }

#ifndef NX_DISABLE_LOOPBACK_INTERFACE
        if (addr -> nxd_ipv6_address_attached ==
            &ip_ptr -> nx_ip_interface[NX_LOOPBACK_INTERFACE])
        {
            continue;
        }
#endif /* NX_DISABLE_LOOPBACK_INTERFACE */

        if (anx6_prefix_covers(addr -> nxd_ipv6_address,
                               addr -> nxd_ipv6_address_prefix_length, dest))
        {
            return addr -> nxd_ipv6_address_attached;
        }
    }

    /* 6. a default router. */
    for (i = 0; i < NX_IPV6_DEFAULT_ROUTER_TABLE_SIZE; i++)
    {
    NX_IPV6_DEFAULT_ROUTER_ENTRY *rt = &ip_ptr -> nx_ipv6_default_router_table[i];

        if (rt -> nx_ipv6_default_router_entry_flag == 0)
        {
            continue;
        }

        if (rt -> nx_ipv6_default_router_entry_interface_ptr == NX_NULL)
        {
            continue;
        }

        if (rt -> nx_ipv6_default_router_entry_interface_ptr -> nx_interface_link_up ==
            NX_FALSE)
        {
            continue;
        }

        return rt -> nx_ipv6_default_router_entry_interface_ptr;
    }

    return NX_NULL;
}


/*
 * ONE CANDIDATE, WITH EVERYTHING THE RULES ASK ABOUT IT ALREADY WORKED OUT.
 *
 * The rules are a pairwise comparison and the loop below is a pass over the
 * table keeping the best so far, so a field read straight out of
 * NXD_IPV6_ADDRESS would be recomputed for every comparison the incumbent
 * takes part in.  The label is the one that matters: it is a walk of the
 * nine-row policy table, and _nxd_ipv6_interface_find() is on the UDP send
 * path (nxd_udp_socket_send.c) and runs once per datagram.
 *
 * Filling this in is O(candidates); comparing is then field reads.
 */
typedef struct ANX6_CANDIDATE_STRUCT
{
    NXD_IPV6_ADDRESS *anx6_cand_address;
    UINT              anx6_cand_same;         /* Rule 1: it IS the destination */
    UINT              anx6_cand_scope;        /* Rule 2 */
    UINT              anx6_cand_deprecated;   /* Rule 3 */
    UINT              anx6_cand_outgoing;     /* Rule 5 */
    UINT              anx6_cand_label_match;  /* Rule 6 */
    UINT              anx6_cand_prefix_len;   /* Rule 8, already capped */
} ANX6_CANDIDATE;


static VOID anx6_describe(ANX6_CANDIDATE *out, NXD_IPV6_ADDRESS *addr,
                          const ULONG *dest, UINT dest_label,
                          const NX_INTERFACE *out_if)
{
UINT length;

    out -> anx6_cand_address = addr;
    out -> anx6_cand_same    = anx6_same_address(addr -> nxd_ipv6_address, dest);
    out -> anx6_cand_scope   = anx6_scope(addr -> nxd_ipv6_address);

    out -> anx6_cand_deprecated =
        (addr -> nxd_ipv6_address_state == NX_IPV6_ADDR_STATE_DEPRECATED) ? 1U : 0U;

    out -> anx6_cand_outgoing =
        (addr -> nxd_ipv6_address_attached == out_if) ? 1U : 0U;

    out -> anx6_cand_label_match =
        (anx6_label(addr -> nxd_ipv6_address) == dest_label) ? 1U : 0U;

    /* §2.2: CommonPrefixLen counts only as far as the source's own prefix. */
    length = anx6_common_prefix_len(addr -> nxd_ipv6_address, dest);
    if (length > addr -> nxd_ipv6_address_prefix_length)
    {
        length = addr -> nxd_ipv6_address_prefix_length;
    }
    out -> anx6_cand_prefix_len = length;
}


/*
 * RFC 6724 §5, the rules, in order.  Positive when sa is preferred, negative
 * when sb is, zero when no rule separates them -- in which case the caller
 * keeps the one it already had, so the answer is the first in table order and
 * is stable rather than arbitrary.
 */
static INT anx6_better(UINT dest_scope,
                       const ANX6_CANDIDATE *sa, const ANX6_CANDIDATE *sb)
{
UINT a;
UINT b;

    /* Rule 1: prefer same address. */
    a = sa -> anx6_cand_same;
    b = sb -> anx6_cand_same;
    if (a != b)
    {
        return a ? 1 : -1;
    }

    /* Rule 2: prefer appropriate scope.  The smaller scope wins only when it
       is still large enough for the destination. */
    a = sa -> anx6_cand_scope;
    b = sb -> anx6_cand_scope;
    if (a != b)
    {
        if (a < b)
        {
            return (a < dest_scope) ? -1 : 1;
        }

        return (b < dest_scope) ? 1 : -1;
    }

    /* Rule 3: avoid deprecated addresses. */
    a = sa -> anx6_cand_deprecated;
    b = sb -> anx6_cand_deprecated;
    if (a != b)
    {
        return a ? -1 : 1;
    }

    /*
     * Rule 4: prefer home addresses.  NOT APPLICABLE.  Mobile IPv6 (RFC 6275)
     * is the only source of a home address and a care-of address, this stack
     * implements no part of it, and nothing in NXD_IPV6_ADDRESS can hold the
     * distinction.  Every address here is a home address by the RFC's own
     * definition, so the rule cannot separate any two of them.
     */

    /* Rule 5: prefer the outgoing interface. */
    a = sa -> anx6_cand_outgoing;
    b = sb -> anx6_cand_outgoing;
    if (a != b)
    {
        return a ? 1 : -1;
    }

    /*
     * Rule 5.5: prefer addresses in a prefix advertised by the next hop.  NOT
     * APPLICABLE.  It needs the link from a prefix to the router that
     * advertised it, and NX_IPV6_PREFIX_ENTRY carries no router: the entry is
     * the prefix, its length, its lifetime and its L bit, and
     * nx_icmpv6_process_ra.c does not record which of the (at most two)
     * default routers put it there.  The information is not merely unread,
     * it is never written, so the rule cannot be evaluated.
     */

    /* Rule 6: prefer matching label, from the §2.1 policy table. */
    a = sa -> anx6_cand_label_match;
    b = sb -> anx6_cand_label_match;
    if (a != b)
    {
        return a ? 1 : -1;
    }

    /*
     * Rule 7: prefer temporary addresses.  NOT APPLICABLE.  Temporary
     * addresses are RFC 8981 privacy extensions and this stack forms none:
     * every autoconfigured address comes from the interface identifier in
     * nx_icmpv6_process_ra.c, which is derived from the MAC and never
     * regenerated.  With no temporary address in nx_ipv6_address[] the rule
     * has nothing to prefer, and there is no field it could read if it had.
     */

    /* Rule 8: use longest matching prefix, §2.2 CommonPrefixLen. */
    a = sa -> anx6_cand_prefix_len;
    b = sb -> anx6_cand_prefix_len;
    if (a != b)
    {
        return (a > b) ? 1 : -1;
    }

    return 0;
}


/**************************************************************************/
/*                                                                        */
/*  FUNCTION                                                              */
/*                                                                        */
/*    _nxd_ipv6_interface_find                                            */
/*                                                                        */
/*  DESCRIPTION                                                           */
/*                                                                        */
/*    Selects the source address for a datagram to dest_address, by        */
/*    RFC 6724 sections 4 and 5, and with it the outgoing interface: the   */
/*    caller reads nxd_ipv6_address_attached off the answer.               */
/*                                                                        */
/*  INPUT                                                                 */
/*                                                                        */
/*    ip_ptr                                Pointer to IP control block   */
/*    dest_address                          Destination IP address        */
/*    ipv6_addr                             Selected source address, out  */
/*    if_ptr                                Restrict the candidate set to */
/*                                            this interface, or NX_NULL  */
/*                                                                        */
/*  OUTPUT                                                                */
/*                                                                        */
/*    NX_SUCCESS, or NX_NO_INTERFACE_ADDRESS when the candidate set is    */
/*    empty or the destination has no route.                              */
/*                                                                        */
/**************************************************************************/
UINT _nxd_ipv6_interface_find(NX_IP *ip_ptr, ULONG *dest_address,
                              NXD_IPV6_ADDRESS **ipv6_addr, NX_INTERFACE *if_ptr)
{
UINT              i;
UINT              dest_scope;
UINT              dest_label;
UINT              have_best = 0;
NX_INTERFACE     *out_if;
NXD_IPV6_ADDRESS *addr;
ANX6_CANDIDATE    best;
ANX6_CANDIDATE    here;

    /* ipv6_addr must not be NULL. */
    NX_ASSERT(ipv6_addr != NX_NULL);

    /*
     * §4: "the outgoing interface".  When the caller names one it is the
     * answer -- nx_icmpv6_send_rs() and the multicast arm of
     * nx_icmpv6_process_echo_request() both do, because the reply has to
     * leave by the interface the request arrived on.
     */
    if (if_ptr != NX_NULL)
    {
        out_if = if_ptr;
    }
    else
    {
        out_if = anx6_outgoing_interface(ip_ptr, dest_address);

        if (out_if == NX_NULL)
        {
            return NX_NO_INTERFACE_ADDRESS;
        }
    }

    dest_scope = anx6_scope(dest_address);
    dest_label = anx6_label(dest_address);

    for (i = 0; i < (NX_MAX_IPV6_ADDRESSES + NX_LOOPBACK_IPV6_ENABLED); i++)
    {
        addr = &ip_ptr -> nx_ipv6_address[i];

        if (!anx6_usable(addr))
        {
            continue;
        }

        /*
         * §4: "For all multicast and link-local destination addresses, the
         * set of candidate source addresses MUST only include addresses
         * assigned to interfaces belonging to the same link as the outgoing
         * interface."  One SANA-II device is one link, so same link is same
         * interface here.
         *
         * This is also what keeps ::1 out of every other answer and everything
         * else out of ::1's: the loopback interface is a link of its own and
         * ::1 is link scope, so a loopback destination admits only loopback
         * addresses and a link-local destination on eth0 admits only eth0's.
         */
        if ((dest_scope <= ANX6_SCOPE_LINK) &&
            (addr -> nxd_ipv6_address_attached != out_if))
        {
            continue;
        }

        /*
         * A destination of larger scope never leaves by loopback, and ::1 is
         * not a legal source for it.  §4 puts this the other way round -- the
         * loopback interface is not the outgoing interface, so its addresses
         * are not in the candidate set -- and this is that, made explicit
         * because the loopback address lives in the same array as the rest.
         */
#ifndef NX_DISABLE_LOOPBACK_INTERFACE
        if ((addr -> nxd_ipv6_address_attached ==
             &ip_ptr -> nx_ip_interface[NX_LOOPBACK_INTERFACE]) &&
            (out_if != &ip_ptr -> nx_ip_interface[NX_LOOPBACK_INTERFACE]))
        {
            continue;
        }
#endif /* NX_DISABLE_LOOPBACK_INTERFACE */

        /* The caller's constraint, which §4 does not know about: it narrows
           the candidate set to one interface and nothing else. */
        if ((if_ptr != NX_NULL) && (addr -> nxd_ipv6_address_attached != if_ptr))
        {
            continue;
        }

        anx6_describe(&here, addr, dest_address, dest_label, out_if);

        if (!have_best || (anx6_better(dest_scope, &here, &best) > 0))
        {
            best      = here;
            have_best = 1;
        }
    }

    if (!have_best)
    {
        return NX_NO_INTERFACE_ADDRESS;
    }

    /*
     * §4's failure case, "no address of appropriate scope": Rule 2 orders the
     * candidates but cannot invent one, and the winner of a set that holds
     * nothing wide enough is still too narrow.  A link-local source on a
     * datagram to a global destination is discarded by the first router it
     * reaches, so the caller is told there is no route rather than being given
     * a source that cannot work -- which is what the routine this replaces
     * did, by never considering the address at all.
     */
    if (best.anx6_cand_scope < dest_scope)
    {
        return NX_NO_INTERFACE_ADDRESS;
    }

    *ipv6_addr = best.anx6_cand_address;

    return NX_SUCCESS;
}

#endif /* FEATURE_NX_IPV6 */
