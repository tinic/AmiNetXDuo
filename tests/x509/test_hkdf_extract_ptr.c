/*
 * N-160: NX_CRYPTO_HKDF_EXTRACT copies the PRK to `output` and hands `input`
 * to the HMAC as the IKM.  A NULL output, or a NULL input with a length, is
 * refused with NX_CRYPTO_POINTER_ERROR before the arm stores anything in the
 * context: the HKDF control block is compared byte for byte across the call.
 *
 * Nothing invalid is ever dereferenced here.  The refused calls never reach
 * a copy or the HMAC, and H_CONTROLS_ONLY builds the valid half alone, which
 * is what runs against a tree without the guard.
 *
 * The controls run the same sequence as _nx_secure_tls_hkdf_extract()
 * (nx_secure_tls_1_3_generate_keys.c): init with the IKM, SET_HMAC, SET_HASH,
 * EXTRACT.  RFC 5869 A.1 and A.3 (an empty salt), an empty IKM both as NULL
 * and as a pointer, and an output that is also the salt, as the handshake and
 * master secret derivations pass it.  The empty-IKM PRK is HMAC-SHA-256 with
 * the A.1 salt over no bytes, computed outside this tree (Python hmac).
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include "nx_crypto_hkdf.h"
#include "nx_crypto_hmac.h"

extern NX_CRYPTO_METHOD crypto_method_hkdf;
extern NX_CRYPTO_METHOD crypto_method_hmac;
extern NX_CRYPTO_METHOD crypto_method_sha256;

static int failures;
static int checks;

static void expect(int ok, const char *what)
{
    checks++;
    if (!ok)
    {
        printf("FAIL %s\n", what);
        failures++;
    }
}

static union
{
    ULONG align;
    UCHAR bytes[sizeof(NX_CRYPTO_HKDF) + sizeof(NX_CRYPTO_HMAC)];
} metadata;

/* RFC 5869 A.1 and A.3. */
static UCHAR       a1_ikm[22];
static UCHAR       a1_salt[13];
static const UCHAR a1_prk[32] =
{
    0x07, 0x77, 0x09, 0x36, 0x2c, 0x2e, 0x32, 0xdf, 0x0d, 0xdc, 0x3f, 0x0d, 0xc4, 0x7b, 0xba, 0x63,
    0x90, 0xb6, 0xc7, 0x3b, 0xb5, 0x0f, 0x9c, 0x31, 0x22, 0xec, 0x84, 0x4a, 0xd7, 0xc2, 0xb3, 0xe5
};
static const UCHAR a3_prk[32] =
{
    0x19, 0xef, 0x24, 0xa3, 0x2c, 0x71, 0x7b, 0x16, 0x7f, 0x33, 0xa9, 0x1d, 0x6f, 0x64, 0x8b, 0xdf,
    0x96, 0x59, 0x67, 0x76, 0xaf, 0xdb, 0x63, 0x77, 0xac, 0x43, 0x4c, 0x1c, 0x29, 0x3c, 0xcb, 0x04
};
/* HMAC-SHA-256(key = A.1 salt, message = empty). */
static const UCHAR empty_ikm_prk[32] =
{
    0x90, 0xa3, 0x3d, 0x18, 0x6b, 0x94, 0x0b, 0xac, 0x8a, 0x4e, 0x69, 0xef, 0xce, 0x8b, 0x74, 0xba,
    0x4c, 0x71, 0x86, 0x40, 0x14, 0x0d, 0x54, 0xd8, 0x53, 0xb5, 0xe3, 0x0e, 0xd8, 0xd7, 0x70, 0x6b
};

/* init, SET_HMAC, SET_HASH: the context EXTRACT runs in. */
static UINT setup(UCHAR *ikm, UINT ikm_len)
{
    UINT status;

    memset(&metadata, 0, sizeof(metadata));
    status = crypto_method_hkdf.nx_crypto_init(&crypto_method_hkdf, ikm, ikm_len << 3, NX_CRYPTO_NULL,
                                               metadata.bytes, sizeof(metadata.bytes));
    if (status == NX_CRYPTO_SUCCESS)
        status = crypto_method_hkdf.nx_crypto_operation(NX_CRYPTO_HKDF_SET_HMAC, NX_CRYPTO_NULL,
                                                        &crypto_method_hmac, NX_CRYPTO_NULL, 0,
                                                        NX_CRYPTO_NULL, 0, NX_CRYPTO_NULL,
                                                        NX_CRYPTO_NULL, 0, metadata.bytes,
                                                        sizeof(metadata.bytes), NX_CRYPTO_NULL,
                                                        NX_CRYPTO_NULL);
    if (status == NX_CRYPTO_SUCCESS)
        status = crypto_method_hkdf.nx_crypto_operation(NX_CRYPTO_HKDF_SET_HASH, NX_CRYPTO_NULL,
                                                        &crypto_method_sha256, NX_CRYPTO_NULL, 0,
                                                        NX_CRYPTO_NULL, 0, NX_CRYPTO_NULL,
                                                        NX_CRYPTO_NULL, 0, metadata.bytes,
                                                        sizeof(metadata.bytes), NX_CRYPTO_NULL,
                                                        NX_CRYPTO_NULL);
    return status;
}

