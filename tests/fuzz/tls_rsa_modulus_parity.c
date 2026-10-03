/*
 * N-171: ami_crypto_method_rsa refuses an even modulus -- zero above all --
 * before any number is set up or any exponentiation runs.  The certificate
 * parser accepts any modulus bytes; crypto68k refuses an even one and hands
 * it to the vendored arithmetic, whose modulus scan walks below its buffer
 * for zero.
 *
 * The four exponentiation entry points the method can reach are wrapped at
 * link time (-Wl,--wrap): accelerated and reference, public and CRT.  Each
 * wrapper counts its calls and runs the real routine only for an odd
 * modulus; for an even one it returns a defined zero instead.  So no even
 * modulus ever reaches real arithmetic, and the same program runs safely
 * against the method without the check -- there the wrappers are reached and
 * the calls report success, which is what fails.
 *
 * Every refused case is checked for the status, for no exponentiation call
 * and for an output buffer left exactly as it was.  The controls are real
 * keys from tests/tls in both arithmetic modes: the leaf's signature
 * verified with the CA's public key, and a private-key operation through the
 * CRT with the device key's primes that its public key inverts.
 *
 * 32-BIT ONLY, for tls_rsa_key_regression's reason (tests/fuzz/CMakeLists.txt).
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include "nx_secure_tls.h"
#include "nx_secure_x509.h"
#include "nx_crypto_rsa.h"
#include "ami_tls_crypto.h"
#include "crypto68k.h"
#include "tls.h"

#include "tls_test_certs.h"

/* tls.library's timer, which the crypto module reads for its counters. */
BOOL  ami_tls_timer_is_open(VOID)          { return 0; }
ULONG ami_tls_eclock(VOID)                 { return 0; }
ULONG ami_tls_eclock_micros(ULONG ticks)   { return ticks; }

static int checks;
static int failures;
static const char *mode_name = "";

#define CHECK(c, what) do { checks++; if (!(c)) { failures++; \
    printf("FAIL %s: %s\n", mode_name, what); } } while (0)

/* ------------------------------------------------- the wrapped routines -- */

static int h_calls;             /* exponentiations reached */

static int h_even(const NX_CRYPTO_HUGE_NUMBER *m)
{
    return (m -> nx_crypto_huge_number_size == 0) ||
           ((m -> nx_crypto_huge_number_data[0] & 1u) == 0);
}

/* A defined zero in place of the result real arithmetic would have made. */
static void h_zero(NX_CRYPTO_HUGE_NUMBER *result)
{
    result -> nx_crypto_huge_number_data[0]     = 0;
    result -> nx_crypto_huge_number_size        = 1;
    result -> nx_crypto_huge_number_is_negative = NX_CRYPTO_FALSE;
}

VOID __real_c68k_huge_number_mont_power_modulus(NX_CRYPTO_HUGE_NUMBER *x, NX_CRYPTO_HUGE_NUMBER *e,
                                                NX_CRYPTO_HUGE_NUMBER *m, NX_CRYPTO_HUGE_NUMBER *result,
                                                HN_UBASE *scratch, UINT scratch_limbs);
VOID __wrap_c68k_huge_number_mont_power_modulus(NX_CRYPTO_HUGE_NUMBER *x, NX_CRYPTO_HUGE_NUMBER *e,
                                                NX_CRYPTO_HUGE_NUMBER *m, NX_CRYPTO_HUGE_NUMBER *result,
                                                HN_UBASE *scratch, UINT scratch_limbs)
{
    h_calls++;
    if (h_even(m))
        h_zero(result);
    else
        __real_c68k_huge_number_mont_power_modulus(x, e, m, result, scratch, scratch_limbs);
}

VOID __real_c68k_crt_power_modulus(NX_CRYPTO_HUGE_NUMBER *x, NX_CRYPTO_HUGE_NUMBER *e,
                                   NX_CRYPTO_HUGE_NUMBER *p, NX_CRYPTO_HUGE_NUMBER *q,
                                   NX_CRYPTO_HUGE_NUMBER *m, NX_CRYPTO_HUGE_NUMBER *result,
                                   HN_UBASE *scratch, HN_UBASE *powm_scratch, UINT powm_scratch_limbs);
