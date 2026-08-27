/*
 * tls.library, bsdsocket.library through its PUBLISHED vectors.
 *
 * Every AmigaOS TCP/IP stack implements this table at these offsets -- AmiTCP
 * 4.3 defined it, Roadshow, Miami, MiamiDx, AmiTCP-Genesis and ours all follow
 * it -- so a tls.library that calls nothing else runs on all of them.  That is
 * the whole point of this file: the private context LVO it replaces existed
 * only in our own bsdsocket.library, and made tls.library useless anywhere
 * else.
 *
 * src/tools/toolsock.c calls the same vectors for the command line tools; the
 * two are separate because that one links the tool runtime and prints, and
 * this one runs inside a shared library.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_TLSLIB_SOCK_H
#define AMINETXDUO_TLSLIB_SOCK_H

#include <exec/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The vectors used here, as the positive magnitude of the negative offset.
   Nothing beyond -0x102 is touched: everything below is in every stack. */
#define TLS_SOCK_LVO_SEND           0x042
#define TLS_SOCK_LVO_RECV           0x04E
#define TLS_SOCK_LVO_GETSOCKOPT     0x060
#define TLS_SOCK_LVO_GETPEERNAME    0x06C
#define TLS_SOCK_LVO_WAITSELECT     0x07E
#define TLS_SOCK_LVO_ERRNO          0x0A2

/* The last one this library needs, so a caller can check a base rather than
   trust it.  Errno() at -0x0a2 is older than any stack still in use. */
#define TLS_SOCK_LVO_LAST           TLS_SOCK_LVO_ERRNO

/* bsdsocket.library reports BSD errno values.  Only these three are acted
   on; everything else is one failure. */
#define TLS_SOCK_EINTR              4
#define TLS_SOCK_EWOULDBLOCK        35
#define TLS_SOCK_EAGAIN             TLS_SOCK_EWOULDBLOCK

/* SOL_SOCKET / SO_TYPE / SOCK_STREAM, 4.2BSD numbers, unchanged everywhere. */
#define TLS_SOCK_SOL_SOCKET         0xFFFF
#define TLS_SOCK_SO_TYPE            0x1008
#define TLS_SOCK_SOCK_STREAM        1

#define TLS_SOCK_AF_INET            2
#define TLS_SOCK_AF_INET6           23

/* WaitSelect()'s struct timeval.  Two LONGs, and nothing else has ever been
   passed through this vector. */
typedef struct TLSSockTimeval
{
    LONG    tv_secs;
    LONG    tv_micro;
} TLSSockTimeval;

/*
 * The largest descriptor these calls will build an fd_set for.  It matches
 * the largest table bsdsocket.library's SBTC_DTABLESIZE accepts, and
 * tls_conn.c's TLS_FD_MAX for the same reason.
 */
#define TLS_SOCK_FD_MAX             1024

LONG tls_sock_send(APTR base, LONG fd, const void *buf, LONG len);
LONG tls_sock_recv(APTR base, LONG fd, void *buf, LONG len);
LONG tls_sock_errno(APTR base);

/*
 * getsockopt() and getpeername(), the two questions that separate a connected
 * TCP descriptor from anything else the caller may have handed us.  Both
 * return 0 or -1 and write through the length in place, as the ABI says.
 */
LONG tls_sock_getsockopt(APTR base, LONG fd, LONG level, LONG name,
                         void *val, LONG *len);
LONG tls_sock_getpeername(APTR base, LONG fd, void *sa, LONG *len);

/*
 * Wait for one descriptor.  `tv` NULL blocks forever.  1 when it is ready, 0
 * on timeout, -1 on error.  Descriptors at or above TLS_SOCK_FD_MAX answer -1
 * rather than writing past the set.
 */
LONG tls_sock_wait(APTR base, LONG fd, BOOL write, const TLSSockTimeval *tv);

/*
 * TRUE when the library's jump table actually reaches `lvo`, given as the
 * positive magnitude.  A library that stops short has (APTR)-1 there and
 * jumping to it is a guru, so this is asked once at TLSOpen().
 */
BOOL tls_sock_have_lvo(APTR base, ULONG lvo);

/*
 * TRUE when `fd` is a connected stream socket on `base`.  `port` and `family`
 * are the peer's, from getpeername(), and may be NULL.  This is what replaces
 * the private context's descriptor lookup.
 */
BOOL tls_sock_is_connected_tcp(APTR base, LONG fd, UWORD *port, UWORD *family);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_TLSLIB_SOCK_H */
