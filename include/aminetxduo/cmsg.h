/*
 * RFC 3542 ancillary data: CMSG_* macros, option numbers and payloads.
 * Include after <sys/socket.h>; it undefines the NDK's CMSG_* and replaces
 * them.  CMSG_ALIGN is 4 and that is ABI.  struct cmsghdr stays the NDK's.
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_CMSG_H
#define AMINETXDUO_CMSG_H

/* <sys/socket.h> uses size_t and ssize_t without declaring them, hence the
   two includes before it. */
#include <exec/types.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

/* IPPROTO_IPV6 and the sockaddr_in6 warning live there; one copy of each. */
#include "aminetxduo/in6.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------ the macros, */

#undef  CMSG_ALIGN
#undef  CMSG_DATA
#undef  CMSG_FIRSTHDR
#undef  CMSG_NXTHDR
#undef  CMSG_LEN
#undef  CMSG_SPACE

#define CMSG_ALIGN(n)       ((((socklen_t)(n)) + 3UL) & ~3UL)

/* Data start, header size and total footprint of one object. */
#define CMSG_DATA(cmsg) \
    ((UBYTE *)(cmsg) + CMSG_ALIGN(sizeof(struct cmsghdr)))
#define CMSG_LEN(n)         (CMSG_ALIGN(sizeof(struct cmsghdr)) + (socklen_t)(n))
#define CMSG_SPACE(n) \
    (CMSG_ALIGN(sizeof(struct cmsghdr)) + CMSG_ALIGN(n))

/* NULL when there is no ancillary data, which is how recvmsg() says so. */
#define CMSG_FIRSTHDR(mhdr) \
    (((mhdr)->msg_control != NULL && \
      (mhdr)->msg_controllen >= (socklen_t)sizeof(struct cmsghdr)) \
        ? (struct cmsghdr *)(mhdr)->msg_control \
        : (struct cmsghdr *)NULL)

/* CMSG_NXTHDR(mhdr, NULL) is CMSG_FIRSTHDR(mhdr), per RFC 3542 section 5.1. */
#define CMSG_NXTHDR(mhdr, cmsg) \
    (((cmsg) == NULL) ? CMSG_FIRSTHDR(mhdr) \
     : ((((UBYTE *)(cmsg) + CMSG_ALIGN(((struct cmsghdr *)(cmsg))->cmsg_len) + \
          sizeof(struct cmsghdr)) > \
         ((UBYTE *)(mhdr)->msg_control + (mhdr)->msg_controllen)) \
        ? (struct cmsghdr *)NULL \
        : (struct cmsghdr *)((UBYTE *)(cmsg) + \
              CMSG_ALIGN(((struct cmsghdr *)(cmsg))->cmsg_len))))

/* ------------------------------------------------------------ the buffer,
 *
 * cmsg_len is a socklen_t and a 68000 faults on an odd 32-bit load; `char
 * buf[CMSG_SPACE(n)]` promises only byte alignment.  m68k aligns a cmsghdr to
 * 2, so the attribute is the only way to reach 4; cmsg.c asserts it.
 */
#if defined(__GNUC__)
#define CMSG_BUFFER_ALIGN4  __attribute__((aligned(4)))
#else
#define CMSG_BUFFER_ALIGN4
#endif

#define CMSG_BUFFER(name, bytes)                                    \
    union {                                                         \
        struct cmsghdr cmsgbuf_align;                               \
        UBYTE          cmsgbuf_bytes[bytes];                        \
    } CMSG_BUFFER_ALIGN4 name

#define CMSG_BUFFER_PTR(name)   ((APTR)(name).cmsgbuf_bytes)
#define CMSG_BUFFER_LEN(name)   ((socklen_t)sizeof((name).cmsgbuf_bytes))

/* ------------------------------------------------------------ the levels, */

/* IPPROTO_IPV6 comes from in6.h above.  The NDK's list stops at IPPROTO_RAW,
   so this one is ours too, the IANA number. */
#ifndef IPPROTO_ICMPV6
#define IPPROTO_ICMPV6              58
#endif

