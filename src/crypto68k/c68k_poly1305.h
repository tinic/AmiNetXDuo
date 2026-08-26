/*
 * AmiNetXDuo, crypto68k: Poly1305, the authenticator half of RFC 8439.
 *
 * Constant time: add and multiply on data with no table lookup and no branch
 * on a secret, and the final reduction's select is written branch-free.
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
 * is one-time.  Reuse of one key over two messages reveals r, so the AEAD in
 * c68k_chacha20.h derives a fresh key per record from the cipher itself, and
 * this interface takes the key rather than generating it.
 */
VOID c68k_poly1305_initialize(C68K_POLY1305 *ctx, const UCHAR *key);

VOID c68k_poly1305_update(C68K_POLY1305 *ctx, const UCHAR *input,
                          ULONG input_length);

/*
 * Writes the 16-byte tag and leaves the context zeroed.  The context holds r
 * and the accumulator.  Neither is allowed to outlive the record.
 */
VOID c68k_poly1305_finish(C68K_POLY1305 *ctx, UCHAR *tag);


/*
 * h = (h + m) * r mod 2^130 - 5 over `blocks` whole 16-byte blocks, with
 * `hibit` the 2^128 term a full block carries and a short final one does not.
 *
 * Exposed so it can be checked against c68k_poly1305.S, which implements the
 * same interface in 68020 assembly.  The three calls above take whichever this
 * build has, which c68k_poly1305_blocks_is_asm() reports.  The C stays
 * compiled either way, so a test can run both over the same blocks and compare
 * the accumulator.  tests/crypto68k/crypto68k_bulk does that.  A kernel bug
 * that appears only after the accumulator grows passes the single vector in
 * RFC 8439 2.5.2.
 */
VOID c68k_poly1305_blocks(C68K_POLY1305 *ctx, const UCHAR *m, ULONG blocks,
                          ULONG hibit);

VOID c68k_poly1305_blocks_c(C68K_POLY1305 *ctx, const UCHAR *m, ULONG blocks,
                            ULONG hibit);

/* NX_CRYPTO_TRUE when the block function of this build is the assembly.  In an
   AMINETXDUO_CPU=any build that is a run-time answer: c68k_cpu_select() points
   the vector below at one of the two above. */
UINT c68k_poly1305_blocks_is_asm(VOID);

#ifdef C68K_MV
extern VOID (*c68k_vec_poly1305_blocks)(C68K_POLY1305 *, const UCHAR *, ULONG,
                                        ULONG);
#endif

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_C68K_POLY1305_H */
