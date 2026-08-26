/*
 * AmiNetXDuo, crypto68k: X25519 and Ed25519 for a 32-bit machine with a
 * 32x32->64 multiplier.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_C68K_25519_H
#define AMINETXDUO_C68K_25519_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * RFC 7748 X25519: q = n * p, every value 32 bytes little-endian.  `n` is
 * clamped internally, so a caller can pass a raw random 32 bytes.  Returns
 * 0 on success and -1 if the result is the all-zero point, which RFC 7748
 * section 6.1 says a key exchange must treat as a failure.
 */
int c68k_x25519(unsigned char q[32], const unsigned char n[32],
                const unsigned char p[32]);

/* q = n * basepoint, that is the public half of an X25519 key pair. */
int c68k_x25519_base(unsigned char q[32], const unsigned char n[32]);

/*
 * SHA-512 over up to three chunks, concatenated.  A chunk with len 0 is
 * skipped and its pointer is never read, so a two-chunk hash passes NULL, 0.
 */
typedef void (*c68k_sha512_fn)(unsigned char out[64],
                               const unsigned char *a, unsigned long alen,
                               const unsigned char *b, unsigned long blen,
                               const unsigned char *c, unsigned long clen);

/* RFC 8032 Ed25519.  `sk` is the 32-byte seed, `pk` its public key. */
void c68k_ed25519_pubkey(c68k_sha512_fn sha512, unsigned char pk[32],
                         const unsigned char sk[32]);

void c68k_ed25519_sign(c68k_sha512_fn sha512, unsigned char sig[64],
                       const unsigned char *m, unsigned long mlen,
                       const unsigned char sk[32], const unsigned char pk[32]);

/* 0 if the signature is good, -1 otherwise. */
int c68k_ed25519_verify(c68k_sha512_fn sha512,
                        const unsigned char *m, unsigned long mlen,
                        const unsigned char sig[64],
                        const unsigned char pk[32]);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_C68K_25519_H */
