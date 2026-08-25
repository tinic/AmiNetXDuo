/* clients/dropbear/amiga_25519.c: src/crypto68k's 25519 in place of Dropbear's
 * TweetNaCl, by -Wl,--wrap on the four functions curve25519.c exports.  Its
 * header is included so a change at the pinned tag is a compile error here.
 * SPDX-License-Identifier: MIT */

#include "includes.h"
#include "curve25519.h"
#include "dbrandom.h"

#include "c68k_25519.h"

/*
 * A C68K_MV build carries both field multiplies and something must pick before
 * the first handshake.  tls.library does it in its init; a client has no init,
 * so the four entry points below do it once between them.
 */
#ifdef C68K_MV

#include <exec/execbase.h>
#include <proto/exec.h>

extern void c68k_25519_cpu_select(unsigned int mul_ul);

static int amiga_25519_picked;

static void amiga_25519_pick(void)
{
    unsigned long attn;

    if (amiga_25519_picked)
        return;

    /* The same two facts src/crypto68k/c68k_variant.h names: the 68060 raises
       AFF_68040 too, so the 64-bit MULU.L is 020-and-up AND NOT an 060. */
    attn = (unsigned long)SysBase->AttnFlags;
    c68k_25519_cpu_select(((attn & AFF_68020) != 0UL &&
                           (attn & AFF_68060) == 0UL) ? 1u : 0u);

    amiga_25519_picked = 1;
}

#else
#define amiga_25519_pick()      ((void)0)
#endif

/* Ed25519's SHA-512, from the libtomcrypt already in this binary: c68k_25519.c
   has no hash of its own, and a second copy would be a second thing to keep
   right. */
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
    /* The return value is dropped because kexcurve25519_comb_key() takes no
       status from this call.  RFC 7748 6.1's all-zero check still happens inside
       c68k_x25519(), as it did in the TweetNaCl version being replaced. */
    amiga_25519_pick();

    (void)c68k_x25519(q, n, p);
}

void __wrap_dropbear_ed25519_make_key(unsigned char *pk, unsigned char *sk);
void __wrap_dropbear_ed25519_make_key(unsigned char *pk, unsigned char *sk)
{
    amiga_25519_pick();

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
    amiga_25519_pick();

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
    amiga_25519_pick();

    if (slen < 64)
        return -1;
    return c68k_ed25519_verify(amiga_sha512_3, m, mlen, s, pk);
}
