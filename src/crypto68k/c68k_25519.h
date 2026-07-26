/*
 * AmiNetXDuo -- crypto68k: X25519 and Ed25519 for a 32-bit machine with a
 * 32x32->64 multiplier.
 *
 * WHY THIS EXISTS
 *
 *   docs/RESEARCH.md 31.6 recorded that src/crypto68k/ accelerated RSA, P-256,
 *   SHA-256 and AES, and that the suite a modern OpenSSH negotiates --
 *   curve25519, ed25519, chacha20-poly1305 -- used none of it.  32 measured
 *   where the 84 seconds went and the answer was entirely here.
 *
 *   Dropbear's 25519 is TweetNaCl, which is the smallest correct
 *   implementation in existence and was never meant to be the fastest.  Its
 *   field element is sixteen 16-bit limbs stored in an `i64[16]`, so one field
 *   multiply is 256 SOFTWARE 64x64 multiplies on a part that has a 32x32->64
 *   in hardware.  This file is the same mathematics over eight 32-bit limbs,
 *   which is the shape src/crypto68k/c68k_p256.c already uses for P-256.
 *
 * WHAT IT IS NOT
 *
 *   It is not a wrapper around a reference implementation and it is not
 *   assembly.  §18 established that this toolchain's GCC leaves nothing on the
 *   table for SHA-256 on this part, and the same argument applies to a limb
 *   loop whose whole content is MULU.L and ADDX: the win here is the
 *   REPRESENTATION, not the instruction selection.  Assembly is a separate
 *   question and should be asked only against a measurement of this.
 *
 * THE SHA-512 IS THE CALLER'S
 *
 *   Ed25519 is defined over SHA-512 and this file does not contain one.  That
 *   is deliberate: every program that wants Ed25519 here already has a
 *   SHA-512 -- Dropbear has libtomcrypt's, a TLS build has nx_crypto's -- and
 *   a second copy would be a second thing to keep right.  The callback takes
 *   up to three chunks because that is exactly what RFC 8032 hashes
 *   (prefix||M, and R||A||M); a NULL chunk with length 0 is skipped.
 *
 * CONSTANT TIME, AND THE HONEST LIMIT OF IT
 *
 *   The scalar multiplications are ladder/always-add shaped and the
 *   conditional moves are mask-based, so the secret scalar does not steer a
 *   branch or an address.  What is NOT claimed is immunity to a
 *   microarchitectural attacker: this is a 68020 in a machine with no memory
 *   protection, where any process can read any other's memory outright.  The
 *   constant-time shape is kept because it costs nothing here, not because it
 *   is load-bearing.
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
 * clamped internally, so a caller may pass a raw random 32 bytes.  Returns
 * 0 on success and -1 if the result is the all-zero point, which RFC 7748
 * section 6.1 says a key exchange must treat as a failure.
 */
int c68k_x25519(unsigned char q[32], const unsigned char n[32],
                const unsigned char p[32]);

/* q = n * basepoint, i.e. the public half of an X25519 key pair. */
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
