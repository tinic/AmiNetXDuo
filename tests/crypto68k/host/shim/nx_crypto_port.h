/*
 * AmiNetXDuo -- nx_crypto port header for the host build of the crypto68k
 * vector test.  Never compiled for the Amiga.
 *
 * NX_CRYPTO_STANDALONE_ENABLE is nx_crypto's own supported way to build the
 * crypto library without NetX Duo underneath it: nx_crypto.h then includes
 * this header instead of nx_api.h.  Using it here keeps the host test free of
 * tx_port.h, nx_port.h and everything else that only makes sense on a 68k.
 *
 * The types are fixed width rather than `unsigned long` because every
 * nx_crypto port in third_party -- and tx_port.h in ours -- spells ULONG as
 * `unsigned long`, which is 32 bits on m68k-amigaos and 64 bits on any LP64
 * host.  nx_crypto's multi-precision code is built on
 *
 *     HN_BASE == LONG, HN_UBASE == ULONG, HN_UBASE2 == ULONG64,
 *     HN_SHIFT == sizeof(HN_BASE) * 8
 *
 * and needs HN_UBASE2 to be exactly twice HN_UBASE.  With a 64-bit ULONG,
 * HN_SHIFT becomes 64, every `product >> HN_SHIFT` shifts a 64-bit value by
 * its own width, and the library computes nonsense (clang: "shift count >=
 * width of type", fourteen times in one file).  Pinning LONG/ULONG to
 * int32_t/uint32_t reproduces the m68k widths exactly, so the host run tests
 * the same arithmetic the Amiga run does.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _NX_CRYPTO_PORT_H_
#define _NX_CRYPTO_PORT_H_

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef NX_CRYPTO_STANDALONE_ENABLE

/* Only the byte-oriented primitives (hashes, block ciphers) use these; the
   huge-number code this test exercises does not.  Set from the compiler's own
   answer rather than assumed, so the file is correct on either host. */
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