VOID __wrap_c68k_crt_power_modulus(NX_CRYPTO_HUGE_NUMBER *x, NX_CRYPTO_HUGE_NUMBER *e,
                                   NX_CRYPTO_HUGE_NUMBER *p, NX_CRYPTO_HUGE_NUMBER *q,
                                   NX_CRYPTO_HUGE_NUMBER *m, NX_CRYPTO_HUGE_NUMBER *result,
                                   HN_UBASE *scratch, HN_UBASE *powm_scratch, UINT powm_scratch_limbs)
{
    h_calls++;
    if (h_even(m))
        h_zero(result);
    else
        __real_c68k_crt_power_modulus(x, e, p, q, m, result, scratch, powm_scratch, powm_scratch_limbs);
}

VOID __real__nx_crypto_huge_number_mont_power_modulus(NX_CRYPTO_HUGE_NUMBER *x, NX_CRYPTO_HUGE_NUMBER *e,
                                                      NX_CRYPTO_HUGE_NUMBER *m,
                                                      NX_CRYPTO_HUGE_NUMBER *result, HN_UBASE *scratch);
VOID __wrap__nx_crypto_huge_number_mont_power_modulus(NX_CRYPTO_HUGE_NUMBER *x, NX_CRYPTO_HUGE_NUMBER *e,
                                                      NX_CRYPTO_HUGE_NUMBER *m,
                                                      NX_CRYPTO_HUGE_NUMBER *result, HN_UBASE *scratch)
{
    h_calls++;
    if (h_even(m))
        h_zero(result);
    else
        __real__nx_crypto_huge_number_mont_power_modulus(x, e, m, result, scratch);
}

VOID __real__nx_crypto_huge_number_crt_power_modulus(NX_CRYPTO_HUGE_NUMBER *x, NX_CRYPTO_HUGE_NUMBER *e,
                                                     NX_CRYPTO_HUGE_NUMBER *p, NX_CRYPTO_HUGE_NUMBER *q,
                                                     NX_CRYPTO_HUGE_NUMBER *m,
                                                     NX_CRYPTO_HUGE_NUMBER *result, HN_UBASE *scratch);
VOID __wrap__nx_crypto_huge_number_crt_power_modulus(NX_CRYPTO_HUGE_NUMBER *x, NX_CRYPTO_HUGE_NUMBER *e,
                                                     NX_CRYPTO_HUGE_NUMBER *p, NX_CRYPTO_HUGE_NUMBER *q,
                                                     NX_CRYPTO_HUGE_NUMBER *m,
                                                     NX_CRYPTO_HUGE_NUMBER *result, HN_UBASE *scratch)
{
    h_calls++;
    if (h_even(m))
        h_zero(result);
    else
        __real__nx_crypto_huge_number_crt_power_modulus(x, e, p, q, m, result, scratch);
}

/* ------------------------------------------------------------ fixture -- */

static ULONG metadata_words[32768 / sizeof(ULONG)];
#define METADATA       ((UCHAR *)metadata_words)
#define METADATA_SIZE  ((ULONG)sizeof(metadata_words))

static const NX_CRYPTO_METHOD *rsa;

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

static NX_SECURE_X509_CERT ca, leaf, device;
static UCHAR               ca_der[2048], leaf_der[2048], device_der[2048], device_key[4096];

static int load_certs(void)
{
    memcpy(ca_der, test_ca_cert_der, test_ca_cert_der_len);
    memcpy(leaf_der, test_device_cert_der, test_device_cert_der_len);
    memcpy(device_der, test_device_cert_der, test_device_cert_der_len);
    memcpy(device_key, test_device_cert_key_der, test_device_cert_key_der_len);
    return _nx_secure_x509_certificate_initialize(&ca, ca_der, (USHORT)test_ca_cert_der_len,
                                                  NX_NULL, 0, NX_NULL, 0,
                                                  NX_SECURE_X509_KEY_TYPE_NONE) == NX_SUCCESS &&
           _nx_secure_x509_certificate_initialize(&leaf, leaf_der, (USHORT)test_device_cert_der_len,
                                                  NX_NULL, 0, NX_NULL, 0,
                                                  NX_SECURE_X509_KEY_TYPE_NONE) == NX_SUCCESS &&
           _nx_secure_x509_certificate_initialize(&device, device_der, (USHORT)test_device_cert_der_len,
                                                  NX_NULL, 0, device_key,
                                                  (USHORT)test_device_cert_key_der_len,
                                                  NX_SECURE_X509_KEY_TYPE_RSA_PKCS1_DER) == NX_SUCCESS;
}

/* One operation: init with the modulus, the primes if given, then the
   exponentiation (NX_CRYPTO_DECRYPT is the raw x^e mod m either way). */
