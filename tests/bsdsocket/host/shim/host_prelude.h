/*
 * Force-included ahead of everything in a bsdsocket host test.  Ordering is
 * the content: libc networking headers FIRST, then htonl-family macros
 * undefined, then __asm neutered, all before src/bsdsocket sees a header.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AMINETXDUO_BSD_TEST_HOST_PRELUDE_H
#define AMINETXDUO_BSD_TEST_HOST_PRELUDE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>

#undef htonl
#undef htons
#undef ntohl
#undef ntohs

/*
 * glibc names these with different values (IPV6_PKTINFO is 46 on the Amiga,
 * 50 on Linux); the Amiga's must win, and redefining without #undef is an
 * error under -Werror.
 */
#undef IPV6_RECVPKTINFO
#undef IPV6_PKTINFO
#undef IPV6_RECVHOPLIMIT
#undef IPV6_HOPLIMIT
#undef IPV6_HOPOPTS
#undef IPV6_DSTOPTS
#undef IPV6_RTHDR
#undef IPV6_TCLASS
#undef IPV6_RECVTCLASS
#undef IPV6_UNICAST_HOPS
#undef IPV6_MULTICAST_HOPS
#undef IPV6_MULTICAST_IF
#undef IPV6_MULTICAST_LOOP
#undef IPV6_JOIN_GROUP
#undef IPV6_LEAVE_GROUP
#undef IPV6_V6ONLY
#undef IPV6_CHECKSUM
#undef ICMP6_FILTER
#undef IP_PKTINFO
#undef IP_RECVDSTADDR

/* glibc's TCP_USER_TIMEOUT is 18; aminetxduo/tcp.h publishes 0x1001. */
#undef TCP_USER_TIMEOUT

/* AmiTCP-only SOL_SOCKET option; NDK 3.2 netinclude/sys/socket.h:152. */
#define SO_EVENTMASK 0x2001

#define AMINETXDUO_HAVE_IPV6_MREQ        1
#define AMINETXDUO_HAVE_SOCKADDR_STORAGE 1
#define AMINETXDUO_HAVE_IN_PKTINFO       1

/* macOS publishes this tag unconditionally; rename cmsg.h's Amiga-shaped tag
 * after the host headers are parsed, as with timeval below. */
#if defined(__APPLE__)
#define in6_pktinfo ami_in6_pktinfo
#endif

/* Named in bsdsocket_vectors.h prototypes; declared here so the prototype
 * does not introduce a type scoped to itself (-Werror). */
struct rt_msghdr;
struct AddressAllocationMessage;

/*
 * Amiga timeval is {ULONG tv_secs; ULONG tv_micro;}, POSIX's is a different
 * shape under the same tag.  Renaming from here down gives the tree's code the
 * Amiga shape while everything libc already declared keeps the POSIX one.
 */
#define timeval ami_timeval
struct ami_timeval {
    unsigned int tv_secs;
    unsigned int tv_micro;
};

#define __asm(x)

/* library.c opens with a file-scope m68k asm() block no host assembler can
 * take.  Defined last so nothing in this file is affected by it. */
#define asm(x)

#endif
