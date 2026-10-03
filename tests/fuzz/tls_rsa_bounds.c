/*
 * N-146: the RSA methods - the vendored crypto_method_rsa and ours,
 * ami_crypto_method_rsa (src/tls/ami_tls_crypto.c) - must refuse a modulus
 * larger than the scratch and every caller's output buffer are sized for,
 * check the output against the MODULUS (the key_size they are handed is the
 * exponent's), use the scratch length they are given, in the right units,
 * and report a number that does not fit rather than compute on a stale one.
 *
 * Benign contracts only: no oversized exponentiation is ever run, and an
 * output that is too short sits inside a larger array whose tail is a
 * canary, so the old code's overrun lands in the test's own memory.  A real
 * signature is decrypted with the CA's 2048-bit key to show a valid
 * operation still works with an output exactly the modulus long.
 *
 * 32-BIT ONLY, for tls_rsa_key_regression's reason (tests/fuzz/CMakeLists.txt).
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nx_secure_tls.h"
#include "nx_secure_x509.h"
#include "nx_crypto_rsa.h"
#include "ami_tls_crypto.h"
#include "tls.h"

#include "tls_test_certs.h"

/* tls.library's timer, which the crypto module reads for its counters. */
BOOL  ami_tls_timer_is_open(VOID)          { return 0; }
ULONG ami_tls_eclock(VOID)                 { return 0; }
ULONG ami_tls_eclock_micros(ULONG ticks)   { return ticks; }

/* The header's device key is not needed here; it is static, so name it once. */
static const void *const unused_key[] = { test_device_cert_key_der, &test_device_cert_key_der_len };

static int checks;
static int failures;

#define CHECK(c, what) do { checks++; if (!(c)) { failures++; \
    printf("FAIL %s: %s\n", method_name, what); } } while (0)

static const char *method_name = "";

extern NX_CRYPTO_METHOD crypto_method_rsa;

/* Metadata big enough for either context (ours embeds NX_CRYPTO_RSA first). */
static ULONG metadata_words[32768 / sizeof(ULONG)];
#define METADATA       ((UCHAR *)metadata_words)
#define METADATA_SIZE  ((ULONG)sizeof(metadata_words))

static const NX_CRYPTO_METHOD *ami_rsa(void)
{
    const NX_SECURE_TLS_CIPHERSUITE_INFO *table =
        ami_crypto_tls_ciphers_ecc.nx_secure_tls_ciphersuite_lookup_table;
    USHORT n = ami_crypto_tls_ciphers_ecc.nx_secure_tls_ciphersuite_lookup_table_size;
    USHORT i;

    for (i = 0; i < n; i++)
        if (table[i].nx_secure_tls_ciphersuite == TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256)
            return table[i].nx_secure_tls_public_auth;
    return NX_NULL;
}

static NX_SECURE_X509_CERT ca, leaf;
static UCHAR               ca_der[2048], leaf_der[2048];

static int load_certs(void)
{
    memcpy(ca_der, test_ca_cert_der, test_ca_cert_der_len);
    memcpy(leaf_der, test_device_cert_der, test_device_cert_der_len);
    return _nx_secure_x509_certificate_initialize(&ca, ca_der, (USHORT)test_ca_cert_der_len,
                                                  NX_NULL, 0, NX_NULL, 0,
                                                  NX_SECURE_X509_KEY_TYPE_NONE) == NX_SUCCESS &&
           _nx_secure_x509_certificate_initialize(&leaf, leaf_der, (USHORT)test_device_cert_der_len,
                                                  NX_NULL, 0, NX_NULL, 0,
                                                  NX_SECURE_X509_KEY_TYPE_NONE) == NX_SUCCESS;
}

/* ------------------------------------------------------------ contracts -- */

