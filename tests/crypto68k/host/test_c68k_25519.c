/*
 * AmiNetXDuo, host vectors for src/crypto68k/c68k_25519.c.
 *
 * The code is portable C over <stdint.h>, so the vectors run here in a second
 * on every push rather than only under the emulator.  c68k_25519.c takes its
 * hash as a callback; this tier binds it to _nx_crypto_sha512_*.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "nx_crypto_sha5.h"

#include "c68k_25519.h"

/* Declared here rather than in the public header; see the note where it is
   defined. */
int c68k_25519_selfcheck_sqr(const unsigned char in[32]);


static int failures;

static void ck(const char *what, int ok)
{
    if (!ok) {
        printf("  FAIL  %s\n", what);
        failures++;
    }
}

static void unhex(unsigned char *out, const char *s)
{
    size_t i;

    for (i = 0; i < strlen(s) / 2; i++) {
        unsigned v = 0;
        sscanf(s + 2 * i, "%2x", &v);
        out[i] = (unsigned char)v;
    }
}

static void sha512_3(unsigned char out[64],
                     const unsigned char *a, unsigned long alen,
                     const unsigned char *b, unsigned long blen,
                     const unsigned char *c, unsigned long clen)
{
    static NX_CRYPTO_SHA512 ctx;

    _nx_crypto_sha512_initialize(&ctx, NX_CRYPTO_HASH_SHA512);
    if (alen)
        _nx_crypto_sha512_update(&ctx, (UCHAR *)a, (UINT)alen);
    if (blen)
        _nx_crypto_sha512_update(&ctx, (UCHAR *)b, (UINT)blen);
    if (clen)
        _nx_crypto_sha512_update(&ctx, (UCHAR *)c, (UINT)clen);
    _nx_crypto_sha512_digest_calculate(&ctx, out, NX_CRYPTO_HASH_SHA512);
}


/* ------------------------------------------------------------ the field - */

static void test_sqr_is_mul(void)
{
    unsigned char a[32];
    int           i, j, bad = 0;

    srand(20260726);

    /* All-zero, all-ones and 2^256-38 first: the lazy representation carries 0
       as 2^256-38 routinely, and that is where the carry bugs live. */
    memset(a, 0x00, 32); bad |= c68k_25519_selfcheck_sqr(a) != 0;
    memset(a, 0xff, 32); bad |= c68k_25519_selfcheck_sqr(a) != 0;
    memset(a, 0xff, 32); a[0] = 0xda; bad |= c68k_25519_selfcheck_sqr(a) != 0;

    for (i = 0; i < 20000; i++) {
        for (j = 0; j < 32; j++)
            a[j] = (unsigned char)(rand() >> 5);
        if (c68k_25519_selfcheck_sqr(a) != 0) {
            bad = 1;
            break;
        }
    }
    ck("fe_sqr agrees with fe_mul on 20,003 inputs", bad == 0);
}


/* ------------------------------------------------------------- X25519 --- */

static void test_x25519(void)
{
    unsigned char sk_a[32], sk_b[32], pk_a[32], pk_b[32];
    unsigned char want[32], got[32], k[32], u[32];

    /* RFC 7748 section 6.1 */
    unhex(sk_a, "77076d0a7318a57d3c16c17251b26645"
                "df4c2f87ebc0992ab177fba51db92c2a");
    unhex(sk_b, "5dab087e624a8a4b79e17f8b83800ee6"
                "6f3bb1292618b6fd1c2f8b27ff88e0eb");

    ck("X25519 base(a) succeeds", c68k_x25519_base(pk_a, sk_a) == 0);
    unhex(want, "8520f0098930a754748b7ddcb43ef75a"
                "0dbf3a0d26381af4eba4a98eaa9b4e6a");
    ck("RFC 7748 6.1 alice public", memcmp(pk_a, want, 32) == 0);

    ck("X25519 base(b) succeeds", c68k_x25519_base(pk_b, sk_b) == 0);
    unhex(want, "de9edb7d7b7dc1b4d35b61c2ece43537"
                "3f8343c85b78674dadfc7e146f882b4f");
    ck("RFC 7748 6.1 bob public", memcmp(pk_b, want, 32) == 0);

    unhex(want, "4a5d9d5ba4ce2de1728e3bf480350f25"
                "e07e21c947d19e3376f09b3c1e161742");
    ck("X25519 a*B succeeds", c68k_x25519(got, sk_a, pk_b) == 0);
    ck("RFC 7748 6.1 shared, a*B", memcmp(got, want, 32) == 0);
    ck("X25519 b*A succeeds", c68k_x25519(got, sk_b, pk_a) == 0);
    ck("RFC 7748 6.1 shared, b*A", memcmp(got, want, 32) == 0);

    /* RFC 7748 section 5.2, the two direct vectors.  These use a u with its
       high bit set and a non-canonical scalar, which is exactly what the
       clamping and the bit-255 mask are for. */
    unhex(k, "a546e36bf0527c9d3b16154b82465edd"
             "62144c0ac1fc5a18506a2244ba449ac4");
    unhex(u, "e6db6867583030db3594c1a424b15f7c"
             "726624ec26b3353b10a903a6d0ab1c4c");
    unhex(want, "c3da55379de9c6908e94ea4df28d084f"
                "32eccf03491c71f754b4075577a28552");
    (void)c68k_x25519(got, k, u);
    ck("RFC 7748 5.2 vector 1", memcmp(got, want, 32) == 0);

    unhex(k, "4b66e9d4d1b4673c5ad22691957d6af5"
             "c11b6421e0ea01d42ca4169e7918ba0d");
    unhex(u, "e5210f12786811d3f4b7959d0538ae2c"
             "31dbe7106fc03c3efc4cd549c715a493");
    unhex(want, "95cbde9476e8907d7aade45cb4b873f8"
                "8b595a68799fa152e6f8f7647aac7957");
    (void)c68k_x25519(got, k, u);
    ck("RFC 7748 5.2 vector 2", memcmp(got, want, 32) == 0);
}


