/*
 * clients/dropbear/tweetnacl-count.c -- how many field multiplications an SSH
 * handshake costs, counted on the build host.
 *
 * The emulator queue is deep and every timing run has to hold the machine
 * alone (tools/fsuae-run.sh -x), so anything that does not need a 68020 should
 * not take a slot.  An operation count does not: 2^255-19 arithmetic executes
 * the same number of multiplies on any machine, and the count is the half of
 * the cost model a wall clock cannot give.  The guest measures milliseconds
 * per primitive, this measures multiplies per primitive, and dividing one by
 * the other gives the cost of one field multiply on this part.
 *
 * Reaching a `static` function without patching third_party/dropbear:
 * clients/dropbear/tweetnacl-count.sh derives a copy of
 * third_party/dropbear/src/curve25519.c into build/, renaming the two
 * definitions `M` and `S` and inserting counting macros of the same names
 * directly after them.  Every later use in the file -- and every use is later,
 * because TweetNaCl defines bottom-up -- goes through the counter.  The
 * submodule is untouched; the derived file is a build artifact the script
 * regenerates, so it cannot drift from the pinned tag unnoticed.
 *
 * The counts are therefore of the same code the Amiga runs, not of a
 * re-implementation.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Provided by the derived curve25519 copy. */
extern unsigned long tn_count_M;      /* every M(), including the ones S() does */
extern unsigned long tn_count_S;      /* squarings only                          */

void dropbear_curve25519_scalarmult(unsigned char *q, const unsigned char *n,
                                    const unsigned char *p);
void dropbear_ed25519_sign(const unsigned char *m, unsigned long mlen,
                           unsigned char *s, unsigned long *slen,
                           const unsigned char *sk, const unsigned char *pk);
int  dropbear_ed25519_verify(const unsigned char *m, unsigned long mlen,
                             const unsigned char *s, unsigned long slen,
                             const unsigned char *pk);

/*
 * RFC 8032 section 7.1, test 2.  A real key pair and a real signature, so the
 * counts are of a correct operation and the harness checks itself before it
 * reports anything.  The 1-byte message is what makes this test 2 rather than
 * test 1; SSH signs a 32-byte exchange hash, and the message length moves only
 * the SHA-512, never the curve arithmetic.
 */
static const unsigned char sk2[32] = {
    0x4c,0xcd,0x08,0x9b,0x28,0xff,0x96,0xda,0x9d,0xb6,0xc3,0x46,0xec,0x11,0x4e,0x0f,
    0x5b,0x8a,0x31,0x9f,0x35,0xab,0xa6,0x24,0xda,0x8c,0xf6,0xed,0x4f,0xb8,0xa6,0xfb
};
static const unsigned char pk2[32] = {
    0x3d,0x40,0x17,0xc3,0xe8,0x43,0x89,0x5a,0x92,0xb7,0x0a,0xa7,0x4d,0x1b,0x7e,0xbc,
    0x9c,0x98,0x2c,0xcf,0x2e,0xc4,0x96,0x8c,0xc0,0xcd,0x55,0xf1,0x2a,0xf4,0x66,0x0c
};
static const unsigned char msg2[1] = { 0x72 };
static const unsigned char sig2[64] = {
    0x92,0xa0,0x09,0xa9,0xf0,0xd4,0xca,0xb8,0x72,0x0e,0x82,0x0b,0x5f,0x64,0x25,0x40,
    0xa2,0xb2,0x7b,0x54,0x16,0x50,0x3f,0x8f,0xb3,0x76,0x22,0x23,0xeb,0xdb,0x69,0xda,
    0x08,0x5a,0xc1,0xe4,0x3e,0x15,0x99,0x6e,0x45,0x8f,0x36,0x13,0xd0,0xf1,0x1d,0x8c,
    0x38,0x7b,0x2e,0xae,0xb4,0x30,0x2a,0xee,0xb0,0x0d,0x29,0x16,0x12,0xbb,0x0c,0x00
};

