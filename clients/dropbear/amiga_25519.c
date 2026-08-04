/*
 * clients/dropbear/amiga_25519.c, give Dropbear src/crypto68k's 25519
 * instead of TweetNaCl's, without patching third_party/dropbear.
 *
 * docs/RESEARCH.md 35 profiled a connection and found the whole of it here.
 * Dropbear's curve25519.c is TweetNaCl: sixteen 16-bit limbs in an i64[16],
 * one field multiply is 256 software 64x64 multiplies, and a handshake is
 * 21,482 of them.  src/crypto68k/c68k_25519.c is the same mathematics over
 * eight 32-bit limbs, one MULU.L per partial product on a part that has one.
 *
 * -Wl,--wrap on the four functions curve25519.c exports.  Every reference from
 * ed25519.c and kex-x25519.c lands here instead; the TweetNaCl bodies are
 * still linked and never called, which makes the A/B one linker flag
 * (clients/dropbear/build.sh -S) rather than two source trees.
 *
 * __real_* is not referenced.  Naming it would keep a route back into the code
 * being replaced, and a fallback nobody exercises is a fallback nobody knows
 * is broken.
 *
 * third_party/dropbear/src/curve25519.h declares the interface, down to
 * `unsigned long` where c68k_25519.h says the same thing.  It is included
 * rather than retyped so that a change at the pinned tag is a compile error
 * here and not a silently mismatched call.
 *
 * SPDX-License-Identifier: MIT
 */

#include "includes.h"
#include "curve25519.h"
#include "dbrandom.h"

#include "c68k_25519.h"

/*
 * Ed25519's SHA-512, taken from the libtomcrypt already in this binary.
 * c68k_25519.c has no hash of its own: this program has one, a TLS build has
 * nx_crypto's, and a second copy would be a second thing to keep right.
 */
static void amiga_sha512_3(unsigned char out[64],
                           const unsigned char *a, unsigned long alen,
                           const unsigned char *b, unsigned long blen,
                           const unsigned char *c, unsigned long clen)
{
    hash_state hs;

    sha512_init(&hs);
    if (alen)
        sha512_process(&hs, a, alen);
    if (blen)
        sha512_process(&hs, b, blen);
    if (clen)
        sha512_process(&hs, c, clen);
    sha512_done(&hs, out);
}


void __wrap_dropbear_curve25519_scalarmult(unsigned char *q,
                                           const unsigned char *n,
                                           const unsigned char *p);
void __wrap_dropbear_curve25519_scalarmult(unsigned char *q,
                                           const unsigned char *n,
                                           const unsigned char *p)
{
    /*
     * The return value is dropped because Dropbear's interface has nowhere to
     * put it: kex-x25519.c's kexcurve25519_comb_key() takes no status from
     * this call.  RFC 7748 section 6.1's all-zero check is still performed by
     * c68k_x25519(), as it was in the TweetNaCl version being replaced.
     */
    (void)c68k_x25519(q, n, p);
}

void __wrap_dropbear_ed25519_make_key(unsigned char *pk, unsigned char *sk);
void __wrap_dropbear_ed25519_make_key(unsigned char *pk, unsigned char *sk)
{
    genrandom(sk, 32);
    c68k_ed25519_pubkey(amiga_sha512_3, pk, sk);
}

void __wrap_dropbear_ed25519_sign(const unsigned char *m, unsigned long mlen,
                                  unsigned char *s, unsigned long *slen,
                                  const unsigned char *sk,
                                  const unsigned char *pk);
void __wrap_dropbear_ed25519_sign(const unsigned char *m, unsigned long mlen,
                                  unsigned char *s, unsigned long *slen,
                                  const unsigned char *sk,
                                  const unsigned char *pk)
{
    *slen = 64;
    c68k_ed25519_sign(amiga_sha512_3, s, m, mlen, sk, pk);
}

int __wrap_dropbear_ed25519_verify(const unsigned char *m, unsigned long mlen,
                                   const unsigned char *s, unsigned long slen,
                                   const unsigned char *pk);
int __wrap_dropbear_ed25519_verify(const unsigned char *m, unsigned long mlen,
                                   const unsigned char *s, unsigned long slen,
                                   const unsigned char *pk)
{
    if (slen < 64)
        return -1;
    return c68k_ed25519_verify(amiga_sha512_3, m, mlen, s, pk);
}
