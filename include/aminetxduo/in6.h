/*
 * The AF_INET6 names the Roadshow NDK does not define.
 *
 * The NDK has struct in6_addr and struct sockaddr_in6 and stops there: no
 * IPPROTO_IPV6, no IPV6_* option, no INET6_ADDRSTRLEN, no IN6_IS_ADDR_*, no
 * IN6ADDR_*_INIT, no PF_INET6, no sockaddr_storage.  An application written
 * against it has therefore spelled the numbers out itself, which is why every
 * definition below is #ifndef-guarded and why each one has to be the value
 * everyone else uses -- a caller who already has them keeps their own and must
 * still be right.  Verified absent from the NDK include tree on 2026-07-31.
 *
 * There are no vectors here.  Everything is a macro or a type; the calls these
 * name are setsockopt(), getsockopt(), socket() and getaddrinfo(), which the
 * NDK already declares.  So there is nothing to check lib_Revision for: what a
 * caller wants to know is whether this build has IPv6 at all, and the answer
 * to that is whether socket(AF_INET6, SOCK_STREAM, 0) succeeds.
 *
 *
 * THE sockaddr_in6 TRAP.  Read this before casting anything.
 *
 * The NDK's `struct sockaddr_in` is 4.4BSD's: sin_len at offset 0, sin_family
 * at offset 1.  The `struct sockaddr_in6` immediately below it in the same
 * header is the LINUX one, pasted in verbatim -- sin6_family at offset 0, and
 * no sin6_len at all.  The two are not interchangeable through a
 * `struct sockaddr *`:
 *
 *     struct sockaddr_in6 sin6;
 *     ...
 *     ((struct sockaddr *)&sin6)->sa_family     // reads the PADDING byte
 *
 * because sa_family sits at offset 1, where sockaddr_in6 has the compiler's
 * pad in front of sin6_port.  Decide the family from the length instead: 16
 * bytes is a sockaddr_in and its family is at offset 1, 28 bytes is a
 * sockaddr_in6 and its family is at offset 0.  bsdsocket.library does exactly
 * that, and src/bsdsocket/in6.c pins every offset with _Static_assert, so the
 * layout cannot move underneath either side.
 *
 * Never write a length byte into a sockaddr_in6.  On this NDK that byte is
 * sin6_family.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_IN6_H
#define AMINETXDUO_IN6_H

#include <exec/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * IN6_IS_ADDR_* below take a `const struct in6_addr *`, and struct
 * sockaddr_in6 comes from <netinet/in.h>.  Not included from here: the NDK's
 * <sys/socket.h>, which <netinet/in.h> pulls in, uses size_t and ssize_t
 * without declaring them, so it has to follow <stddef.h> and <sys/types.h>
 * and a header that forced that order would be deciding it for its includer.
 */

/* --------------------------------------------------------------- protocol */

/* The IANA number.  The NDK's list stops at IPPROTO_RAW 255. */
#ifndef IPPROTO_IPV6
#define IPPROTO_IPV6            41
#endif

/* PF_* and AF_* are the same numbers here as everywhere since 4.2BSD. */
#ifndef PF_INET6
#define PF_INET6                AF_INET6
#endif

/* "0:0:0:0:0:ffff:255.255.255.255" plus the terminator. */
#ifndef INET6_ADDRSTRLEN
#define INET6_ADDRSTRLEN        46
#endif

/* ---------------------------------------------------------------- options
 *
 * Each of these has two numbers in the wild and the NDK picks neither.  This
 * header set is 4.4BSD everywhere except the pasted-in sockaddr_in6, so the
 * BSD number is what is published -- but the library accepts BOTH on
 * setsockopt() and getsockopt(), because a caller who spelled the numbers out
 * may have taken them from either lineage and neither is wrong here.
 *
 * Neither alternative collides with something else this library offers: 26 is
 * IPV6_CHECKSUM in BSD and 27 is IPV6_JOIN_ANYCAST in Linux, both raw-socket
 * options, and there are no raw IPv6 sockets here.
 */

/* BSD 27, Linux 26.  Both accepted. */
#ifndef IPV6_V6ONLY
#define IPV6_V6ONLY             27
#endif

/* BSD 4, Linux 16.  Both accepted. */
#ifndef IPV6_UNICAST_HOPS
#define IPV6_UNICAST_HOPS        4
#endif

/*
 * BSD 61, Linux 67.  Both accepted.  Darwin's 36 is not: Apple kept it for
 * binary compatibility, and 36 names IPV6_RECVPKTINFO in BSD and IPV6_HDRINCL
 * in Linux, so it means something else in both lineages this header set could
 * belong to.
 */
#ifndef IPV6_TCLASS
#define IPV6_TCLASS             61
#endif