/* ------------------------------------------------------------ Ed25519 --- */

static void ed_case(const char *name, const char *sk_hex, const char *pk_hex,
                    const char *msg_hex, const char *sig_hex)
{
    unsigned char sk[32], pk[32], want_pk[32], sig[64], want_sig[64];
    unsigned char msg[128];
    unsigned long mlen = (unsigned long)(strlen(msg_hex) / 2);
    char          label[96];

    unhex(sk, sk_hex);
    unhex(want_pk, pk_hex);
    unhex(want_sig, sig_hex);
    if (mlen)
        unhex(msg, msg_hex);

    c68k_ed25519_pubkey(sha512_3, pk, sk);
    snprintf(label, sizeof(label), "%s public key", name);
    ck(label, memcmp(pk, want_pk, 32) == 0);

    c68k_ed25519_sign(sha512_3, sig, msg, mlen, sk, want_pk);
    snprintf(label, sizeof(label), "%s signature", name);
    ck(label, memcmp(sig, want_sig, 64) == 0);

    snprintf(label, sizeof(label), "%s verifies", name);
    ck(label, c68k_ed25519_verify(sha512_3, msg, mlen, want_sig, want_pk) == 0);

    /* A verifier that returns success unconditionally passes every positive
       vector ever published, so each one is broken three ways as well. */
    memcpy(sig, want_sig, 64);
    sig[0] ^= 0x01;
    snprintf(label, sizeof(label), "%s refuses a mangled R", name);
    ck(label, c68k_ed25519_verify(sha512_3, msg, mlen, sig, want_pk) != 0);

    memcpy(sig, want_sig, 64);
    sig[40] ^= 0x01;
    snprintf(label, sizeof(label), "%s refuses a mangled S", name);
    ck(label, c68k_ed25519_verify(sha512_3, msg, mlen, sig, want_pk) != 0);

    if (mlen) {
        msg[0] ^= 0x01;
        snprintf(label, sizeof(label), "%s refuses a mangled message", name);
        ck(label, c68k_ed25519_verify(sha512_3, msg, mlen, want_sig,
                                      want_pk) != 0);
    }
}

static void test_ed25519(void)
{
    /* RFC 8032 section 7.1 */
    ed_case("RFC 8032 TEST 1",
            "9d61b19deffd5a60ba844af492ec2cc4"
            "4449c5697b326919703bac031cae7f60",
            "d75a980182b10ab7d54bfed3c964073a"
            "0ee172f3daa62325af021a68f707511a",
            "",
            "e5564300c360ac729086e2cc806e828a"
            "84877f1eb8e5d974d873e06522490155"
            "5fb8821590a33bacc61e39701cf9b46b"
            "d25bf5f0595bbe24655141438e7a100b");

    ed_case("RFC 8032 TEST 2",
            "4ccd089b28ff96da9db6c346ec114e0f"
            "5b8a319f35aba624da8cf6ed4fb8a6fb",
            "3d4017c3e843895a92b70aa74d1b7ebc"
            "9c982ccf2ec4968cc0cd55f12af4660c",
            "72",
            "92a009a9f0d4cab8720e820b5f642540"
            "a2b27b5416503f8fb3762223ebdb69da"
            "085ac1e43e15996e458f3613d0f11d8c"
            "387b2eaeb4302aeeb00d291612bb0c00");

    ed_case("RFC 8032 TEST 3",
            "c5aa8df43f9f837bedb7442f31dcb7b1"
            "66d38535076f094b85ce3a2e0b4458f7",
            "fc51cd8e6218a1a38da47ed00230f058"
            "0816ed13ba3303ac5deb911548908025",
            "af82",
            "6291d657deec24024827e69c3abe01a3"
            "0ce548a284743a445e3680d7db5ac3ac"
            "18ff9b538d16f290ae67f760984dc659"
            "4a7c15e9716ed28dc027beceea1ec40a");

    ed_case("RFC 8032 TEST SHA(abc)",
            "833fe62409237b9d62ec77587520911e"
            "9a759cec1d19755b7da901b96dca3d42",
            "ec172b93ad5e563bf4932c70e1245034"
            "c35467ef2efd4d64ebf819683467e2bf",
            "ddaf35a193617abacc417349ae204131"
            "12e6fa4e89a97ea20a9eeee64b55d39a"
            "2192992a274fc1a836ba3c23a3feebbd"
            "454d4423643ce80e2a9ac94fa54ca49f",
            "dc2a4459e7369633a52b1bf277839a00"
            "201009a3efbf3ecb69bea2186c26b589"
            "09351fc9ac90b3ecfdfbc7c66431e030"
            "3dca179c138ac17ad9bef1177331a704");
}


int main(void)
{
    printf("crypto68k 25519 vectors\n");

    test_sqr_is_mul();
    test_x25519();
    test_ed25519();

    if (failures) {
        printf("%d FAILURE(S)\n", failures);
        return 1;
    }
    printf("all vectors pass\n");
    return 0;
}
