/*
 * bsdsocket.library, the AF_INET6 ABI, pinned.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include "aminetxduo/config.h"

_Static_assert(AF_INET6 == 23, "AF_INET6 is not the Roadshow NDK's 23");

_Static_assert(sizeof(struct in6_addr) == 16, "in6_addr is not 16 bytes");

/*
 * 28 bytes: family(1) + pad(1) + port(2) + flowinfo(4) + addr(16) + scope(4).
 * The pad at offset 1 is not a length byte, see the header comment.
 */
_Static_assert(sizeof(struct sockaddr_in6) == 28,
               "sockaddr_in6 is not the 28-byte no-sin6_len shape");
_Static_assert(offsetof(struct sockaddr_in6, sin6_family)   ==  0,
               "sin6_family moved, is this NDK's sockaddr_in6 4.4BSD after "
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

_Static_assert(sizeof(struct sockaddr_in) == 16, "sockaddr_in is not 16 bytes");
_Static_assert(offsetof(struct sockaddr_in, sin_len)    == 0, "sin_len moved");
_Static_assert(offsetof(struct sockaddr_in, sin_family) == 1, "sin_family moved");

_Static_assert(sizeof(((NXD_ADDRESS *)0)->nxd_ip_address.v6) == 16,
               "NXD_ADDRESS.v6 is not four ULONGs");

/*
 * struct in6_addr is 16 bytes in network order.  NetX Duo keeps four ULONGs in
 * host order with [0] most significant.  On m68k those are the same bytes, so
 * a memcpy works, but it breaks silently on a little-endian build.  The shifts
 * cost four instructions per word, the same trade the BSD_HTONL macros make.
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
        return TRUE;                    /* a real IPv6 address, nothing to do */

    /*
     * A V6ONLY socket must not talk to an IPv4 host at all, mapped notation or
     * not.
     */
    if ((sock->as_Flags & ASF_V6ONLY) != 0)
        return FALSE;

    /*
     * On a dual-stack socket the mapped form is a spelling of an IPv4 address,
     * so it is converted.  It cannot be left as it stands.  NetX Duo has no
     */
    bsd_addr_from_v4(addr, v4);

    return TRUE;
}

/*
 * Level IPPROTO_IPV6.
 */

/*
 * Whether the Linux numbering can be read on this socket.
 */
BOOL bsd_v6_linux_numbering(const AmiSocket *sock)
{
    return (BOOL)((sock->as_Flags & ASF_RAW) == 0);
}

static LONG bsd_v6only_option(const AmiSocket *sock, LONG optname)
{
    return (optname == AMI_IPV6_V6ONLY_BSD ||
            (optname == AMI_IPV6_V6ONLY_LINUX &&
             bsd_v6_linux_numbering(sock)));
}

static LONG bsd_hops_option(const AmiSocket *sock, LONG optname)
{
    return (optname == AMI_IPV6_UNICAST_HOPS_BSD ||
            (optname == AMI_IPV6_UNICAST_HOPS_LINUX &&
             bsd_v6_linux_numbering(sock)));
}

static LONG bsd_tclass_option(const AmiSocket *sock, LONG optname)
{
    return (optname == AMI_IPV6_TCLASS_BSD ||
            (optname == AMI_IPV6_TCLASS_LINUX &&
             bsd_v6_linux_numbering(sock)));
}

