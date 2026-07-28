/*
 * AmiNetXDuo -- crypto68k: Poly1305, the authenticator half of RFC 8439.
 *
 *   docs/RESEARCH.md 5.5 measured AES-GCM at 344.6 ms for 1 KB against
 *   AES-CBC's 21.9, twenty times slower, because nx_crypto_gcm.c's GHASH is a
 *   bit-serial GF(2^128) multiply -- and then said what the alternative was:
 *   "nx_secure has no ChaCha20-Poly1305 (verified: no source files, no
 *   ciphersuite entries).  That is the AEAD a 68k would want."  This is the
 *   authenticator half of writing it.
 *
 *   Needed because the modern web has stopped accepting the CBC suites this
 *   client was born with.  `www.google.com` offers ChaCha20-Poly1305 and
 *   AES-GCM and nothing else, so the question is which AEAD this machine can
 *   afford.
 *
 *   2^26 limbs and not 2^32:
 *
 *   Poly1305 is arithmetic modulo 2^130 - 5, so a 32-bit machine holds the
 *   accumulator in five limbs either way.  The 2^26 layout is the standard
 *   32-bit one -- the same decomposition RFC 8439 2.5 describes and that
 *   poly1305-donna uses -- in which every partial product fits a 64-bit
 *   accumulator with room to add four more, so a block is twenty-five MULU.L
 *   and no carry chain at all.  A 2^32 layout needs twenty multiplies
 *   but pays for them in explicit carry propagation, which on this part costs
 *   more than the five multiplies it saves: an ADD.L is 2 cycles against
 *   MULU.L's 43, but the 2^32 form needs a two-word add and a compare per
 *   partial product rather than one 64-bit accumulate.
 *
 *   Twenty-five MULU.L at 43 cycles is 1,075 cycles a block, 67 a byte: the
 *   floor for this construction on a 68020, and about a third of what
 *   AES-128-CBC charges (233 cycles a byte, docs/RESEARCH.md 18.2).
 *
 * Constant time: the arithmetic is add and multiply on data with no table
 * lookup and no branch on a secret, and the final reduction's select is
 * written branch-free.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_C68K_POLY1305_H
#define AMINETXDUO_C68K_POLY1305_H

#include "nx_crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define C68K_POLY1305_KEY_SIZE      32u
#define C68K_POLY1305_TAG_SIZE      16u
#define C68K_POLY1305_BLOCK_SIZE    16u

/*
 * ULONG is exactly 32 bits here and the limb decomposition depends on it:
 * m68k's is 32, and the host tier's shim (tests/crypto68k/host/shim) spells
 * it uint32_t for this reason.  ULONG64 is the 64-bit accumulator, the same
 * type the huge-number code calls HN_UBASE2.
 */
typedef struct C68K_POLY1305_STRUCT
{
    ULONG   c68k_poly1305_r[5];         /* the clamped key, radix 2^26      */
    ULONG   c68k_poly1305_h[5];         /* the accumulator, radix 2^26      */
    ULONG   c68k_poly1305_pad[4];       /* key[16..31], added at the end    */
    UCHAR   c68k_poly1305_buffer[C68K_POLY1305_BLOCK_SIZE];
    UINT    c68k_poly1305_leftover;     /* bytes held in the buffer         */
} C68K_POLY1305;

/*
 * `key` is 32 bytes: r in the first sixteen, s in the second.  A Poly1305 key
 * is one-time -- reusing one over two messages hands over r -- so the AEAD in
 * c68k_chacha20.h derives a fresh one per record from the cipher itself, and
 * this interface takes the key rather than generating it.
 */
VOID c68k_poly1305_initialize(C68K_POLY1305 *ctx, const UCHAR *key);

VOID c68k_poly1305_update(C68K_POLY1305 *ctx, const UCHAR *input,
                          ULONG input_length);

/*
 * Writes the 16-byte tag and leaves the context zeroed: what is left behind is
 * r and the accumulator, and neither should outlive the record.
 */
VOID c68k_poly1305_finish(C68K_POLY1305 *ctx, UCHAR *tag);


/*
 * h = (h + m) * r mod 2^130 - 5 over `blocks` whole 16-byte blocks, with
 * `hibit` the 2^128 term a full block carries and a short final one does not.
 *
 * Exposed so it can be checked against c68k_poly1305.S, which implements the
 * same interface in 68020 assembly.  The three calls above take whichever this
 * build has -- ask with c68k_poly1305_blocks_is_asm() -- but the C stays
 * compiled either way, so a test can run both over the same blocks and compare
 * the accumulator.  tests/crypto68k/crypto68k_bulk does; a kernel bug that
 * only showed once the accumulator had grown would pass RFC 8439 2.5.2's
 * single vector.
 */
VOID c68k_poly1305_blocks(C68K_POLY1305 *ctx, const UCHAR *m, ULONG blocks,
                          ULONG hibit);

VOID c68k_poly1305_blocks_c(C68K_POLY1305 *ctx, const UCHAR *m, ULONG blocks,
                            ULONG hibit);

/* NX_CRYPTO_TRUE when this build's block function is the assembly. */
UINT c68k_poly1305_blocks_is_asm(VOID);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_C68K_POLY1305_H */
