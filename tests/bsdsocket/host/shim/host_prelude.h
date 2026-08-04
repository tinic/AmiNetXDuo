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

#define AMINETXDUO_HAVE_IPV6_MREQ        1
#define AMINETXDUO_HAVE_SOCKADDR_STORAGE 1
#define AMINETXDUO_HAVE_IN_PKTINFO       1

#define __asm(x)

#endif
