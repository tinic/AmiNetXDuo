/*
 * AmiNetXDuo, crypto68k: ChaCha20, and the RFC 8439 AEAD built from it and
 * Poly1305.  This is the TLS record path for ciphersuites 0xCCA8 and 0xCCA9.
 *
 * Constant time, at no cost: add, rotate and exclusive-or on data, no table
 * and no branch on a secret.
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
    UINT    c68k_chacha20_used;         /* bytes of `stream` already spent. */
                                        /* BLOCK_SIZE means none is held    */
} C68K_CHACHA20;

/*
 * `key` is 32 bytes, `nonce` 12, `counter` the initial block counter, 0 for
 * RFC 8439's own test vectors and for the Poly1305 key derivation, 1 for the
 * first block of AEAD payload.
 */
VOID c68k_chacha20_initialize(C68K_CHACHA20 *ctx, const UCHAR *key,
                              const UCHAR *nonce, ULONG counter);

/*
 * out = in XOR keystream, `length` bytes, continued from the point the last
 * call reached.  `in` and `out` can be the same pointer.  Neither needs any
 * alignment: the 68020 does misaligned longword accesses in hardware, and the
 * payload of a TLS record starts 21 bytes into the packet buffer.
 */
VOID c68k_chacha20_xor(C68K_CHACHA20 *ctx, const UCHAR *in, UCHAR *out,
                       ULONG length);

/* The keystream on its own, for deriving a one-time Poly1305 key. */
VOID c68k_chacha20_keystream(C68K_CHACHA20 *ctx, UCHAR *out, ULONG length);


/*
 * The block function, RFC 8439 section 2.3: out[i] = round20(in)[i] + in[i],
 * sixteen words each.  `in` and `out` can be the same array.
 *
 * Exposed so it can be checked against c68k_chacha20.S, which implements the
 * same interface in 68020 assembly.  The calls above take whichever this build
 * has, which c68k_chacha20_core_is_asm() reports.  The C stays compiled either
 * way, so a test can run both and compare.
 * tests/crypto68k/crypto68k_bulk does.
 */
VOID c68k_chacha20_core_c(const ULONG *in, ULONG *out);

/* NX_CRYPTO_TRUE when this build's block function is the assembly. */
UINT c68k_chacha20_core_is_asm(VOID);


/* ------------------------------------------------------------- the AEAD, */

/*
 * RFC 8439 section 2.8 AEAD, streamed the way nx_secure drives a session
 * cipher: one INITIALIZE, some number of UPDATEs, one CALCULATE.
 *
 * Associated data must not be added after the first payload byte, because the
 * padding between the two halves is written at that transition.
 *
 * Decryption does not compare the tag.  The caller must.
 * c68k_chacha20_poly1305_verify() below is the comparison, constant-time.
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
