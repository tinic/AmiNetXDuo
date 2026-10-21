/* Compatibility include and optional aligned-buffer convenience macros.
 * Standard declarations are supplied by the common SDK.
 * SPDX-License-Identifier: MIT */
#ifndef AMINETXDUO_CMSG_H
#define AMINETXDUO_CMSG_H
#include <exec/types.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/cmsg.h>
#include <aminetxduo/in6.h>
#ifndef _NETINET_ICMP6_H
#include <netinet/icmp6.h>
#endif
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


#endif