static UINT rsa_op(const UCHAR *modulus, UINT modulus_length,
                   const UCHAR *exponent, UINT exponent_length,
                   const UCHAR *p, UINT p_length, const UCHAR *q, UINT q_length,
                   const UCHAR *in, UINT in_length, UCHAR *out, ULONG out_length)
{
    VOID *handler = NX_NULL;
    UINT  status;

    status = rsa -> nx_crypto_init((NX_CRYPTO_METHOD *)rsa, (UCHAR *)modulus,
                                   (NX_CRYPTO_KEY_SIZE)(modulus_length << 3),
                                   &handler, METADATA, METADATA_SIZE);
    if ((status == NX_CRYPTO_SUCCESS) && (p != NX_NULL))
        status = rsa -> nx_crypto_operation(NX_CRYPTO_SET_PRIME_P, handler, (NX_CRYPTO_METHOD *)rsa,
                                            NX_NULL, 0, (UCHAR *)p, p_length, NX_NULL, NX_NULL, 0,
                                            METADATA, METADATA_SIZE, NX_NULL, NX_NULL);
    if ((status == NX_CRYPTO_SUCCESS) && (q != NX_NULL))
        status = rsa -> nx_crypto_operation(NX_CRYPTO_SET_PRIME_Q, handler, (NX_CRYPTO_METHOD *)rsa,
                                            NX_NULL, 0, (UCHAR *)q, q_length, NX_NULL, NX_NULL, 0,
                                            METADATA, METADATA_SIZE, NX_NULL, NX_NULL);
    if (status == NX_CRYPTO_SUCCESS)
        status = rsa -> nx_crypto_operation(NX_CRYPTO_DECRYPT, handler, (NX_CRYPTO_METHOD *)rsa,
                                            (UCHAR *)exponent, (NX_CRYPTO_KEY_SIZE)(exponent_length << 3),
                                            (UCHAR *)in, in_length, NX_NULL, out, out_length,
                                            METADATA, METADATA_SIZE, NX_NULL, NX_NULL);
    if (rsa -> nx_crypto_cleanup)
        (void)rsa -> nx_crypto_cleanup(METADATA);
    return status;
}

#define H_CANARY  0xA7
#define H_MAX     512

static int all_canary(const UCHAR *b, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
        if (b[i] != H_CANARY)
            return 0;
    return 1;
}

/* --------------------------------------------------------------- cases -- */

static void refused(const char *name, const UCHAR *modulus, UINT modulus_length, int crt)
{
    const NX_SECURE_RSA_PRIVATE_KEY *priv = &device.nx_secure_x509_private_key.rsa_private_key;
    const NX_SECURE_RSA_PUBLIC_KEY  *pub  = &ca.nx_secure_x509_public_key.rsa_public_key;
    static UCHAR out[H_MAX + 16];
    static UCHAR in[H_MAX];
    UINT  status;
    char  what[160];

    memset(out, H_CANARY, sizeof(out));
    memset(in, 0x11, sizeof(in));
    in[0] = 0x00;
    h_calls = 0;

    if (crt)
        status = rsa_op(modulus, modulus_length,
                        priv -> nx_secure_rsa_private_exponent, priv -> nx_secure_rsa_private_exponent_length,
                        priv -> nx_secure_rsa_private_prime_p, priv -> nx_secure_rsa_private_prime_p_length,
                        priv -> nx_secure_rsa_private_prime_q, priv -> nx_secure_rsa_private_prime_q_length,
                        in, modulus_length, out, modulus_length);
    else
        status = rsa_op(modulus, modulus_length,
                        pub -> nx_secure_rsa_public_exponent, pub -> nx_secure_rsa_public_exponent_length,
                        NX_NULL, 0, NX_NULL, 0, in, modulus_length, out, modulus_length);

    snprintf(what, sizeof(what), "N-171 %s: status 0x%x, want NX_CRYPTO_INVALID_KEY", name, status);
    CHECK(status == NX_CRYPTO_INVALID_KEY, what);
    snprintf(what, sizeof(what), "N-171 %s: %d exponentiations reached, want 0", name, h_calls);
    CHECK(h_calls == 0, what);
    snprintf(what, sizeof(what), "N-171 %s: the output is untouched", name);
    CHECK(all_canary(out, sizeof(out)), what);
}

