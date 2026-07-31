/*
 * RFC 3542 -- ancillary data, and the options that ride it.
 *
 * sendmsg()/recvmsg() already carry msg_control, and this is what goes in it.
 * No new LVOs: everything here is a socket option number, a struct shape or a
 * macro, and the calls that move it are the ones already in the table.
 *
 * WHAT THE NDK ALREADY HAS, which is more than it looks like from a grep --
 * ndk-include is Latin-1, so `grep -r` reads it as binary and finds nothing.
 * `LC_ALL=C grep -a`.
 *
 * <sys/socket.h> defines `struct cmsghdr` (socklen_t + two __LONGs, 12 bytes),
 * CMSG_DATA, CMSG_FIRSTHDR and CMSG_NXTHDR, and <netinet/in.h> defines
 * IP_RECVDSTADDR as 7.  Two of those three macros are unusable as shipped:
 *
 *   - CMSG_NXTHDR expands to ALIGN(), which no NDK header defines, so any
 *     translation unit that uses it fails to compile ("implicit declaration of
 *     function 'ALIGN'");
 *   - CMSG_FIRSTHDR returns msg_control without testing msg_controllen, which
 *     RFC 3542 section 20.3.1 calls out by name -- recvmsg() reports "no
 *     ancillary data" by setting msg_controllen to 0, and the unguarded macro
 *     turns that into a pointer at an uninitialised buffer.
 *
 * So both are replaced here, along with CMSG_LEN and CMSG_SPACE, which the NDK
 * does not have at all.  Including this header after <sys/socket.h> is what a
 * caller wants; it undefines what it replaces.
 *
 * Replaced rather than repaired by defining ALIGN: that name is unprefixed and
 * common enough to collide with the caller's own code, and supplying it would
 * still leave CMSG_NXTHDR refusing the NULL second argument RFC 3542 5.1
 * requires.  CMSG_FIRSTHDR has to be replaced whatever happens, so one
 * mechanism for both is the smaller surface.
 *
 * `struct cmsghdr` itself is NOT redefined here.  It is the NDK's, it is
 * already the right shape, and a second definition would be an ODR-style trap
 * for anyone who included the two headers in the other order.  cmsg.c pins
 * every offset of it with _Static_assert instead.
 *
 * CMSG_ALIGN IS 4 BYTES, and that is ABI.  Every 32-bit BSD used 4, it keeps
 * struct cmsghdr at 12 bytes with no padding before the data, and nothing
 * about m68k argues for more -- wider alignment would only waste buffer.  A
 * caller that sizes its buffer with CMSG_SPACE() never has to know.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_CMSG_H
#define AMINETXDUO_CMSG_H

/*
 * Self-sufficient, unlike aminetxduo/in6.h next door, and for a reason it does
 * not have: everything here is built out of `struct cmsghdr`, `struct in6_addr`
 * and `struct in_addr`, so there is no version of this header that leaves the
 * include order to its includer.  <sys/socket.h> uses size_t and ssize_t
 * without declaring them, hence the two before it.
 */
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

/* ------------------------------------------------------------ the macros -- */

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

/* ------------------------------------------------------------ the levels -- */

/* IPPROTO_IPV6 comes from in6.h above.  The NDK's list stops at IPPROTO_RAW,
   so this one is ours too -- the IANA number. */
#ifndef IPPROTO_ICMPV6
#define IPPROTO_ICMPV6              58
#endif

/* ----------------------------------------------------------- the options --
 *
 * Two numberings exist for the IPv6 options and this NDK picks neither, so
 * both are accepted -- the same decision IPV6_V6ONLY needed, for the same
 * reason.  The unsuffixed name is the 4.4BSD/KAME number, which is the
 * lineage the rest of this header set belongs to; the _LINUX alternates are
 * here because the library answers to them, not because a caller should
 * spell them.
 *
 * Which numbering a caller enables an option with decides which numbering it
 * gets back as cmsg_type.  Enable with IPV6_RECVPKTINFO (36) and the arriving
 * cmsg_type is IPV6_PKTINFO (46); enable with 49 and it is 50.  Mixing the
 * two on one socket is a caller error and reads as "whichever was set last".
 */

#define IPV6_RECVPKTINFO            36
#define IPV6_RECVPKTINFO_LINUX      49
#define IPV6_PKTINFO                46
#define IPV6_PKTINFO_LINUX          50

#define IPV6_RECVHOPLIMIT           37
#define IPV6_RECVHOPLIMIT_LINUX     51
#define IPV6_HOPLIMIT               47
#define IPV6_HOPLIMIT_LINUX         52

/* Level IPPROTO_ICMPV6.  1 in every lineage. */
#define ICMP6_FILTER                1

/*
 * The IPv4 half.  IP_RECVDSTADDR is the NDK's own 7 and carries a bare
 * struct in_addr; IP_PKTINFO is Linux's 8 and carries a struct in_pktinfo.
 * They are separate options, not spellings of one, because the payloads
 * differ -- a socket may have either, both or neither.
 *
 * 8 is IP_RETOPTS in this NDK.  That is a 4.3BSD get/set of the IP options a
 * datagram arrived with; it is refused here and always has been, and no
 * AmigaOS stack ever answered it.  Taking the number is the same trade
 * IPV6_TCLASS made against IPV6_PATHMTU: an option this library does not
 * offer loses to one it does.
 */
#ifndef IP_RECVDSTADDR
#define IP_RECVDSTADDR              7
#endif
#define IP_PKTINFO                  8

/* ---------------------------------------------------------- the payloads -- */

/*
 * RFC 3542 section 6.6.  On receive, ipi6_addr is the destination address out
 * of the arriving header -- which may be a multicast address, not one of this
 * machine's -- and ipi6_ifindex is the arrival interface, numbered as
 * if_nametoindex() numbers it.  On send, ipi6_addr picks the source address
 * and ipi6_ifindex the outgoing interface; either may be zero to leave that
 * half to the stack.
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
struct in_pktinfo
{
    ULONG           ipi_ifindex;
    struct in_addr  ipi_spec_dst;
    struct in_addr  ipi_addr;
};

/*
 * RFC 3542 section 3.2.  Opaque to a caller: allocate one, hand it to
 * setsockopt(IPPROTO_ICMPV6, ICMP6_FILTER, ...), and use the six macros.
 * A raw ICMPv6 socket passes every type until one is installed, and a
 * setsockopt with optlen 0 puts it back that way.
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