/* ----------------------------------------------------------- the options,
 *
 * 4.4BSD/KAME numbering, the only one the library answers to at this level.
 * The Linux alternates 49-52 are NOT accepted: in BSD numbering those are
 * IPV6_HOPOPTS/DSTOPTS/RTHDR/PKTOPTIONS and would alias onto these.
 */

#define IPV6_RECVPKTINFO            36
#define IPV6_PKTINFO                46

/*
 * IPV6_HOPLIMIT is ancillary only: a LONG in a cmsg, never a setsockopt.  On
 * sendmsg it is 0..255, or -1 for "whatever IPV6_UNICAST_HOPS says"; any other
 * value is EINVAL.
 */
#define IPV6_RECVHOPLIMIT           37
#define IPV6_HOPLIMIT               47

/* Level IPPROTO_ICMPV6.  1 in every lineage. */
#define ICMP6_FILTER                1

/*
 * Separate options with different payloads, not spellings of one:
 * IP_RECVDSTADDR (7) carries a bare struct in_addr, IP_PKTINFO (8) a struct
 * in_pktinfo.  8 is IP_RETOPTS in this NDK and is taken over here.
 */
#ifndef IP_RECVDSTADDR
#define IP_RECVDSTADDR              7
#endif
#define IP_PKTINFO                  8

/* ---------------------------------------------------------- the payloads, */

/*
 * RFC 3542 6.6.  ipi6_ifindex is numbered as if_nametoindex() numbers it; on
 * send either field may be zero to leave that half to the stack.  Datagram or
 * raw sockets only: a TCP sendmsg() carrying one is EINVAL.
 */
struct in6_pktinfo
{
    struct in6_addr ipi6_addr;      /* src/dst IPv6 address                 */
    ULONG           ipi6_ifindex;   /* send/recv interface index            */
};

/*
 * Linux's shape, byte for byte.  ipi_spec_dst is the local address the
 * datagram was addressed to; ipi_addr is the header's destination, which for
 * a broadcast or multicast datagram is not the same thing.
 */
#ifndef AMINETXDUO_HAVE_IN_PKTINFO
#define AMINETXDUO_HAVE_IN_PKTINFO

struct in_pktinfo
{
    ULONG           ipi_ifindex;
    struct in_addr  ipi_spec_dst;
    struct in_addr  ipi_addr;
};

#endif

/*
 * RFC 3542 3.2.  Opaque: use the six macros.  A raw ICMPv6 socket passes every
 * type until one is installed; a setsockopt with optlen 0 puts it back.
 */
struct icmp6_filter
{
    ULONG           icmp6_filt[8];  /* 8*32 = 256 bits, one per ICMPv6 type */
};

#define ICMP6_FILTER_WILLPASS(type, filterp) \
    ((((filterp)->icmp6_filt[((type) & 0xFF) >> 5]) & \
      (1UL << ((type) & 31))) != 0)
#define ICMP6_FILTER_WILLBLOCK(type, filterp) \
    ((((filterp)->icmp6_filt[((type) & 0xFF) >> 5]) & \
      (1UL << ((type) & 31))) == 0)
#define ICMP6_FILTER_SETPASS(type, filterp) \
    ((((filterp)->icmp6_filt[((type) & 0xFF) >> 5]) |= (1UL << ((type) & 31))))
#define ICMP6_FILTER_SETBLOCK(type, filterp) \
    ((((filterp)->icmp6_filt[((type) & 0xFF) >> 5]) &= ~(1UL << ((type) & 31))))

/* Spelled out rather than memset(), so <string.h> is not dragged in. */
#define ICMP6_FILTER_SETPASSALL(filterp)                        \
    do {                                                        \
        int _i_;                                                \
        for (_i_ = 0; _i_ < 8; _i_++)                           \
            (filterp)->icmp6_filt[_i_] = 0xFFFFFFFFUL;          \
    } while (0)
#define ICMP6_FILTER_SETBLOCKALL(filterp)                       \
    do {                                                        \
        int _i_;                                                \
        for (_i_ = 0; _i_ < 8; _i_++)                           \
            (filterp)->icmp6_filt[_i_] = 0UL;                   \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_CMSG_H */