static UINT extract(UCHAR *salt, UINT salt_len, UCHAR *ikm, UINT ikm_len, UCHAR *out, UINT out_len)
{
    return crypto_method_hkdf.nx_crypto_operation(NX_CRYPTO_HKDF_EXTRACT, NX_CRYPTO_NULL,
                                                  &crypto_method_hkdf, salt, salt_len << 3,
                                                  ikm, ikm_len, NX_CRYPTO_NULL, out, out_len,
                                                  metadata.bytes, sizeof(metadata.bytes),
                                                  NX_CRYPTO_NULL, NX_CRYPTO_NULL);
}

static void control(const char *name, UCHAR *salt, UINT salt_len, UCHAR *ikm, UINT ikm_len,
                    const UCHAR *want)
{
    UCHAR out[32 + 8];
    UINT  status;
    char  what[160];

    memset(out, 0x5A, sizeof(out));
    status = setup(ikm, ikm_len);
    if (status == NX_CRYPTO_SUCCESS)
        status = extract(salt, salt_len, ikm, ikm_len, out, 32);

    snprintf(what, sizeof(what), "%s: status 0x%x", name, status);
    expect(status == NX_CRYPTO_SUCCESS, what);
    snprintf(what, sizeof(what), "%s: the PRK", name);
    expect(memcmp(out, want, 32) == 0, what);
    snprintf(what, sizeof(what), "%s: nothing past the PRK", name);
    expect(out[32] == 0x5A && out[39] == 0x5A, what);
}

/* The handshake and master secrets are derived in place: output is salt. */
static void control_aliased(void)
{
    UCHAR buf[32];
    UINT  status;

    memset(buf, 0, sizeof(buf));
    memcpy(buf, a1_salt, sizeof(a1_salt));
    status = setup(a1_ikm, sizeof(a1_ikm));
    if (status == NX_CRYPTO_SUCCESS)
        status = extract(buf, sizeof(a1_salt), a1_ikm, sizeof(a1_ikm), buf, sizeof(buf));

    expect(status == NX_CRYPTO_SUCCESS, "output aliasing the salt: status");
    expect(memcmp(buf, a1_prk, 32) == 0, "output aliasing the salt: the A.1 PRK");
}

/* What was refused before the guard, and still is. */
static void unchanged_contracts(void)
{
    UCHAR out[32];

    setup(a1_ikm, sizeof(a1_ikm));
    expect(extract(NX_CRYPTO_NULL, 0, a1_ikm, sizeof(a1_ikm), out, sizeof(out)) == NX_CRYPTO_POINTER_ERROR,
           "a NULL salt is still NX_CRYPTO_POINTER_ERROR");

    setup(a1_ikm, sizeof(a1_ikm));
    expect(extract(a1_salt, sizeof(a1_salt), a1_ikm, sizeof(a1_ikm), out, 31) == NX_CRYPTO_SIZE_ERROR,
           "an output one byte short is still NX_CRYPTO_SIZE_ERROR");
}

#ifndef H_CONTROLS_ONLY
static void refused(const char *name, UCHAR *ikm, UINT ikm_len, UCHAR *out)
{
    static UCHAR before[sizeof(metadata.bytes)];
    UINT         status;
    char         what[160];

    setup(a1_ikm, sizeof(a1_ikm));
    memcpy(before, metadata.bytes, sizeof(before));

    status = extract(a1_salt, sizeof(a1_salt), ikm, ikm_len, out, 32);

    snprintf(what, sizeof(what), "N-160 %s: status 0x%x, want NX_CRYPTO_POINTER_ERROR", name, status);
    expect(status == NX_CRYPTO_POINTER_ERROR, what);
    snprintf(what, sizeof(what), "N-160 %s: the HKDF context is untouched", name);
    expect(memcmp(before, metadata.bytes, sizeof(before)) == 0, what);
}
#endif

int main(void)
{
    UCHAR empty[1];

    memset(a1_ikm, 0x0b, sizeof(a1_ikm));
    {
        UINT i;
        for (i = 0; i < sizeof(a1_salt); i++)
            a1_salt[i] = (UCHAR)i;
    }

    control("RFC 5869 A.1", a1_salt, sizeof(a1_salt), a1_ikm, sizeof(a1_ikm), a1_prk);
    control("RFC 5869 A.3 (empty salt)", empty, 0, a1_ikm, sizeof(a1_ikm), a3_prk);
    control("empty IKM as NULL", a1_salt, sizeof(a1_salt), NX_CRYPTO_NULL, 0, empty_ikm_prk);
    control("empty IKM as a pointer", a1_salt, sizeof(a1_salt), empty, 0, empty_ikm_prk);
    control_aliased();
    unchanged_contracts();

#ifndef H_CONTROLS_ONLY
    {
        UCHAR out[32];
        UCHAR canary[32];

        memset(out, 0x5A, sizeof(out));
        memset(canary, 0x5A, sizeof(canary));
        refused("NULL output", a1_ikm, sizeof(a1_ikm), NX_CRYPTO_NULL);
        refused("NULL IKM with a length", NX_CRYPTO_NULL, sizeof(a1_ikm), out);
        expect(memcmp(out, canary, sizeof(out)) == 0, "N-160 NULL IKM with a length: no PRK written");
    }
#endif

    printf("hkdf_extract_ptr: %d checks, %d failures\n", checks, failures);

    return failures == 0 ? 0 : 1;
}
