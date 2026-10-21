/* IPv4 ancillary addendum and optional aligned-buffer convenience macros.
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
#include <aminetxduo/icmp6.h>
/* AmiNetXDuo bsdsocket.library 4.11 or later. Identify the stack before
 * using IP_PKTINFO, or handle ENOPROTOOPT from setsockopt().
 * IPv4 ancillary extension: Darwin's allocation, outside the legacy BSD
 * options. The Linux value 8 is already IP_RETOPTS and must not be reused.
 */
#define IP_PKTINFO 26
#ifndef _STRUCT_IN_PKTINFO_DECLARED
#define _STRUCT_IN_PKTINFO_DECLARED
struct in_pktinfo {
    unsigned int ipi_ifindex;
    struct in_addr ipi_spec_dst;
    struct in_addr ipi_addr;
};
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
