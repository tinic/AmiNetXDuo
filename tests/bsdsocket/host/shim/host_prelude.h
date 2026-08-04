/*
 * Force-included ahead of everything in a bsdsocket host test.
 *
 * Three things have to happen before src/bsdsocket sees a header, and the
 * order is the whole content of this file.
 *
 *   1. The C library's own networking headers arrive FIRST, so that when
 *      aminetxduo/in6.h and aminetxduo/cmsg.h ask whether a structure already
 *      exists, the answer is yes.  Those two headers publish what the NDK
 *      leaves out and each definition is behind an AMINETXDUO_HAVE_ guard for
 *      exactly this case; setting the guards here is what stops a redefinition
 *      of what glibc already has.
 *
 *   2. htonl and friends are macros on the Amiga and functions here, and a
 *      macro expanded inside glibc's declaration of the function is a syntax
 *      error.  Undefined after the system headers, so the host's versions win.
 *
 *   3. __asm is neutered.  Every vector in src/bsdsocket declares its
 *      arguments in the m68k register convention, which no host compiler can
 *      honour, and putting a host #ifdef into each of them would be a change
 *      to shipping code for the benefit of a test.
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
#include <netdb.h>

#undef htonl
#undef htons
#undef ntohl
#undef ntohs

/*
 * The socket options aminetxduo/cmsg.h publishes because the NDK does not.
 * glibc has all of them, with the same names and, for some, different values:
 * IPV6_PKTINFO is 46 on the Amiga and 50 on Linux.  The Amiga's numbers are
 * what the library under test answers to, so ours must win, and a macro
 * redefined without being undefined first is an error under the -Werror the
 * CI build uses.
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

#define AMINETXDUO_HAVE_IPV6_MREQ        1
#define AMINETXDUO_HAVE_SOCKADDR_STORAGE 1
#define AMINETXDUO_HAVE_IN_PKTINFO       1

/*
 * Two structures bsdsocket_vectors.h names in prototypes without defining:
 * the Roadshow routing message and the address-allocation message.  Declared
 * here so the prototype does not introduce a type scoped to itself, which
 * -Werror treats as the mistake it usually is.
 */
struct rt_msghdr;
struct AddressAllocationMessage;

/*
 * The Amiga's struct timeval, under a name of its own.
 *
 * It is {ULONG tv_secs; ULONG tv_micro;} and POSIX's is {time_t tv_sec;
 * suseconds_t tv_usec;}: the same tag, different members, different types.
 * The C library's headers are above and have already defined theirs, and
 * <netinet/in.h> needs that one, so it cannot simply be displaced.
 *
 * Renaming the tag from here down gives the tree's code the Amiga shape while
 * everything the C library declared keeps the POSIX one.  It works because
 * nothing in src/bsdsocket passes a timeval to libc: the only calls that take
 * one are timer.device's, which are the Amiga's.
 */
#define timeval ami_timeval
struct ami_timeval {
    unsigned int tv_secs;
    unsigned int tv_micro;
};

#define __asm(x)

#endif
