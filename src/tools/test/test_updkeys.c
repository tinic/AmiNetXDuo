/*
 * The tests for src/tools/updkeys.c: which keys a build holds.  Compiled
 * twice, as test_updkeys (a release build) and test_updkeys_testkey (with
 * UPD_TEST_KEY, the scenario build):
 *
 *   release   holds rel-2026 and nothing else; a manifest signed by the
 *             committed TEST key is refused as an unknown key, also when the
 *             signature file names it under the release key's id
 *   test key  holds test-a and nothing else; the same manifest verifies
 *
 * MANIFEST and SIG_A are test_updsig.c's: openssl's signature by
 * tests/updater/test-only-update-key-a.pem.  Nothing here is signed by the
 * release key, whose private half is never in the tree.
 *
 * SPDX-License-Identifier: MIT
 */

#include "updkeys.h"

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
#define PK_REL "8e523c5d33b7911c9fb3cd958ef23f64c6ba60311e4042e0c15ee92c4763f448"

#define SIG_A                                                                \
    "c3b729d6f8a76e80a044ad02b14cfa4a5646abba19a1e11e3d1bb01daa6d6c08"       \
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

static UpdResult verify(const char *sig)
{
    return upd_verify((const unsigned char *)MANIFEST,
                      (unsigned long)strlen(MANIFEST), sig,
                      (unsigned long)strlen(sig), upd_keys, upd_key_count);
}

int main(void)
{
    unsigned char pk_a[32], pk_rel[32];

    unhex(pk_a, PK_A);
    unhex(pk_rel, PK_REL);

    CHECK(upd_key_count == 1);

#ifdef UPD_TEST_KEY
    CHECK(strcmp(upd_keys[0].id, "test-a") == 0);
    CHECK(memcmp(upd_keys[0].pk, pk_a, 32) == 0);
    CHECK(verify("key=test-a sig=" SIG_A "\n") == UPD_OK);
    CHECK(verify("key=rel-2026 sig=" SIG_A "\n") == UPD_E_SIG_UNKNOWN_KEY);
#else
    CHECK(strcmp(upd_keys[0].id, "rel-2026") == 0);
    CHECK(memcmp(upd_keys[0].pk, pk_rel, 32) == 0);
    CHECK(memcmp(upd_keys[0].pk, pk_a, 32) != 0);
    CHECK(verify("key=test-a sig=" SIG_A "\n") == UPD_E_SIG_UNKNOWN_KEY);
    CHECK(verify("key=rel-2026 sig=" SIG_A "\n") == UPD_E_SIG_BAD);
#endif

    printf("updkeys: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
