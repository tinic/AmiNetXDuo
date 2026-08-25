/*
 * AmiNetXDuo, nx_crypto port header for the host build of the crypto68k
 * vector test.  LONG/ULONG must be fixed 32-bit so HN_UBASE2 is exactly twice
 * HN_UBASE, as on m68k; a 64-bit ULONG makes HN_SHIFT 64 and the huge-number
 * arithmetic wrong.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _NX_CRYPTO_PORT_H_
#define _NX_CRYPTO_PORT_H_

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef NX_CRYPTO_STANDALONE_ENABLE

#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define NX_CRYPTO_LITTLE_ENDIAN                   0
#else
#define NX_CRYPTO_LITTLE_ENDIAN                   1
#endif

#if NX_CRYPTO_LITTLE_ENDIAN
#define NX_CRYPTO_CHANGE_ULONG_ENDIAN(arg)        (arg) = __builtin_bswap32(arg)
#define NX_CRYPTO_CHANGE_USHORT_ENDIAN(arg)       (arg) = __builtin_bswap16(arg)
#else
#define NX_CRYPTO_CHANGE_ULONG_ENDIAN(a)
#define NX_CRYPTO_CHANGE_USHORT_ENDIAN(a)
#endif

#ifndef VOID
#define VOID                                      void
typedef char                                      CHAR;
typedef unsigned char                             UCHAR;
typedef int                                       INT;
typedef unsigned int                              UINT;
typedef int32_t                                   LONG;
typedef uint32_t                                  ULONG;
typedef short                                     SHORT;
typedef unsigned short                            USHORT;
#endif

typedef uint64_t                                  ULONG64;
#define ULONG64_DEFINED

#endif /* NX_CRYPTO_STANDALONE_ENABLE */

#endif /* _NX_CRYPTO_PORT_H_ */
