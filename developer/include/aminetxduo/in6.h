/*
 * The AF_INET6 names the Roadshow NDK does not define.
 *
 * The NDK has struct in6_addr and struct sockaddr_in6 and stops there: no
 * IPPROTO_IPV6, no IPV6_* option, no INET6_ADDRSTRLEN, no IN6_IS_ADDR_*, no
 * IN6ADDR_*_INIT, no PF_INET6, no sockaddr_storage.  An application written
 * against it has therefore spelled the numbers out itself, which is why every
 * definition below is #ifndef-guarded and why each one has to be the value
 * everyone else uses, a caller who already has them keeps their own and must
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
 * header is the LINUX one, pasted in verbatim, sin6_family at offset 0, and
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

/*
 * Self-sufficient, as aminetxduo/cmsg.h is.  It did not used to be: everything
 * here was a macro, the IN6_IS_ADDR_* ones took a `const struct in6_addr *`
 * the includer already had, and leaving the include order alone was free.
 * `struct ipv6_mreq` below has a `struct in6_addr` member, so there is no
 * longer a version of this header that can leave it alone.  The NDK's
 * <sys/socket.h>, which <netinet/in.h> pulls in, uses size_t and ssize_t
 * without declaring them, hence the two before it.
 */
#include <exec/types.h>
#include <stddef.h>
#include <sys/types.h>
#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

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
 * BSD number is what is published, but the library accepts BOTH on
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
 * RFC 3542's ancillary-data options, IPV6_PKTINFO, IPV6_RECVPKTINFO,
 * IPV6_HOPLIMIT, IPV6_RECVHOPLIMIT, ICMP6_FILTER, and struct in6_pktinfo,
 * struct icmp6_filter and the CMSG_ macros the NDK is missing are NOT here.
 * They are in aminetxduo/cmsg.h, which includes this header for IPPROTO_IPV6.
 * The NDK does define struct cmsghdr, CMSG_DATA, CMSG_FIRSTHDR and
 * CMSG_NXTHDR in <sys/socket.h>; what it is missing is CMSG_LEN, CMSG_SPACE,
 * CMSG_ALIGN, and the ALIGN() its own CMSG_NXTHDR expands to and nothing
 * defines, so cmsg.h replaces the last two rather than adding to them.
 *
 * IPv4 multicast is not here either, and does not need to be: IP_MULTICAST_IF,
 * IP_MULTICAST_TTL, IP_MULTICAST_LOOP, IP_ADD_MEMBERSHIP, IP_DROP_MEMBERSHIP
 * and struct ip_mreq are all in the NDK's <netinet/in.h>.  The IPv6 set is
 * absent from it, so it is below.
 */

/* -------------------------------------------------------------- multicast
 *
 * RFC 3493 section 5.2.  BSD 9/10/11/12/13, Linux 17/18/19/20/21; the BSD
 * five are published and both are accepted, as above.
 *
 * Taking the BSD numbers costs the Linux names that sit on them --
 * IPV6_NEXTHOP 9, IPV6_AUTHHDR 10 and IPV6_FLOWINFO 11, none of which this
 * library offers; 12 and 13 are unassigned there.  Same trade
 * IPV6_UNICAST_HOPS made against IPV6_2292DSTOPTS.
 *
 * WHAT A JOIN DOES AND DOES NOT DO.  There is no MLD in this stack: joining a
 * group registers the 33:33:xx:xx:xx:xx address with the interface and makes
 * the stack accept datagrams sent to it, and sends no Multicast Listener
 * Report.  On a plain switch, and for the link-local groups that carry mDNS
 * (ff02::fb), LLMNR (ff02::1:3) and SSDP (ff02::c), that is the whole
 * transaction and it works.  A switch running MLD snooping with an active
 * querier will prune the group, and no router will forward a wider-scope
 * group here.  Link-local scope is what to rely on.
 */

/* BSD 9, Linux 17.  Both accepted.  Takes an interface index, the
   if_nametoindex() kind, and 0 gives the choice back to the route. */
#ifndef IPV6_MULTICAST_IF
#define IPV6_MULTICAST_IF        9
#endif

/* BSD 10, Linux 18.  Both accepted.  0..255, or -1 for the default. */
#ifndef IPV6_MULTICAST_HOPS
#define IPV6_MULTICAST_HOPS     10
#endif

/*
 * BSD 11, Linux 19.  Both accepted, and it always reads back 0: NetX Duo
 * loops back IPv4 multicast and has no IPv6 equivalent, so a sender never
 * hears its own group traffic here.  Setting it succeeds either way rather
 * than failing a request for the behaviour already in force.
 */
#ifndef IPV6_MULTICAST_LOOP
#define IPV6_MULTICAST_LOOP     11
#endif

/* BSD 12/13, Linux 20/21.  Both accepted.  Take a struct ipv6_mreq. */
#ifndef IPV6_JOIN_GROUP
#define IPV6_JOIN_GROUP         12
#endif
#ifndef IPV6_LEAVE_GROUP
#define IPV6_LEAVE_GROUP        13
#endif

/* Linux's older spelling of the same two options. */
#ifndef IPV6_ADD_MEMBERSHIP
#define IPV6_ADD_MEMBERSHIP     IPV6_JOIN_GROUP
#endif
#ifndef IPV6_DROP_MEMBERSHIP
#define IPV6_DROP_MEMBERSHIP    IPV6_LEAVE_GROUP
#endif

/*
 * 20 bytes.  ipv6mr_interface is an interface index and not an address, which
 * is the one structural difference from struct ip_mreq; 0 means "the
 * interface the route would pick", as INADDR_ANY does there.
 */
#ifndef AMINETXDUO_HAVE_IPV6_MREQ
#define AMINETXDUO_HAVE_IPV6_MREQ

struct ipv6_mreq
{
    struct in6_addr ipv6mr_multiaddr;
    ULONG           ipv6mr_interface;
};

#endif

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
 * which is what the initialisers are for.
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
 * other, the trap at the top of this file, wearing a struct member's name.
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
 * accept, a bit outside it is EAI_BADFLAGS, not a flag quietly ignored.
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
