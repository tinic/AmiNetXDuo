/*
 * bsdsocket.library -- the AF_INET6 ABI, pinned.
 *
 * Compiled only in an AMINETXDUO_IPV6 build.
 *
 * The Roadshow NDK defines the IPv6 socket ABI even though no Amiga stack has
 * ever implemented it, so the shape is not ours to choose, only to verify.
 * Two earlier cases where a header did not say what it looked like it said:
 *
 *   - ndk-include/pwd.h turned out to be newlib's ten-field `struct passwd`
 *     rather than the Amiga's seven-field one, a silent 12-byte difference;
 *   - bpf_set_notify_mask takes its arguments in (d1,d0) where its immediate
 *     neighbour in the LVO table takes them in (d0,d1).
 *
 * So every offset an application will rely on is asserted below.  A toolchain
 * whose netinet/in.h differs is a build failure here, not a wrong answer at
 * run time.
 *
 * Verified against amigaos/tools/m68k-amigaos-gcc/m68k-amigaos/ndk-include on
 * 2026-07-25:
 *
 *   sys/socket.h:110   typedef unsigned char sa_family_t;
 *   sys/socket.h:196   #define AF_INET6 23
 *   netinet/in.h:87    typedef unsigned short in_port_t;
 *   netinet/in.h:170   struct sockaddr_in { __UBYTE sin_len; sa_family_t
 *                        sin_family; ... }       <-- 4.4BSD, has a length byte
 *   netinet/in.h:178   struct in6_addr { unsigned char s6_addr[16]; }
 *   netinet/in.h:182   struct sockaddr_in6 { sa_family_t sin6_family; ... }
 *                                              <-- Linux, no length byte
 *
 * So `sockaddr_in` and `sockaddr_in6` in the same header disagree about where
 * the address family lives: offset 1 in one, offset 0 in the other.  Casting a
 * sockaddr_in6 to `struct sockaddr *` and reading sa_family therefore reads
 * the padding byte the compiler inserted in front of sin6_port.
 * bsd_sa_family() in socket.c never does that; it decides from the bytes plus
 * the declared length.
 *
 * Nothing here may write a length byte into a sockaddr_in6 the way
 * bsd_sockaddr_put() writes sin_len into a sockaddr_in: on this NDK that byte
 * is sin6_family.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include "aminetxduo/config.h"

/* ---------------------------------------------------------------- the ABI */

_Static_assert(AF_INET6 == 23, "AF_INET6 is not the Roadshow NDK's 23");

_Static_assert(sizeof(struct in6_addr) == 16, "in6_addr is not 16 bytes");

/*
 * 28 bytes: family(1) + pad(1) + port(2) + flowinfo(4) + addr(16) + scope(4).
 * The pad at offset 1 is not a length byte -- see the header comment.
 */
_Static_assert(sizeof(struct sockaddr_in6) == 28,
               "sockaddr_in6 is not the 28-byte no-sin6_len shape");
_Static_assert(offsetof(struct sockaddr_in6, sin6_family)   ==  0,
               "sin6_family moved -- is this NDK's sockaddr_in6 4.4BSD after "
               "all, with a sin6_len at offset 0?");
_Static_assert(offsetof(struct sockaddr_in6, sin6_port)     ==  2,
               "sin6_port moved");
_Static_assert(offsetof(struct sockaddr_in6, sin6_flowinfo) ==  4,
               "sin6_flowinfo moved");
_Static_assert(offsetof(struct sockaddr_in6, sin6_addr)     ==  8,
               "sin6_addr moved");
_Static_assert(offsetof(struct sockaddr_in6, sin6_scope_id) == 24,
               "sin6_scope_id moved");

_Static_assert(sizeof(((struct sockaddr_in6 *)0)->sin6_family) == 1,
               "sa_family_t is not one byte");
_Static_assert(sizeof(((struct sockaddr_in6 *)0)->sin6_port) == 2,
               "in_port_t is not two bytes");
_Static_assert(sizeof(((struct sockaddr_in6 *)0)->sin6_flowinfo) == 4,
               "sin6_flowinfo is not four bytes");
_Static_assert(sizeof(((struct sockaddr_in6 *)0)->sin6_scope_id) == 4,
               "sin6_scope_id is not four bytes");

/* The IPv4 side, restated: the layout sockaddr_in6 does not share. */
_Static_assert(sizeof(struct sockaddr_in) == 16, "sockaddr_in is not 16 bytes");
_Static_assert(offsetof(struct sockaddr_in, sin_len)    == 0, "sin_len moved");
_Static_assert(offsetof(struct sockaddr_in, sin_family) == 1, "sin_family moved");

/* NetX Duo's side of the same address. */
_Static_assert(sizeof(((NXD_ADDRESS *)0)->nxd_ip_address.v6) == 16,
               "NXD_ADDRESS.v6 is not four ULONGs");

/* ------------------------------------------------------ address conversion */

