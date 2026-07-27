/*
 * AmiNetXDuo -- crypto68k: ChaCha20, and the RFC 8439 AEAD built from it and
 * Poly1305.  This is the TLS record path for ciphersuites 0xCCA8 and 0xCCA9.
 *
 * WHY THIS EXISTS
 *
 *   Reach, first.  This client offered two ciphersuites, `0xC027` and
 *   `0xC023`, and both are AES-128-CBC with HMAC-SHA256.  Google's front end
 *   answers a ClientHello carrying only those with a handshake_failure alert
 *   in a second and a half, and the share of the web that does the same only
 *   grows.  An AEAD suite is the difference between a stack that reaches
 *   modern sites and one that does not.
 *
 *   Which AEAD, second, and that is a question about this machine rather than
 *   about the literature.  AES-GCM's GHASH is a carry-less multiply in
 *   GF(2^128) which no 68k instruction does, so nx_crypto does it bit by bit
 *   and charges 344.6 ms for 1 KB against AES-CBC's 21.9 (docs/RESEARCH.md
 *   5.5) -- twenty times slower than the cipher it is meant to replace, which
 *   would make `https` unusable rather than merely slow.  ChaCha20 is add,
 *   rotate and exclusive-or on 32-bit words, which is the only thing a 68020
 *   is good at, and Poly1305 is 130-bit adds and a 32x32 -> 64 multiply, which
 *   is the one wide instruction the part does have.  The measured comparison
 *   is in docs/RESEARCH.md 54 and tests/crypto68k/crypto68k_bulk prints it.
 *
 * THE COST, PRICED FROM docs/RESEARCH.md 18.1 BEFORE IT WAS WRITTEN
 *
 *   Twenty rounds are eighty quarter-rounds of four ADD.L, four EOR.L and
 *   four rotates for every 64 bytes.  On the measured instruction table that
 *   is about 42 cycles of arithmetic a quarter-round with the operands in
 *   registers, and the state is sixteen words against eight data registers,
 *   so the spills roughly double it: ~120 cycles a byte, against AES-128-CBC's
 *   measured 233 and HMAC-SHA256's 236.  Poly1305 adds ~67.  So the pair
 *   should come in under half of what the CBC suite costs, and it buys reach
 *   at the same time -- which is the rare case where the compatible choice and
 *   the fast one are the same choice.
 *
 * ENDIANNESS IS THE ONE TAX
 *
 *   ChaCha20 is defined little-endian and this is a big-endian machine, so
 *   sixteen words a block have to be reversed on the way out.  That is three
 *   instructions each (ROL.W #8, SWAP, ROL.W #8, about 15.8 cycles) against
 *   the alternative of serialising the keystream to bytes and exclusive-oring
 *   byte by byte, which the instruction table prices at four times as much.
 *   The reversal is folded into the block exclusive-or below.
 *
 * CONSTANT TIME: yes, and for free -- add, rotate and exclusive-or on data,
 * no table and no branch on a secret.  Unlike c68k_aes.c, which cannot be on
 * a machine with no data cache.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_C68K_CHACHA20_H
#define AMINETXDUO_C68K_CHACHA20_H

#include "nx_crypto.h"

#include "c68k_poly1305.h"

#ifdef __cplusplus
extern "C" {
#endif

#define C68K_CHACHA20_KEY_SIZE      32u
#define C68K_CHACHA20_NONCE_SIZE    12u
#define C68K_CHACHA20_BLOCK_SIZE    64u

typedef struct C68K_CHACHA20_STRUCT
{
    ULONG   c68k_chacha20_state[16];    /* constants, key, counter, nonce   */
    UCHAR   c68k_chacha20_stream[C68K_CHACHA20_BLOCK_SIZE];
    UINT    c68k_chacha20_used;         /* bytes of `stream` already spent; */
                                        /* BLOCK_SIZE means none is held    */
} C68K_CHACHA20;

/*
 * `key` is 32 bytes, `nonce` 12, `counter` the initial block counter -- 0 for
 * RFC 8439's own test vectors and for the Poly1305 key derivation, 1 for the
 * first block of AEAD payload.
 */
VOID c68k_chacha20_initialize(C68K_CHACHA20 *ctx, const UCHAR *key,
                              const UCHAR *nonce, ULONG counter);

/*
 * out = in XOR keystream, `length` bytes, continuing from wherever the last
 * call left off.  `in` and `out` may be the same pointer.  Neither needs any
 * alignment: the 68020 does misaligned longword accesses in hardware and a
 * TLS record's payload starts 21 bytes into the packet buffer.
 */
VOID c68k_chacha20_xor(C68K_CHACHA20 *ctx, const UCHAR *in, UCHAR *out,
                       ULONG length);

/* The keystream on its own, for deriving a one-time Poly1305 key. */
VOID c68k_chacha20_keystream(C68K_CHACHA20 *ctx, UCHAR *out, ULONG length);


/* ------------------------------------------------------------- the AEAD -- */

/*
 * RFC 8439 section 2.8: ChaCha20 from block counter 1 over the payload,
 * Poly1305 over the associated data and the CIPHERTEXT with both padded to a
 * multiple of sixteen, then the two lengths, keyed by the first 32 bytes of
 * the cipher's own block 0.
 *
 * Streaming, because that is the shape nx_secure drives a session cipher in:
 * one INITIALIZE, some number of UPDATEs over a packet chain, one CALCULATE.
 *
 *   c68k_chacha20_poly1305_initialize(ctx, key, nonce)
 *   c68k_chacha20_poly1305_associate(ctx, aad, aad_length)   -- zero or more
 *   c68k_chacha20_poly1305_encrypt(ctx, in, out, n)          -- or _decrypt
 *   c68k_chacha20_poly1305_tag(ctx, tag)
 *
 * No associated data may be added once the first payload byte has gone
 * through, because the padding between the two halves is written at that
 * transition.
 *
 * DECRYPTION DOES NOT COMPARE THE TAG.  The caller does, and must, because
 * the tag arrives after the ciphertext in a TLS record and nx_secure hands it
 * over in a separate call.  c68k_chacha20_poly1305_verify() below is the
 * comparison, constant-time, for callers that have both.
 */
typedef struct C68K_CHACHA20_POLY1305_STRUCT
{
    C68K_CHACHA20   c68k_aead_cipher;
    C68K_POLY1305   c68k_aead_mac;
    ULONG           c68k_aead_aad_length;
    ULONG           c68k_aead_data_length;
    UINT            c68k_aead_data_started;
} C68K_CHACHA20_POLY1305;

VOID c68k_chacha20_poly1305_initialize(C68K_CHACHA20_POLY1305 *ctx,
                                       const UCHAR *key, const UCHAR *nonce);

VOID c68k_chacha20_poly1305_associate(C68K_CHACHA20_POLY1305 *ctx,
                                      const UCHAR *aad, ULONG aad_length);

VOID c68k_chacha20_poly1305_encrypt(C68K_CHACHA20_POLY1305 *ctx,
                                    const UCHAR *in, UCHAR *out,
                                    ULONG length);

VOID c68k_chacha20_poly1305_decrypt(C68K_CHACHA20_POLY1305 *ctx,
                                    const UCHAR *in, UCHAR *out,
                                    ULONG length);

VOID c68k_chacha20_poly1305_tag(C68K_CHACHA20_POLY1305 *ctx, UCHAR *tag);

/* NX_CRYPTO_TRUE when the 16 bytes agree.  Constant time: it accumulates the
   differences rather than returning at the first one. */
UINT c68k_chacha20_poly1305_verify(const UCHAR *a, const UCHAR *b);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_C68K_CHACHA20_H */