/*
 * RFC 3542's ancillary-data options -- IPV6_PKTINFO, IPV6_RECVPKTINFO,
 * IPV6_HOPLIMIT, IPV6_RECVHOPLIMIT, ICMP6_FILTER -- and struct in6_pktinfo,
 * struct icmp6_filter and the CMSG_ macros the NDK is missing are NOT here.
 * They arrive as aminetxduo/cmsg.h.  Worth knowing before writing it: the NDK
 * DOES define struct cmsghdr, CMSG_DATA, CMSG_FIRSTHDR and CMSG_NXTHDR in
 * <sys/socket.h>; what it is missing is CMSG_LEN, CMSG_SPACE, CMSG_ALIGN, and
 * the ALIGN() its own CMSG_NXTHDR expands to and nothing defines.
 *
 * IPv4 multicast is not here either, and does not need to be: IP_MULTICAST_IF,
 * IP_MULTICAST_TTL, IP_MULTICAST_LOOP, IP_ADD_MEMBERSHIP, IP_DROP_MEMBERSHIP
 * and struct ip_mreq are all in the NDK's <netinet/in.h>.  IPv6 multicast
 * (IPV6_JOIN_GROUP, IPV6_LEAVE_GROUP, struct ipv6_mreq) is absent from it and
 * would belong here.
 */

/* -------------------------------------------------------------- addresses
 *
 * The NDK's struct in6_addr is `{ unsigned char s6_addr[16]; }` with no union
 * over it, so an initialiser needs two levels of braces and the tests below
 * read bytes rather than 32-bit words.  Reading words would also assume an
 * alignment a `struct sockaddr *` cast does not guarantee.
 */

#ifndef IN6ADDR_ANY_INIT
#define IN6ADDR_ANY_INIT \
    {{ 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0 }}
#endif

#ifndef IN6ADDR_LOOPBACK_INIT
#define IN6ADDR_LOOPBACK_INIT \
    {{ 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1 }}
#endif

/*
 * in6addr_any and in6addr_loopback, the two extern constants RFC 3493 also
 * names, are deliberately NOT declared: they are data, a library vector
 * cannot export data, and there is no link library in this drawer for them to
 * live in.  Define one where it is wanted --
 *
 *     static const struct in6_addr any = IN6ADDR_ANY_INIT;
 *
 * -- which is what the initialisers are for.
 */

#ifndef IN6_IS_ADDR_UNSPECIFIED
#define IN6_IS_ADDR_UNSPECIFIED(a) \
    ((a)->s6_addr[ 0] == 0 && (a)->s6_addr[ 1] == 0 && \
     (a)->s6_addr[ 2] == 0 && (a)->s6_addr[ 3] == 0 && \
     (a)->s6_addr[ 4] == 0 && (a)->s6_addr[ 5] == 0 && \
     (a)->s6_addr[ 6] == 0 && (a)->s6_addr[ 7] == 0 && \
     (a)->s6_addr[ 8] == 0 && (a)->s6_addr[ 9] == 0 && \
     (a)->s6_addr[10] == 0 && (a)->s6_addr[11] == 0 && \
     (a)->s6_addr[12] == 0 && (a)->s6_addr[13] == 0 && \
     (a)->s6_addr[14] == 0 && (a)->s6_addr[15] == 0)
#endif

#ifndef IN6_IS_ADDR_LOOPBACK
#define IN6_IS_ADDR_LOOPBACK(a) \
    ((a)->s6_addr[ 0] == 0 && (a)->s6_addr[ 1] == 0 && \
     (a)->s6_addr[ 2] == 0 && (a)->s6_addr[ 3] == 0 && \
     (a)->s6_addr[ 4] == 0 && (a)->s6_addr[ 5] == 0 && \
     (a)->s6_addr[ 6] == 0 && (a)->s6_addr[ 7] == 0 && \
     (a)->s6_addr[ 8] == 0 && (a)->s6_addr[ 9] == 0 && \
     (a)->s6_addr[10] == 0 && (a)->s6_addr[11] == 0 && \
     (a)->s6_addr[12] == 0 && (a)->s6_addr[13] == 0 && \
     (a)->s6_addr[14] == 0 && (a)->s6_addr[15] == 1)
#endif

#ifndef IN6_IS_ADDR_MULTICAST
#define IN6_IS_ADDR_MULTICAST(a)    ((a)->s6_addr[0] == 0xFF)
#endif

#ifndef IN6_IS_ADDR_LINKLOCAL
#define IN6_IS_ADDR_LINKLOCAL(a) \
    ((a)->s6_addr[0] == 0xFE && ((a)->s6_addr[1] & 0xC0) == 0x80)
#endif

#ifndef IN6_IS_ADDR_SITELOCAL
#define IN6_IS_ADDR_SITELOCAL(a) \
    ((a)->s6_addr[0] == 0xFE && ((a)->s6_addr[1] & 0xC0) == 0xC0)
#endif

#ifndef IN6_IS_ADDR_V4MAPPED
#define IN6_IS_ADDR_V4MAPPED(a) \
    ((a)->s6_addr[ 0] == 0 && (a)->s6_addr[ 1] == 0 && \
     (a)->s6_addr[ 2] == 0 && (a)->s6_addr[ 3] == 0 && \
     (a)->s6_addr[ 4] == 0 && (a)->s6_addr[ 5] == 0 && \
     (a)->s6_addr[ 6] == 0 && (a)->s6_addr[ 7] == 0 && \
     (a)->s6_addr[ 8] == 0 && (a)->s6_addr[ 9] == 0 && \
     (a)->s6_addr[10] == 0xFF && (a)->s6_addr[11] == 0xFF)
