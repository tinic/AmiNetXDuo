/*
 * The tests for src/tools/updsig.c: upd_verify() against signatures openssl
 * made with the two TEST keys in tests/updater/.  Neither key signs anything
 * real.  To make the vectors again:
 *
 *   openssl pkey -in tests/updater/test-only-update-key-a.pem -pubout \
 *       -outform DER | tail -c 32 | od -An -tx1 -v | tr -d ' \n'
 *   openssl pkeyutl -sign -rawin \
 *       -inkey tests/updater/test-only-update-key-a.pem -in m -out s
 *
 * with `m` holding exactly MANIFEST below.
 *
 * SPDX-License-Identifier: MIT
 */

#include "updsig.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                    \
    } while (0)

#define MANIFEST                                                             \
    "format=1\n"                                                             \
    "product=AmiNetXDuo\n"                                                   \
    "version=1.0.0-rc5\n"                                                    \
    "channel=prerelease\n"                                                   \
    "container=full 5 "                                                      \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef "      \
    "AmiNetXDuo-1.0.0-rc5-full.update.lha\n"                                 \
    "end=5\n"

#define PK_A "1d818ade40eee9daac034dd166eebdabc0cd6116eb468b1846b044fbe1590ad7"
#define PK_B "1a9e8c5db8aebe8a014ff615e1168cb415aef029707317235dd5546b3ddd2fff"

/* MANIFEST signed by key a, and by key b. */
#define SIG_A                                                                \
    "c3b729d6f8a76e80a044ad02b14cfa4a5646abba19a1e11e3d1bb01daa6d6c08"       \
    "cf187bd6c9ffa6692078144b7f52fd64c92c028a4eb59963e147e43ad49cb601"
#define SIG_B                                                                \
    "6bbb9153819d69c7a52454410664fb8d93390d48d1eceb89d995335b5da47e95"       \
    "8815be7628f2ac487ae845393756d89fbce6aeadb21120bf6134c498fcc5a60e"

/* SIG_A with one bit of R flipped. */
#define SIG_A_BAD                                                            \
    "c2b729d6f8a76e80a044ad02b14cfa4a5646abba19a1e11e3d1bb01daa6d6c08"       \
    "cf187bd6c9ffa6692078144b7f52fd64c92c028a4eb59963e147e43ad49cb601"

static void unhex(unsigned char *out, const char *s)
{
    size_t i;

    for (i = 0; i < strlen(s) / 2; i++)
    {
        unsigned v = 0;

        sscanf(s + 2 * i, "%2x", &v);
        out[i] = (unsigned char)v;
    }
}

static UpdKey keys[3];

static UpdResult verify_n(const char *m, const char *sig, unsigned int nkeys)
{
    return upd_verify((const unsigned char *)m, (unsigned long)strlen(m),
                      sig, (unsigned long)strlen(sig), keys, nkeys);
}

static UpdResult verify(const char *sig)
{
    return verify_n(MANIFEST, sig, 1);
}

int main(void)
{
    char m[512];

    keys[0].id = "test-a";
    unhex(keys[0].pk, PK_A);
    keys[1].id = "test-b";
    unhex(keys[1].pk, PK_B);
    keys[2].id = "test-c";
    unhex(keys[2].pk, PK_A);

    /* good */
    CHECK(verify("key=test-a sig=" SIG_A "\n") == UPD_OK);

    /* bad signature, and a good one over a changed manifest */
    CHECK(verify("key=test-a sig=" SIG_A_BAD "\n") == UPD_E_SIG_BAD);
    snprintf(m, sizeof(m), "%s", MANIFEST);
    m[strlen(m) - 2] = '6';
    CHECK(verify_n(m, "key=test-a sig=" SIG_A "\n", 1) == UPD_E_SIG_BAD);
    CHECK(upd_verify((const unsigned char *)MANIFEST, strlen(MANIFEST) - 1,
                     "key=test-a sig=" SIG_A "\n",
                     strlen("key=test-a sig=" SIG_A "\n"), keys, 1) ==
          UPD_E_SIG_BAD);

    /* key b's signature filed under key a's id */
    CHECK(verify("key=test-a sig=" SIG_B "\n") == UPD_E_SIG_BAD);

    /* an id the tool does not hold, even with a signature that verifies */
    CHECK(verify("key=test-x sig=" SIG_A "\n") == UPD_E_SIG_UNKNOWN_KEY);
    CHECK(verify("key=test-b sig=" SIG_B "\n") == UPD_E_SIG_UNKNOWN_KEY);

    /* two lines, one good: either order, unknown or bad first */
    CHECK(verify("key=test-x sig=" SIG_B "\nkey=test-a sig=" SIG_A "\n") ==
          UPD_OK);
    CHECK(verify("key=test-a sig=" SIG_A "\nkey=test-x sig=" SIG_B "\n") ==
          UPD_OK);
    CHECK(verify_n(MANIFEST,
                   "key=test-a sig=" SIG_A_BAD "\nkey=test-b sig=" SIG_B "\n",
                   2) == UPD_OK);
    CHECK(verify_n(MANIFEST,
                   "key=test-a sig=" SIG_A_BAD "\nkey=test-b sig=" SIG_A "\n",
                   2) == UPD_E_SIG_BAD);

    /* a list of three, the signer not first */
    CHECK(verify_n(MANIFEST, "key=test-b sig=" SIG_B "\n", 3) == UPD_OK);
    CHECK(verify_n(MANIFEST, "key=test-c sig=" SIG_A "\n", 3) == UPD_OK);

    /* a malformed file is refused whole, even beside a good line */
    CHECK(verify("key=test-a sig=" SIG_A "\nkey=test-x sig=00\n") ==
          UPD_E_HEX);
    CHECK(verify("key=test-a sig=" SIG_A) == UPD_E_TRUNCATED);
    CHECK(verify("") == UPD_E_TRUNCATED);

    /* the key list */
    CHECK(verify_n(MANIFEST, "key=test-a sig=" SIG_A "\n", 0) == UPD_E_ARG);
    CHECK(upd_verify((const unsigned char *)MANIFEST, strlen(MANIFEST),
                     "key=test-a sig=" SIG_A "\n",
                     strlen("key=test-a sig=" SIG_A "\n"), keys,
                     UPD_KEYS_MAX + 1) == UPD_E_ARG);

    printf("test_updsig: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
