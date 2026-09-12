/*
 * AmiNetXDuo, crypto68k: X25519 and Ed25519 for a 32-bit machine with a
 * 32x32->64 multiplier.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_C68K_25519_H
#define AMINETXDUO_C68K_25519_H

/* The field arithmetic below is declared in fixed-width words, so the header
   carries the type rather than leaving it to whoever includes it. */
#include <stdint.h>

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


/*
 * The field arithmetic, and the pure-C reference each assembly routine is
 * checked against.  c68k_25519_test.c drives both halves, so both are
 * declared here rather than left for the compiler to guess at.
 */
void c68k_25519_fe_add(uint32_t r[8], const uint32_t a[8], const uint32_t b[8]);
void c68k_25519_fe_add_ref(uint32_t r[8], const uint32_t a[8], const uint32_t b[8]);
void c68k_25519_fe_sub(uint32_t r[8], const uint32_t a[8], const uint32_t b[8]);
void c68k_25519_fe_sub_ref(uint32_t r[8], const uint32_t a[8], const uint32_t b[8]);
void c68k_25519_fe_mul(uint32_t r[8], const uint32_t a[8], const uint32_t b[8]);
void c68k_25519_fe_mul_ref(uint32_t r[8], const uint32_t a[8], const uint32_t b[8]);
int  c68k_25519_fe_mul_is_asm(void);
void c68k_25519_fe_sqr(uint32_t r[8], const uint32_t a[8]);
void c68k_25519_fe_sqr_ref(uint32_t r[8], const uint32_t a[8]);
void c68k_25519_cpu_select(unsigned int mul_ul);

#endif /* AMINETXDUO_C68K_25519_H */
