/*
 * The port header NetX Duo's crypto library asks for when it is built on its
 * own, which is how httpd gets SHA-1 for the WebSocket handshake.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _NX_CRYPTO_PORT_H_
#define _NX_CRYPTO_PORT_H_

#include <stdlib.h>
#include <string.h>

#ifdef NX_CRYPTO_STANDALONE_ENABLE

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && \
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define NX_CRYPTO_LITTLE_ENDIAN     1
#else
#define NX_CRYPTO_LITTLE_ENDIAN     0
#endif

#if NX_CRYPTO_LITTLE_ENDIAN
#define NX_CRYPTO_CHANGE_ULONG_ENDIAN(arg)      (arg) = __builtin_bswap32(arg)
#define NX_CRYPTO_CHANGE_USHORT_ENDIAN(arg)     (arg) = __builtin_bswap16(arg)
#else
#define NX_CRYPTO_CHANGE_ULONG_ENDIAN(a)
#define NX_CRYPTO_CHANGE_USHORT_ENDIAN(a)
#endif

#ifndef VOID
#define VOID                        void
typedef char                        CHAR;
typedef unsigned char               UCHAR;
typedef int                         INT;
typedef unsigned int                UINT;
typedef int                         LONG;
typedef unsigned int                ULONG;
typedef short                       SHORT;
typedef unsigned short              USHORT;
#endif

/* SHA-1's chaining state is five 32-bit words and its length counter is two.
   A wider ULONG produces a digest that is wrong on every input. */
typedef char nx_crypto_port_ulong_is_32[(sizeof(ULONG) == 4) ? 1 : -1];

#endif /* NX_CRYPTO_STANDALONE_ENABLE */

#endif /* _NX_CRYPTO_PORT_H_ */