#endif

#ifndef IN6_IS_ADDR_V4COMPAT
#define IN6_IS_ADDR_V4COMPAT(a) \
    ((a)->s6_addr[ 0] == 0 && (a)->s6_addr[ 1] == 0 && \
     (a)->s6_addr[ 2] == 0 && (a)->s6_addr[ 3] == 0 && \
     (a)->s6_addr[ 4] == 0 && (a)->s6_addr[ 5] == 0 && \
     (a)->s6_addr[ 6] == 0 && (a)->s6_addr[ 7] == 0 && \
     (a)->s6_addr[ 8] == 0 && (a)->s6_addr[ 9] == 0 && \
     (a)->s6_addr[10] == 0 && (a)->s6_addr[11] == 0 && \
     !((a)->s6_addr[12] == 0 && (a)->s6_addr[13] == 0 && \
       (a)->s6_addr[14] == 0 && \
       ((a)->s6_addr[15] == 0 || (a)->s6_addr[15] == 1)))
#endif

/* Multicast scope, from the low nibble of the second byte. */
#ifndef IN6_IS_ADDR_MC_NODELOCAL
#define IN6_IS_ADDR_MC_NODELOCAL(a) \
    (IN6_IS_ADDR_MULTICAST(a) && ((a)->s6_addr[1] & 0x0F) == 0x01)
#endif
#ifndef IN6_IS_ADDR_MC_LINKLOCAL
#define IN6_IS_ADDR_MC_LINKLOCAL(a) \
    (IN6_IS_ADDR_MULTICAST(a) && ((a)->s6_addr[1] & 0x0F) == 0x02)
#endif
#ifndef IN6_IS_ADDR_MC_SITELOCAL
#define IN6_IS_ADDR_MC_SITELOCAL(a) \
    (IN6_IS_ADDR_MULTICAST(a) && ((a)->s6_addr[1] & 0x0F) == 0x05)
#endif
#ifndef IN6_IS_ADDR_MC_ORGLOCAL
#define IN6_IS_ADDR_MC_ORGLOCAL(a) \
    (IN6_IS_ADDR_MULTICAST(a) && ((a)->s6_addr[1] & 0x0F) == 0x08)
#endif
#ifndef IN6_IS_ADDR_MC_GLOBAL
#define IN6_IS_ADDR_MC_GLOBAL(a) \
    (IN6_IS_ADDR_MULTICAST(a) && ((a)->s6_addr[1] & 0x0F) == 0x0E)
#endif

/* ---------------------------------------------------------------- storage
 *
 * Somewhere to receive an address of either family from accept(),
 * getsockname(), getpeername() or recvfrom() without knowing which it will
 * be.  128 bytes and 4-byte aligned, which is BSD's size and as much
 * alignment as an m68k asks for.
 *
 * WITHOUT ss_family, on purpose.  POSIX gives sockaddr_storage a family
 * member, and on this NDK there is nowhere honest to put one: the family byte
 * is at offset 1 for AF_INET and at offset 0 for AF_INET6, so a member at
 * either offset would be right for one family and silently wrong for the
 * other -- the trap at the top of this file, wearing a struct member's name.
 * Read the family the way the library does: from the length the call returned
 * and then from that offset.
 */
#ifndef AMINETXDUO_HAVE_SOCKADDR_STORAGE
#define AMINETXDUO_HAVE_SOCKADDR_STORAGE

struct sockaddr_storage
{
    ULONG   ss_align[32];       /* 128 bytes, 4-byte aligned  */
};

#endif

/* ------------------------------------------------------------- getaddrinfo
 *
 * The NDK's netdb.h has AI_PASSIVE, AI_CANONNAME, AI_NUMERICHOST,
 * AI_NUMERICSERV and AI_EXT, and its AI_MASK is the set getaddrinfo() will
 * accept -- a bit outside it is EAI_BADFLAGS, not a flag quietly ignored.
 *
 * AI_ADDRCONFIG is 0 because the behaviour it asks for is unconditional here:
 * an AAAA lookup only happens when the stack has IPv6 running and an A lookup
 * only when it has an IPv4 address.  So a caller can pass the flag, get what
 * it means, and not be refused for setting a bit AI_MASK does not know.
 *
 * AI_V4MAPPED is NOT defined, and that is the point.  This library never
 * synthesises ::ffff:a.b.c.d from an A record, so no value would be truthful:
 * 0 would claim the behaviour is always on, and any real bit would be refused
 * with EAI_BADFLAGS.  Code that needs both families asks for AF_UNSPEC, which
 * returns the IPv6 results first.  Leaving the name undefined makes that a
 * compile error to be fixed rather than a silent difference in what gets
 * connected to.
 */
#ifndef AI_ADDRCONFIG
#define AI_ADDRCONFIG            0
#endif

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_IN6_H */
