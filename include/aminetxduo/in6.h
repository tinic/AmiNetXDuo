/*
 * The AF_INET6 names the Roadshow NDK does not define.  Its sockaddr_in6 is
 * Linux's: no sin6_len, family at offset 0 not 1.  Decide family by length --
 * 16 = sockaddr_in, 28 = sockaddr_in6 -- and never write a length byte in one.
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_IN6_H
#define AMINETXDUO_IN6_H

/* The NDK's <sys/socket.h>, which <netinet/in.h> pulls in, uses size_t and
   ssize_t without declaring them, hence the two includes before it. */
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
 * BSD numbers are published; the library accepts the Linux ones too, EXCEPT on
 * a raw socket, where Linux's 26 collides with BSD IPV6_CHECKSUM.
 */

/* BSD 27, Linux 26.  Both accepted. */
#ifndef IPV6_V6ONLY
#define IPV6_V6ONLY             27
#endif

/* BSD 4, Linux 16.  Both accepted. */
#ifndef IPV6_UNICAST_HOPS
#define IPV6_UNICAST_HOPS        4
#endif

/* BSD 61, Linux 67.  Both accepted.  Darwin's 36 is not: it names
   IPV6_RECVPKTINFO in BSD and IPV6_HDRINCL in Linux. */
#ifndef IPV6_TCLASS
#define IPV6_TCLASS             61
#endif

/* -------------------------------------------------------------- multicast
 *
 * RFC 3493 5.2.  There is no MLD here: a join registers the 33:33 address and
 * accepts datagrams, and sends no Listener Report, so a group above link-local
 * scope is accepted but receives only what reaches the link anyway.
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

/* BSD 11, Linux 19.  Both accepted, and it always reads back 0: a sender never
   hears its own IPv6 group traffic here, whatever it sets. */
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

/* 20 bytes.  ipv6mr_interface is an interface index, not an address, and 0
   means "the interface the route would pick". */
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

/* in6addr_any and in6addr_loopback are deliberately not declared: a library
   vector cannot export data.  Define your own from the initialisers above. */

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
 * 128 bytes, 4-byte aligned, and without ss_family on purpose: the family byte
 * is at offset 1 for AF_INET and offset 0 for AF_INET6, so read it from the
 * length the call returned and then from that offset.
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
 * A flag outside the NDK's AI_MASK is EAI_BADFLAGS, not ignored.  AI_ADDRCONFIG
 * is 0 because that behaviour is unconditional here; AI_V4MAPPED stays
 * undefined because no value for it would be truthful.
 */
#ifndef AI_ADDRCONFIG
#define AI_ADDRCONFIG            0
#endif

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_IN6_H */