static void init_ceiling(const NX_CRYPTO_METHOD *m)
{
    static UCHAR modulus[600];
    VOID        *handler = NX_NULL;
    UINT         status;
    ULONG        i;
    int          untouched = 1;

    memset(modulus, 0xC3, sizeof(modulus));

    status = m->nx_crypto_init((NX_CRYPTO_METHOD *)m, modulus, 4096, &handler,
                               METADATA, METADATA_SIZE);
    CHECK(status == NX_CRYPTO_SUCCESS, "a 4096-bit modulus is accepted");

    memset(METADATA, 0xA5, 64);
    status = m->nx_crypto_init((NX_CRYPTO_METHOD *)m, modulus, 4096 + 8, &handler,
                               METADATA, METADATA_SIZE);
    CHECK(status == NX_CRYPTO_UNSUPPORTED_KEY_SIZE, "a 4104-bit modulus is refused");
    for (i = 0; i < 64; i++)
        untouched &= (METADATA[i] == 0xA5);
    CHECK(untouched, "and the context is not written");

    status = m->nx_crypto_init((NX_CRYPTO_METHOD *)m, modulus, 0, &handler,
                               METADATA, METADATA_SIZE);
    CHECK(status == NX_CRYPTO_UNSUPPORTED_KEY_SIZE, "a zero-length modulus is refused");
}

/* The leaf's signature, decrypted with the CA's public key: PKCS#1 type 1. */
static UINT verify_into(const NX_CRYPTO_METHOD *m, UCHAR *out, ULONG out_len)
{
    const NX_SECURE_RSA_PUBLIC_KEY *pub = &ca.nx_secure_x509_public_key.rsa_public_key;
    VOID *handler = NX_NULL;
    UINT  status;

    status = m->nx_crypto_init((NX_CRYPTO_METHOD *)m,
                               (UCHAR *)pub->nx_secure_rsa_public_modulus,
                               (NX_CRYPTO_KEY_SIZE)(pub->nx_secure_rsa_public_modulus_length << 3),
                               &handler, METADATA, METADATA_SIZE);
    if (status != NX_CRYPTO_SUCCESS)
        return status;

    return m->nx_crypto_operation(NX_CRYPTO_DECRYPT, handler, (NX_CRYPTO_METHOD *)m,
                                  (UCHAR *)pub->nx_secure_rsa_public_exponent,
                                  (NX_CRYPTO_KEY_SIZE)(pub->nx_secure_rsa_public_exponent_length << 3),
                                  (UCHAR *)leaf.nx_secure_x509_signature_data,
                                  leaf.nx_secure_x509_signature_data_length,
                                  NX_NULL, out, out_len, METADATA, METADATA_SIZE, NX_NULL, NX_NULL);
}

static void output_capacity(const NX_CRYPTO_METHOD *m)
{
    const UINT mod = ca.nx_secure_x509_public_key.rsa_public_key.nx_secure_rsa_public_modulus_length;
    static UCHAR out[1024];
    UINT  status, i;
    int   canary = 1;

    /* Exactly the modulus: a valid operation, unchanged. */
    memset(out, 0, sizeof(out));
    status = verify_into(m, out, mod);
    CHECK(status == NX_CRYPTO_SUCCESS, "an output exactly the modulus long works");
    CHECK(out[0] == 0x00 && out[1] == 0x01 && out[2] == 0xFF,
          "and holds the PKCS#1 block of the real signature");

    /* One byte short: refused, nothing written past it.  The array goes on,
       so the old code's full-length write lands in the canary, not beyond. */
    memset(out, 0x5C, sizeof(out));
    status = verify_into(m, out, mod - 1);
    CHECK(status == NX_CRYPTO_INVALID_BUFFER_SIZE, "an output one byte short is refused");
    for (i = mod - 1; i < mod + 16; i++)
        canary &= (out[i] == 0x5C);
    CHECK(canary, "and nothing is written at or past its end");
}

/* An exponent whose significant bytes exceed the number carved for it:
   setup() refuses it, and that refusal must come back. */
