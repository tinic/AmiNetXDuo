/*
 * The port header NetX Duo's crypto library asks for when it is built on its
 * own, which is how httpd gets SHA-1 for the WebSocket handshake.
 *
 * WHY THIS EXISTS AT ALL
 *
 *   third_party/netxduo/crypto_libraries/src/nx_crypto_sha1.c is the SHA-1 in
 *   this tree and there is no reason for a second one.  It normally reaches
 *   nx_api.h, and httpd links no part of NetX Duo -- it is a bsdsocket.library
 *   application, the same as nc and telnet -- so pulling the stack's headers
 *   into it to get one hash would be the wrong trade.
 *
 *   nx_crypto.h has the escape hatch already: NX_CRYPTO_STANDALONE_ENABLE
 *   swaps nx_api.h for this header, and this header is nothing but the eight
 *   integer types and the byte-order macros.  The vendored ports supply one
 *   each for Cortex-M and Win32 and there is none for m68k.
 *
 * WHY IT IS HERE AND NOT IN port/netxduo-amiga
 *
 *   That drawer is on the include path of the stack, the TLS library and the
 *   host fuzz drivers, all of which build nx_crypto in its ORDINARY mode where
 *   this file must not be found.  src/tools is on the path of the commands and
 *   nothing else, so a header here can only reach what asks for it.
 *
 * THE TYPES ARE NOT THE VENDOR'S
 *
 *   The Cortex ports say `typedef unsigned long ULONG`, which is 32 bits
 *   there and 64 on a 64-bit host, and SHA-1's state is five ULONGs that must
 *   be exactly 32.  `unsigned int` is 32 bits on m68k-amigaos and on every
 *   host this is tested on, so that is what these say, and the assertion below
 *   fails the build rather than the digest if it ever is not.
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
