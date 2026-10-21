/* IPv6 socket API additions. SPDX-License-Identifier: MIT */
#ifndef _NETINET6_IN6_H
#define _NETINET6_IN6_H
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

/* AROS option numbers, except V6ONLY: AROS currently assigns 26 to both
 * V6ONLY and CHECKSUM. Keep the BSD V6ONLY value 27 to avoid that collision.
 * Each option has one meaning on every socket type; no Linux numeric aliases.
 */
#define IPV6_V6ONLY             27
#define IPV6_UNICAST_HOPS        4
#define IPV6_MULTICAST_IF        9
#define IPV6_MULTICAST_HOPS     10
#define IPV6_MULTICAST_LOOP     11
#define IPV6_JOIN_GROUP        12
#define IPV6_LEAVE_GROUP       13
#define IPV6_CHECKSUM          26
#define IPV6_TCLASS            36
#define IPV6_RECVPKTINFO       61
#define IPV6_PKTINFO           46
#define IPV6_RECVHOPLIMIT      37
#define IPV6_HOPLIMIT          47
#define IPV6_ADD_MEMBERSHIP    IPV6_JOIN_GROUP
#define IPV6_DROP_MEMBERSHIP   IPV6_LEAVE_GROUP

#ifndef AMINETXDUO_HAVE_IPV6_MREQ
#define AMINETXDUO_HAVE_IPV6_MREQ
struct ipv6_mreq {
    struct in6_addr ipv6mr_multiaddr;
    unsigned int ipv6mr_interface;
};
#endif
struct in6_pktinfo {
    struct in6_addr ipi6_addr;
    unsigned int ipi6_ifindex;
};
#ifndef IPPROTO_ICMPV6
#define IPPROTO_ICMPV6 58
#endif

/* -------------------------------------------------------------- addresses
 *
 * The NDK's struct in6_addr has no union over s6_addr[16], so an initialiser
 * needs two brace levels and the tests must read bytes, not 32-bit words:
 * a `struct sockaddr *` cast guarantees no word alignment.
 */

#ifndef IN6ADDR_ANY_INIT
#define IN6ADDR_ANY_INIT \
    {{ 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0 }}
#endif

#ifndef IN6ADDR_LOOPBACK_INIT
#define IN6ADDR_LOOPBACK_INIT \
    {{ 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1 }}
#endif

/* Supplied by the C runtime or, when absent, SDK support/in6_addr.c.
 * These are application data, not bsdsocket.library vectors. */
extern const struct in6_addr in6addr_any;
extern const struct in6_addr in6addr_loopback;

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


#ifdef __cplusplus
}
#endif
#endif
