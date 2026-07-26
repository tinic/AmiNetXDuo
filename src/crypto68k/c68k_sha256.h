/*
 * AmiNetXDuo -- crypto68k: SHA-256, for the TLS record path and everything
 * else in the handshake that hashes.
 *
 * WHY THIS EXISTS
 *
 *   docs/RESEARCH.md 15.7 ends with the plainest sentence in that section:
 *   "the largest single lever available to https:// on a classic Amiga is
 *   still an unwritten 68020 SHA-256."  This is it.  We were already 1.28x
 *   AmiSSL on HMAC-SHA256 and both sides were portable C, so the 1.28x was a
 *   statement about two C implementations and not about the machine.
 *
 * WHY SHA-256 IS THE BETTER TARGET OF THE TWO
 *
 *   AES on this machine is 160 table reads a block against roughly 4,700
 *   cycles, so a quarter of it is bus and three quarters instruction issue.
 *   SHA-256 touches memory only for the message schedule and is otherwise
 *   pure ALU -- and the ALU work is rotates, which is exactly what the 68020
 *   is good at and what most 32-bit architectures have to synthesise from two
 *   shifts and an OR.  ROR.L and ROL.L take an immediate count of 1 to 8;
 *   SWAP is a 16-bit rotate for 4 cycles; every rotation SHA-256 asks for is
 *   one or two of those.  The compiler does not know that -- for a count
 *   above 8 it loads the count into a data register first, on a machine that
 *   has eight of them and eight live state variables.
 *
 *   ROTR(x,13) = SWAP then ROL.L #3.  ROTR(x,22) = SWAP then ROR.L #6.
 *   ROTR(x,17) = SWAP then ROR.L #1.  ROTR(x,25) = SWAP then ROL.L #7.
 *   That is the whole argument for writing this in assembly.
 *
 * THE INTERFACE IS nx_crypto's, DELIBERATELY
 *
 *   c68k_sha256_initialize / _update / _digest_calculate have the same
 *   signatures as _nx_crypto_sha256_initialize and friends, because
 *   _nx_crypto_hmac_metadata_set() takes those three as function pointers.
 *   So HMAC-SHA256 is nx_crypto's own framing with the hash swapped
 *   underneath it, and none of the padding, key-shortening or ipad/opad logic
 *   is reimplemented here.  See src/tls/ami_tls_crypto.c.
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
 * `algorithm` is accepted and ignored beyond a check, so that the three
 * functions are drop-in for nx_crypto's.  SHA-224 is not implemented; it is
 * not on any path this client takes.
 */
UINT c68k_sha256_initialize(C68K_SHA256 *ctx, UINT algorithm);
UINT c68k_sha256_update(C68K_SHA256 *ctx, UCHAR *input, UINT input_length);
UINT c68k_sha256_digest_calculate(C68K_SHA256 *ctx, UCHAR *digest,
                                  UINT algorithm);

/*
 * The compression function itself: `blocks` 64-byte blocks folded into the
 * eight-word state.  `data` needs no alignment -- the 68020 does misaligned
 * longword reads in hardware, and a TLS record's payload starts 21 bytes into
 * the packet buffer.
 *
 * Public because it is the thing being measured, and because measuring it
 * through the update/padding layer measures the padding layer too.
 */
VOID c68k_sha256_blocks(ULONG *state, const UCHAR *data, ULONG blocks);


/* ---------------------------------------------------------- the variants -- */

#define C68K_SHA256_V_C     0u  /* portable C, schedule computed up front  */
#define C68K_SHA256_V_ASM   1u  /* 68020 assembly                          */
#define C68K_SHA256_V_COUNT 2u

#define C68K_SHA256_V_BEST  C68K_SHA256_V_ASM

extern UINT c68k_sha256_variant;

const char *c68k_sha256_variant_name(UINT variant);
UINT c68k_sha256_variant_is_asm(UINT variant);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_C68K_SHA256_H */