static void exponent_overlong(const NX_CRYPTO_METHOD *m)
{
    const NX_SECURE_RSA_PUBLIC_KEY *pub = &ca.nx_secure_x509_public_key.rsa_public_key;
    static UCHAR exponent[512];
    static UCHAR out[1024];
    VOID *handler = NX_NULL;
    UINT  status;

    memset(exponent, 0x11, sizeof(exponent));
    status = m->nx_crypto_init((NX_CRYPTO_METHOD *)m,
                               (UCHAR *)pub->nx_secure_rsa_public_modulus,
                               (NX_CRYPTO_KEY_SIZE)(pub->nx_secure_rsa_public_modulus_length << 3),
                               &handler, METADATA, METADATA_SIZE);
    CHECK(status == NX_CRYPTO_SUCCESS, "init for the overlong exponent");

    status = m->nx_crypto_operation(NX_CRYPTO_DECRYPT, handler, (NX_CRYPTO_METHOD *)m,
                                    exponent,
                                    (NX_CRYPTO_KEY_SIZE)((pub->nx_secure_rsa_public_modulus_length + 8) << 3),
                                    (UCHAR *)leaf.nx_secure_x509_signature_data,
                                    leaf.nx_secure_x509_signature_data_length,
                                    NX_NULL, out, sizeof(out), METADATA, METADATA_SIZE,
                                    NX_NULL, NX_NULL);
    CHECK(status == NX_CRYPTO_SIZE_ERROR,
          "an exponent longer than the modulus is an error, not a stale computation");
}

/* The vendored operation directly: the scratch length it is given counts. */
static void scratch_units(void)
{
    const NX_SECURE_RSA_PUBLIC_KEY *pub = &ca.nx_secure_x509_public_key.rsa_public_key;
    const UINT mod = pub->nx_secure_rsa_public_modulus_length;
    static USHORT scratch[4096];
    static UCHAR  out[1024];
    UINT status;

    method_name = "_nx_crypto_rsa_operation";

    /* Needs 7 * mod + 8 bytes; say there is one USHORT less than that. */
    status = _nx_crypto_rsa_operation(pub->nx_secure_rsa_public_exponent,
                                      pub->nx_secure_rsa_public_exponent_length,
                                      pub->nx_secure_rsa_public_modulus, mod,
                                      NX_NULL, 0, NX_NULL, 0,
                                      leaf.nx_secure_x509_signature_data,
                                      leaf.nx_secure_x509_signature_data_length,
                                      out, scratch, ((7u * mod + 8u) / sizeof(USHORT)) - 1u);
    CHECK(status == NX_CRYPTO_SIZE_ERROR, "a scratch one USHORT short is refused");

    status = _nx_crypto_rsa_operation(pub->nx_secure_rsa_public_exponent,
                                      pub->nx_secure_rsa_public_exponent_length,
                                      pub->nx_secure_rsa_public_modulus, mod,
                                      NX_NULL, 0, NX_NULL, 0,
                                      leaf.nx_secure_x509_signature_data,
                                      leaf.nx_secure_x509_signature_data_length,
                                      out, scratch, (7u * mod + 8u) / sizeof(USHORT));
    CHECK(status == NX_CRYPTO_SUCCESS, "and exactly enough is used");
}

int main(void)
{
    const NX_CRYPTO_METHOD *ours = ami_rsa();

    (void)unused_key;

    if (!load_certs() || ours == NX_NULL)
    {
        printf("FAIL fixture: certificates or the ami RSA method\n");
        return 1;
    }

    method_name = "crypto_method_rsa";
    init_ceiling(&crypto_method_rsa);
    output_capacity(&crypto_method_rsa);
    exponent_overlong(&crypto_method_rsa);

    method_name = "ami_crypto_method_rsa";
    init_ceiling(ours);
    output_capacity(ours);
    exponent_overlong(ours);

    scratch_units();

    printf("tls_rsa_bounds: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