/*
 * struct in6_addr is 16 bytes in network order; NetX Duo keeps four ULONGs in
 * host order with [0] most significant.  On m68k those are the same bytes, so
 * a memcpy would work, but would break silently on a little-endian build.  The
 * shifts cost four instructions per word, the same trade the BSD_HTONL macros
 * make.
 */
VOID bsd_in6_to_words(const UBYTE bytes[16], ULONG words[4])
{
    ULONG i;

    for (i = 0; i < 4; i++)
    {
        words[i] = ((ULONG)bytes[i * 4]     << 24) |
                   ((ULONG)bytes[i * 4 + 1] << 16) |
                   ((ULONG)bytes[i * 4 + 2] <<  8) |
                    (ULONG)bytes[i * 4 + 3];
    }
}

VOID bsd_words_to_in6(const ULONG words[4], UBYTE bytes[16])
{
    ULONG i;

    for (i = 0; i < 4; i++)
    {
        bytes[i * 4]     = (UBYTE)(words[i] >> 24);
        bytes[i * 4 + 1] = (UBYTE)(words[i] >> 16);
        bytes[i * 4 + 2] = (UBYTE)(words[i] >>  8);
        bytes[i * 4 + 3] = (UBYTE)(words[i]);
    }
}

BOOL bsd_addr_is_v4mapped(const NXD_ADDRESS *addr, ULONG *v4)
{
    if (addr->nxd_ip_version != NX_IP_VERSION_V6)
        return FALSE;

    if (addr->nxd_ip_address.v6[0] != 0UL ||
        addr->nxd_ip_address.v6[1] != 0UL ||
        addr->nxd_ip_address.v6[2] != 0x0000FFFFUL)
        return FALSE;

    if (v4 != NULL)
        *v4 = addr->nxd_ip_address.v6[3];

    return TRUE;
}

VOID bsd_addr_to_v4mapped(NXD_ADDRESS *addr, ULONG v4)
{
    addr->nxd_ip_version       = NX_IP_VERSION_V6;
    addr->nxd_ip_address.v6[0] = 0UL;
    addr->nxd_ip_address.v6[1] = 0UL;
    addr->nxd_ip_address.v6[2] = 0x0000FFFFUL;
    addr->nxd_ip_address.v6[3] = v4;
}

BOOL bsd_addr_normalise(const AmiSocket *sock, NXD_ADDRESS *addr)
{
    ULONG v4;

    if (!bsd_addr_is_v4mapped(addr, &v4))
        return TRUE;                    /* a real IPv6 address; nothing to do */

    /*
     * A V6ONLY socket may not talk to an IPv4 host at all, mapped notation or
     * not.
     */
    if ((sock->as_Flags & ASF_V6ONLY) != 0)
        return FALSE;

    /*
     * On a dual-stack socket the mapped form is a spelling of an IPv4 address,
     * so convert it.  It cannot be left as-is: NetX Duo has no concept of a
     * v4-mapped destination and would build an IPv6 header addressed to
     * ::ffff:a.b.c.d, then hand it to neighbour discovery, which would look
     * for the MAC of a host that is not speaking IPv6.
     *
     * The unspecified address (::) is left alone: it is a wildcard for bind(),
     * not a destination, and the port checks catch it.
     */
    bsd_addr_from_v4(addr, v4);

    return TRUE;
}

/* --------------------------------------------------------- socket options */

/*
 * Level IPPROTO_IPV6.
 *
 * The option numbers are not in the NDK and differ between BSD and Linux, so
 * both numberings are accepted -- see the note in bsdsocket_internal.h for why
 * that is safe.  An option that cannot be implemented correctly is refused
 * with ENOPROTOOPT rather than accepted and ignored, the same rule MSG_OOB is
 * held to in transfer.c.
 *
 * Refused, and why:
 *   IPV6_MULTICAST_HOPS/_IF/_LOOP, IPV6_JOIN_GROUP, IPV6_LEAVE_GROUP
 *       -- IPv6 multicast membership needs NX_ENABLE_IPV6_MULTICAST, which the
 *          floor-target tuning leaves off (port/netxduo-amiga/inc/nx_user.h).
 *          A join that never happens is worse than a refused one.
 *   IPV6_RTHDR, HOPOPTS, DSTOPTS, RTHDRDSTOPTS, PATHMTU, RECVPATHMTU,
 *   USE_MIN_MTU, DONTFRAG, NEXTHOP
 *       -- the rest of RFC 3542.  The subset that is implemented -- PKTINFO,
 *          HOPLIMIT and ICMP6_FILTER -- lives in cmsg.c and is dispatched
 *          from here; these name IPv6 extension headers and path-MTU state
 *          NetX Duo does not expose.
 *   IPV6_CHECKSUM
 *       -- names the offset of a checksum field the stack should fill in for
 *          an arbitrary raw protocol.  ICMPv6's is filled in unconditionally
 *          (raw.c) because RFC 4443 makes it mandatory and the sender cannot
 *          compute it; nothing else raw.c carries has one to place.
 */