/* The leaf's signature, decrypted with the CA's public key: PKCS#1 type 1. */
static void control_public(void)
{
    const NX_SECURE_RSA_PUBLIC_KEY *pub = &ca.nx_secure_x509_public_key.rsa_public_key;
    static UCHAR out[H_MAX];
    UINT status;

    h_calls = 0;
    status = rsa_op(pub -> nx_secure_rsa_public_modulus, pub -> nx_secure_rsa_public_modulus_length,
                    pub -> nx_secure_rsa_public_exponent, pub -> nx_secure_rsa_public_exponent_length,
                    NX_NULL, 0, NX_NULL, 0,
                    leaf.nx_secure_x509_signature_data, leaf.nx_secure_x509_signature_data_length,
                    out, pub -> nx_secure_rsa_public_modulus_length);
    CHECK(status == NX_CRYPTO_SUCCESS && out[0] == 0x00 && out[1] == 0x01 && out[2] == 0xFF,
          "control: a real signature verifies with the CA's odd modulus");
    CHECK(h_calls == 1, "control: through one exponentiation");
}

/* Through the CRT with the device key's primes, then back with its public key. */
static void control_crt(void)
{
    const NX_SECURE_RSA_PRIVATE_KEY *priv = &device.nx_secure_x509_private_key.rsa_private_key;
    const NX_SECURE_RSA_PUBLIC_KEY  *pub  = &device.nx_secure_x509_public_key.rsa_public_key;
    const UINT   n = pub -> nx_secure_rsa_public_modulus_length;
    static UCHAR in[H_MAX], sig[H_MAX], back[H_MAX];
    UINT status;

    memset(in, 0x11, sizeof(in));
    in[0] = 0x00;
    in[1] = 0x01;

    h_calls = 0;
    status = rsa_op(pub -> nx_secure_rsa_public_modulus, n,
                    priv -> nx_secure_rsa_private_exponent, priv -> nx_secure_rsa_private_exponent_length,
                    priv -> nx_secure_rsa_private_prime_p, priv -> nx_secure_rsa_private_prime_p_length,
                    priv -> nx_secure_rsa_private_prime_q, priv -> nx_secure_rsa_private_prime_q_length,
                    in, n, sig, n);
    /* The accelerated CRT's two half-size powers are wrapped calls too. */
    CHECK(status == NX_CRYPTO_SUCCESS && h_calls >= 1,
          "control: the device key's private operation runs through the CRT");

    status = rsa_op(pub -> nx_secure_rsa_public_modulus, n,
                    pub -> nx_secure_rsa_public_exponent, pub -> nx_secure_rsa_public_exponent_length,
                    NX_NULL, 0, NX_NULL, 0, sig, n, back, n);
    CHECK(status == NX_CRYPTO_SUCCESS && memcmp(back, in, n) == 0,
          "control: and its public key inverts it");
}

static void run_mode(void)
{
    const NX_SECURE_RSA_PUBLIC_KEY *ca_pub  = &ca.nx_secure_x509_public_key.rsa_public_key;
    const NX_SECURE_RSA_PUBLIC_KEY *dev_pub = &device.nx_secure_x509_public_key.rsa_public_key;
    static UCHAR zero[256];
    static UCHAR even[H_MAX];
    const UINT   n = dev_pub -> nx_secure_rsa_public_modulus_length;

    control_public();
    control_crt();

    /* What the certificate parser lets through: 0x00, then 128 zero bytes. */
    memset(zero, 0, sizeof(zero));
    refused("zero modulus, public", zero, 128, 0);
    refused("zero modulus, CRT", zero, 128, 1);

    /* A real modulus with its low bit cleared: even, not zero. */
    memcpy(even, ca_pub -> nx_secure_rsa_public_modulus, ca_pub -> nx_secure_rsa_public_modulus_length);
    even[ca_pub -> nx_secure_rsa_public_modulus_length - 1] &= (UCHAR)~1u;
    refused("even CA modulus, public", even, ca_pub -> nx_secure_rsa_public_modulus_length, 0);

    memcpy(even, dev_pub -> nx_secure_rsa_public_modulus, n);
    even[n - 1] &= (UCHAR)~1u;
    refused("even device modulus, CRT", even, n, 1);
}

int main(void)
{
    rsa = ami_rsa();
    if (!load_certs() || rsa == NX_NULL)
    {
        printf("FAIL fixture: certificates or the ami RSA method\n");
        return 1;
    }

    ami_tls_crypto_set_crt(1);

    mode_name = "accelerated";
    ami_tls_crypto_set_arithmetic(AMI_TLS_ARITH_C68K);
    run_mode();

    mode_name = "reference";
    ami_tls_crypto_set_arithmetic(AMI_TLS_ARITH_REFERENCE);
    run_mode();

    printf("tls_rsa_modulus_parity: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