LONG bsd_setsockopt_ipv6(struct AmiSocketBase *base, AmiSocket *sock,
                         LONG level, LONG optname, APTR optval,
                         socklen_t optlen)
{
    LONG value = 0;
    LONG owned;

    if ((sock->as_Flags & ASF_INET6) == 0)
        return bsd_fail(base, AMI_ENOPROTOOPT);

    owned = bsd_cmsg_option(base, sock, level, optname, optval, &optlen, TRUE);
    if (owned <= 0)
        return owned;

#ifdef AMINETXDUO_MULTICAST
    if (level == IPPROTO_IPV6 && bsd_mcast6_is_option(sock, optname))
        return bsd_mcast6_setopt(base, sock, optname, optval, optlen);
#endif

    if (optval == NULL)
        return bsd_fail(base, AMI_EFAULT);

    if (optlen >= (socklen_t)sizeof(LONG))
        bsd_bcopy(optval, &value, sizeof(value));
    else if (optlen >= (socklen_t)sizeof(WORD))
    {
        WORD short_value;

        bsd_bcopy(optval, &short_value, sizeof(short_value));
        value = short_value;
    }
    else
        return bsd_fail(base, AMI_EINVAL);

    if (bsd_v6only_option(sock, optname))
    {
        /*
         * BSD requires this to be set before bind().  After that the socket's
         * behaviour is fixed.  Enforced rather than accepted, so an
         * application that gets the order wrong sees the same EINVAL a real
         * stack returns.
         */
        if ((sock->as_Flags & (ASF_BOUND | ASF_CONNECTED)) != 0)
            return bsd_fail(base, AMI_EINVAL);

        if (value != 0)
            sock->as_Flags |= ASF_V6ONLY;
        else
            sock->as_Flags &= ~ASF_V6ONLY;

        return 0;
    }

    if (bsd_hops_option(sock, optname))
    {
        /*
         * The IPv6 hop limit is the IPv4 TTL under another name, and NetX Duo
         * stores one per socket.  IP_TTL and IPV6_UNICAST_HOPS are therefore
         */
        if (value < -1 || value > 255)
            return bsd_fail(base, AMI_EINVAL);

        sock->as_Ttl = (value < 0) ? (LONG)NX_IP_TIME_TO_LIVE : value;

        if (bsd_nx_enter(base) != 0)
            return bsd_fail(base, AMI_ENETDOWN);
        bsd_opt_apply_ip(sock);
        bsd_nx_leave(base);

        return 0;
    }

    if (bsd_tclass_option(sock, optname))
    {
        /*
         * RFC 2474 renamed the IPv4 TOS octet and the IPv6 traffic class
         * octet to the same DS field, and NetX Duo's raw send takes one tos
         * argument for both.  IP_TOS and IPV6_TCLASS are therefore the same
         */
        if (value < -1 || value > 255)
            return bsd_fail(base, AMI_EINVAL);

        sock->as_Tos = (value < 0) ? 0 : value;

        if (bsd_nx_enter(base) != 0)
            return bsd_fail(base, AMI_ENETDOWN);
        bsd_opt_apply_ip(sock);
        bsd_nx_leave(base);

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

#ifdef AMINETXDUO_MULTICAST
    if (level == IPPROTO_IPV6 && bsd_mcast6_is_option(sock, optname))
        return bsd_mcast6_getopt(base, sock, optname, optval, optlen);
#endif

    if (optval == NULL || optlen == NULL)
        return bsd_fail(base, AMI_EFAULT);

    if (bsd_v6only_option(sock, optname))
        value = ((sock->as_Flags & ASF_V6ONLY) != 0) ? 1 : 0;
    else if (bsd_hops_option(sock, optname))
        value = sock->as_Ttl;
    else if (bsd_tclass_option(sock, optname))
        value = sock->as_Tos;
    else
        return bsd_fail(base, AMI_ENOPROTOOPT);

    if (*optlen >= (socklen_t)sizeof(LONG))
    {
        bsd_bcopy(&value, optval, sizeof(value));
        *optlen = (socklen_t)sizeof(LONG);
    }
    else if (*optlen >= (socklen_t)sizeof(WORD))
    {
        WORD short_value = (WORD)value;

        bsd_bcopy(&short_value, optval, sizeof(short_value));
        *optlen = (socklen_t)sizeof(WORD);
    }
    else
    {
        return bsd_fail(base, AMI_EINVAL);
    }

    return 0;
}