static LONG bsd_v6only_option(LONG optname)
{
    return (optname == AMI_IPV6_V6ONLY_BSD ||
            optname == AMI_IPV6_V6ONLY_LINUX);
}

static LONG bsd_hops_option(LONG optname)
{
    return (optname == AMI_IPV6_UNICAST_HOPS_BSD ||
            optname == AMI_IPV6_UNICAST_HOPS_LINUX);
}

static LONG bsd_tclass_option(LONG optname)
{
    return (optname == AMI_IPV6_TCLASS_BSD ||
            optname == AMI_IPV6_TCLASS_LINUX);
}

LONG bsd_setsockopt_ipv6(struct AmiSocketBase *base, AmiSocket *sock,
                         LONG level, LONG optname, APTR optval,
                         socklen_t optlen)
{
    LONG value = 0;
    LONG owned;

    if ((sock->as_Flags & ASF_INET6) == 0)
        return bsd_fail(base, AMI_ENOPROTOOPT);

    /* RFC 3542's ancillary-data options, and the whole of level
       IPPROTO_ICMPV6, belong to cmsg.c. */
    owned = bsd_cmsg_option(base, sock, level, optname, optval, &optlen, TRUE);
    if (owned <= 0)
        return owned;

    if (optval == NULL)
        return bsd_fail(base, AMI_EFAULT);

    if (optlen >= (socklen_t)sizeof(LONG))
        value = *(LONG *)optval;
    else if (optlen >= (socklen_t)sizeof(WORD))
        value = *(WORD *)optval;
    else
        return bsd_fail(base, AMI_EINVAL);

    if (bsd_v6only_option(optname))
    {
        /*
         * BSD requires this to be set before bind(); after that the socket's
         * behaviour is fixed.  Enforced rather than accepted, so an
         * application that gets the order wrong sees the same EINVAL it would
         * get on a real stack.
         */
        if ((sock->as_Flags & (ASF_BOUND | ASF_CONNECTED)) != 0)
            return bsd_fail(base, AMI_EINVAL);

        if (value != 0)
            sock->as_Flags |= ASF_V6ONLY;
        else
            sock->as_Flags &= ~ASF_V6ONLY;

        return 0;
    }

    if (bsd_hops_option(optname))
    {
        /*
         * The IPv6 hop limit is the IPv4 TTL under another name, and NetX Duo
         * stores one per socket, so IP_TTL and IPV6_UNICAST_HOPS are the same
         * setting here.  -1 means "use the default", per RFC 3493.
         */
        if (value < -1 || value > 255)
            return bsd_fail(base, AMI_EINVAL);

        sock->as_Ttl = (value < 0) ? (LONG)NX_IP_TIME_TO_LIVE : value;

        return 0;
    }

    if (bsd_tclass_option(optname))
    {
        /*
         * RFC 2474 renamed the IPv4 TOS octet and the IPv6 traffic class
         * octet to the same DS field, and NetX Duo's raw send takes one tos
         * argument for both, so IP_TOS and IPV6_TCLASS are the same setting
         * here.  -1 means "use the default", per RFC 3542.
         */
        if (value < -1 || value > 255)
            return bsd_fail(base, AMI_EINVAL);

        sock->as_Tos = (value < 0) ? 0 : value;

        return 0;
    }

    return bsd_fail(base, AMI_ENOPROTOOPT);
}

LONG bsd_getsockopt_ipv6(struct AmiSocketBase *base, AmiSocket *sock,
                         LONG level, LONG optname, APTR optval,
                         socklen_t *optlen)
{
    LONG value;
    LONG owned;

    if ((sock->as_Flags & ASF_INET6) == 0)
        return bsd_fail(base, AMI_ENOPROTOOPT);

    owned = bsd_cmsg_option(base, sock, level, optname, optval, optlen, FALSE);
    if (owned <= 0)
        return owned;

    if (optval == NULL || optlen == NULL)
        return bsd_fail(base, AMI_EFAULT);

    if (bsd_v6only_option(optname))
        value = ((sock->as_Flags & ASF_V6ONLY) != 0) ? 1 : 0;
    else if (bsd_hops_option(optname))
        value = sock->as_Ttl;
    else if (bsd_tclass_option(optname))
        value = sock->as_Tos;
    else
        return bsd_fail(base, AMI_ENOPROTOOPT);

    if (*optlen >= (socklen_t)sizeof(LONG))
    {
        *(LONG *)optval = value;
        *optlen = (socklen_t)sizeof(LONG);
    }
    else if (*optlen >= (socklen_t)sizeof(WORD))
    {
        *(WORD *)optval = (WORD)value;
        *optlen = (socklen_t)sizeof(WORD);
    }
    else
    {
        return bsd_fail(base, AMI_EINVAL);
    }

    return 0;
}
