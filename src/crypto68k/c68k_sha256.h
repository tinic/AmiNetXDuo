/*
 * AmiNetXDuo, crypto68k: SHA-256, for the TLS record path and everything
 * else in the handshake that hashes.
 *
 * c68k_sha256_initialize / _update / _digest_calculate must keep the
 * signatures of _nx_crypto_sha256_initialize and friends, because
 * _nx_crypto_hmac_metadata_set() takes those three as function pointers.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_C68K_SHA256_H
#define AMINETXDUO_C68K_SHA256_H

#include "nx_crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define C68K_SHA256_BLOCK_SIZE      64u
#define C68K_SHA256_DIGEST_SIZE     32u

typedef struct C68K_SHA256_STRUCT
{
    ULONG   c68k_sha256_state[8];
    ULONG   c68k_sha256_bits_hi;
    ULONG   c68k_sha256_bits_lo;
    ULONG   c68k_sha256_used;
    UCHAR   c68k_sha256_buffer[C68K_SHA256_BLOCK_SIZE];
} C68K_SHA256;

/*
 * `algorithm` is accepted and ignored beyond a check, so the three functions
 * are drop-in for the nx_crypto ones.  SHA-224 is not implemented.  It is not
 * on any path this client takes.
 */
UINT c68k_sha256_initialize(C68K_SHA256 *ctx, UINT algorithm);
UINT c68k_sha256_update(C68K_SHA256 *ctx, UCHAR *input, UINT input_length);
UINT c68k_sha256_digest_calculate(C68K_SHA256 *ctx, UCHAR *digest,
                                  UINT algorithm);

/*
 * The compression function itself: `blocks` 64-byte blocks folded into the
 * eight-word state.  `data` needs no alignment, the 68020 does misaligned
 * longword reads in hardware, and a TLS record's payload starts 21 bytes into
 * the packet buffer.
 *
 * Public because it is what gets measured.  A measurement through the
 * update/padding layer measures the padding layer too.
 */


/* ---------------------------------------------------------- the variants, */

/*
 * Only one, which is the result.  A 68020 assembly compression function was
 * written, checked against the vectors and measured against this C in the same
 * process: 67,656 us against 66,687 for 16 KiB on an aligned buffer, 67,653
 * against 70,241 on a misaligned one.  The misaligned MOVE.L was the only real
 * advantage of the assembly.  Once it moved into the C as three lines of
 * inline assembly, the C was ahead on both and the 230 lines of hand-written
 * rounds were dropped.  See docs/RESEARCH.md 18 for the instruction costs and
 * for the 68000-era SWAP idiom that the flat 68020 shifter makes pointless.
 */
#define C68K_SHA256_V_C     0u  /* portable C, schedule computed up front  */
#define C68K_SHA256_V_COUNT 1u

#define C68K_SHA256_V_BEST  C68K_SHA256_V_C

extern UINT c68k_sha256_variant;

const char *c68k_sha256_variant_name(UINT variant);
UINT c68k_sha256_variant_is_asm(UINT variant);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_C68K_SHA256_H */