/* RFC 7748 section 6.1: Alice's private key and the curve25519 base point. */
static const unsigned char x25519_sk[32] = {
    0x77,0x07,0x6d,0x0a,0x73,0x18,0xa5,0x7d,0x3c,0x16,0xc1,0x72,0x51,0xb2,0x66,0x45,
    0xdf,0x4c,0x2f,0x87,0xeb,0xc0,0x99,0x2a,0xb1,0x77,0xfb,0xa5,0x1d,0xb9,0x2c,0x2a
};
static const unsigned char x25519_pk_expect[32] = {
    0x85,0x20,0xf0,0x09,0x89,0x30,0xa7,0x54,0x74,0x8b,0x7d,0xdc,0xb4,0x3e,0xf7,0x5a,
    0x0d,0xbf,0x3a,0x0d,0x26,0x38,0x1a,0xf4,0xeb,0xa4,0xa9,0x8e,0xaa,0x9b,0x4e,0x6a
};
static const unsigned char basepoint[32] = { 9 };

/* dropbear_ed25519_make_key() is compiled into curve25519.c and reaches
   dbrandom.c, which reaches an entropy device this program has no business
   opening.  It is never called here -- key generation is not part of a
   handshake -- so one stub satisfies the linker without pulling in a random
   pool.  If it is ever reached, it says so rather than returning zeros. */
void genrandom(unsigned char *buf, unsigned int len);
void genrandom(unsigned char *buf, unsigned int len)
{
    (void)buf; (void)len;
    fprintf(stderr, "genrandom() reached -- this program signs and verifies "
                    "with fixed test vectors and should never generate a key\n");
    exit(2);
}

static int failures;

static void row(const char *what, unsigned long m0, unsigned long s0)
{
    unsigned long m = tn_count_M - m0;
    unsigned long s = tn_count_S - s0;

    printf("  %-34s %8lu %8lu %9lu\n", what, m - s, s, m);
}

static void check(const char *what, int ok)
{
    if (!ok) {
        printf("  !! %s FAILED\n", what);
        failures++;
    }
}

int main(void)
{
    unsigned char out[32], sig[64], pub[32];
    unsigned long siglen = 0, m0, s0;

    printf("TweetNaCl field operations per SSH handshake primitive\n");
    printf("  (counted in third_party/dropbear/src/curve25519.c itself)\n\n");
    printf("  %-34s %8s %8s %9s\n", "primitive", "mul", "sqr", "total M");
    printf("  %-34s %8s %8s %9s\n", "---------", "---", "---", "-------");

    /* 1. The kex keypair: scalar times the base point. */
    m0 = tn_count_M; s0 = tn_count_S;
    dropbear_curve25519_scalarmult(out, x25519_sk, basepoint);
    row("curve25519 keygen (k.basepoint)", m0, s0);
    check("RFC 7748 X25519 public key",
          memcmp(out, x25519_pk_expect, 32) == 0);
    memcpy(pub, out, 32);

    /* 2. The shared secret: the same routine on the peer's point, counted
          separately in case the two ever diverge. */
    m0 = tn_count_M; s0 = tn_count_S;
    dropbear_curve25519_scalarmult(out, x25519_sk, pub);
    row("curve25519 shared secret", m0, s0);

    /* 3. The client's publickey authentication signature. */
    m0 = tn_count_M; s0 = tn_count_S;
    dropbear_ed25519_sign(msg2, sizeof(msg2), sig, &siglen, sk2, pk2);
    row("ed25519 sign", m0, s0);
    check("RFC 8032 test 2 signature",
          siglen == 64 && memcmp(sig, sig2, 64) == 0);

    /* 4. Verifying the server's host key signature over the exchange hash. */
    m0 = tn_count_M; s0 = tn_count_S;
    check("RFC 8032 test 2 verify",
          dropbear_ed25519_verify(msg2, sizeof(msg2), sig2, 64, pk2) == 0);
    row("ed25519 verify", m0, s0);

    printf("\n  %-34s %8s %8s %9lu\n", "ONE HANDSHAKE (sum of the four)",
           "", "", tn_count_M);

    if (failures) {
        printf("\n%d vector(s) FAILED -- the counts above are of code that is "
               "not doing the right thing.\n", failures);
        return 1;
    }
    printf("\n  all four vectors pass.\n");
    return 0;
}
