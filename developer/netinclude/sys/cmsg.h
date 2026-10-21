/* Ancillary layout shared by the SDK and its compatibility include.
 * Four-byte record rounding is part of the m68k socket ABI.
 * SPDX-License-Identifier: MIT */
#ifndef _SYS_CMSG_H
#define _SYS_CMSG_H
#include <sys/socket.h>
#undef  CMSG_ALIGN
#undef  CMSG_DATA
#undef  CMSG_FIRSTHDR
#undef  CMSG_NXTHDR
#undef  CMSG_LEN
#undef  CMSG_SPACE

#define CMSG_ALIGN(n)       ((((socklen_t)(n)) + 3UL) & ~3UL)

/* Data start, header size and total footprint of one object. */
#define CMSG_DATA(cmsg) \
    ((unsigned char *)(cmsg) + CMSG_ALIGN(sizeof(struct cmsghdr)))
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
     : ((((unsigned char *)(cmsg) + CMSG_ALIGN(((struct cmsghdr *)(cmsg))->cmsg_len) + \
          sizeof(struct cmsghdr)) > \
         ((unsigned char *)(mhdr)->msg_control + (mhdr)->msg_controllen)) \
        ? (struct cmsghdr *)NULL \
        : (struct cmsghdr *)((unsigned char *)(cmsg) + \
              CMSG_ALIGN(((struct cmsghdr *)(cmsg))->cmsg_len))))


#endif
