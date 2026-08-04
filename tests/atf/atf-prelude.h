/*
 * Force-included ahead of an adopted FreeBSD test (-include, see CMakeLists).
 *
 * An ATF netinet test opens with <sys/socket.h> and <netinet/in.h> and expects
 * them to be self-sufficient, which they are on FreeBSD.  The Roadshow NDK's
 * are not, and the gaps are the NDK's rather than the test's, so they are
 * closed here instead of by editing a file we do not own:
 *
 *   sys/types.h   NDK sys/socket.h declares recv/send/sendmsg/sendto as
 *                 returning ssize_t and never defines it.
 *   stdint.h      NDK netinet/in.h uses uint32_t in struct sockaddr_in6.
 *   arpa/inet.h   INADDR_LOOPBACK is defined there, not in netinet/in.h.
 *                 INADDR_ANY and INADDR_BROADCAST are in netinet/in.h, so a
 *                 file can use those two and still not build.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_ATF_PRELUDE_H
#define AMINETXDUO_ATF_PRELUDE_H

#include <sys/types.h>
#include <stdint.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#endif /* AMINETXDUO_ATF_PRELUDE_H */
