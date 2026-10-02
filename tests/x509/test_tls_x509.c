/*
 * AmiNetXDuo, host unit tests for the certificate and signature checks.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stddef.h>
#include <string.h>

#include "nx_secure_tls.h"
#include "nx_secure_x509.h"
#include "nx_crypto_rsa.h"
#include "nx_crypto_sha2.h"
#include "nx_crypto_sha5.h"
#include "nx_crypto_ecdsa.h"
#include "nx_crypto_aes.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "tls_test_certs.h"
#pragma GCC diagnostic pop

#include "tls_root_isrg_x1.h"
#include "x509_test_vectors.h"
#include "x509_ext_vectors.h"
#include "x509_pss_vectors.h"

/* _nx_secure_tls_protection is defined by nx_secure_tls_initialize.c, which
   the key-schedule case pulls in through _nx_secure_tls_session_create; a
   second definition here is a duplicate symbol on GNU ld (N106). */

extern NX_SECURE_X509_CRYPTO _nx_crypto_x509_cipher_lookup_table[];
extern const UINT            _nx_crypto_x509_cipher_lookup_table_size;
extern NX_CRYPTO_METHOD      crypto_method_ecdsa;
extern NX_CRYPTO_METHOD      crypto_method_ec_secp256;
extern NX_CRYPTO_METHOD      crypto_method_rsa;
extern NX_CRYPTO_METHOD      crypto_method_sha256;
extern NX_CRYPTO_METHOD      crypto_method_sha384;
extern NX_CRYPTO_METHOD      crypto_method_sha512;
extern const NX_SECURE_TLS_CRYPTO nx_crypto_tls_ciphers_ecc;

static int failures = 0;

static void check(int ok, const char *what)
{
    printf("  %-46s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
    {
        failures++;
    }
}

#define EM_SIZE     256

static const unsigned char digest_info_sha256[] = {
    0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03,
    0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20,
    /* the hash follows, 32 bytes */
};

static unsigned char test_hash[32];

/* Build a well-formed block, then let each case damage one thing about it. */
static unsigned em_build(unsigned char *em, unsigned padding_bytes)
{
    unsigned info_length = (unsigned)sizeof(digest_info_sha256) + 32u;
    unsigned i;
    unsigned at;

    memset(em, 0, EM_SIZE);
    em[0] = 0x00;
    em[1] = 0x01;

    for (i = 0; i < padding_bytes; i++)
    {
        em[2 + i] = 0xFF;
    }

    at = 2 + padding_bytes;
    em[at] = 0x00;
    at++;

    memcpy(&em[at], digest_info_sha256, sizeof(digest_info_sha256));
    memcpy(&em[at + sizeof(digest_info_sha256)], test_hash, 32);

    return at + info_length;
}

/* The padding count that makes the DigestInfo end exactly at EM_SIZE. */
#define EM_FULL_PADDING (EM_SIZE - 3u - (unsigned)sizeof(digest_info_sha256) - 32u)

static UINT em_decode(const unsigned char *em)
{
const UCHAR *oid;
UINT         oid_length;
const UCHAR *hash;
UINT         hash_length;

    return(_nx_secure_x509_pkcs7_decode(em, EM_SIZE, &oid, &oid_length, &hash, &hash_length));
}

static void test_pkcs1(void)
{
unsigned char em[EM_SIZE];
const UCHAR  *oid;
UINT          oid_length;
const UCHAR  *hash;
UINT          hash_length;
UINT          status;
unsigned      i;

    printf("pkcs1\n");

    for (i = 0; i < 32; i++)
    {
        test_hash[i] = (unsigned char)(0xA0 + i);
    }

    /* The one that has to keep working. */
    (void)em_build(em, EM_FULL_PADDING);
    status = _nx_secure_x509_pkcs7_decode(em, EM_SIZE, &oid, &oid_length, &hash, &hash_length);
    check(status == NX_SECURE_X509_SUCCESS, "a conforming block still decodes");
    check(status == NX_SECURE_X509_SUCCESS && hash_length == 32 &&
          memcmp(hash, test_hash, 32) == 0, "and yields the hash it carries");

    (void)em_build(em, EM_FULL_PADDING);
    em[0] = 0x01;
    check(em_decode(em) != NX_SECURE_X509_SUCCESS, "leading byte is not 0x00");

    (void)em_build(em, EM_FULL_PADDING);
    em[1] = 0x00;
    check(em_decode(em) != NX_SECURE_X509_SUCCESS, "block type 0 (ambiguous padding)");

    (void)em_build(em, EM_FULL_PADDING);
    em[1] = 0x02;
    check(em_decode(em) != NX_SECURE_X509_SUCCESS, "block type 2 (encryption)");

    (void)em_build(em, EM_FULL_PADDING);
    em[1] = 0x37;
    check(em_decode(em) != NX_SECURE_X509_SUCCESS, "block type 0x37");

    (void)em_build(em, EM_FULL_PADDING);
    em[40] = 0xAB;
    check(em_decode(em) != NX_SECURE_X509_SUCCESS, "a padding byte that is not 0xFF");

    (void)em_build(em, 3);
    check(em_decode(em) != NX_SECURE_X509_SUCCESS, "fewer than eight padding bytes");

    (void)em_build(em, 8);
    for (i = 62; i < EM_SIZE; i++)
    {
        em[i] = (unsigned char)i;
    }
    check(em_decode(em) != NX_SECURE_X509_SUCCESS, "data after the DigestInfo");

    /* One byte inside the DigestInfo sequence, past the hash. */
    (void)em_build(em, EM_FULL_PADDING - 1u);
    em[2 + (EM_FULL_PADDING - 1u) + 1u + 1u] += 1;  /* sequence length */
    em[EM_SIZE - 1u] = 0x00;
    check(em_decode(em) != NX_SECURE_X509_SUCCESS, "data after the hash, inside the sequence");
}

static union
{
    NX_CRYPTO_ECDSA ecdsa;
    ULONG           align;
} ecdsa_metadata;

static UINT ecdsa_verify(const unsigned char *sig, unsigned sig_length)
{
VOID                *handler = NX_CRYPTO_NULL;
UINT                 status;

    memset(&ecdsa_metadata, 0, sizeof(ecdsa_metadata));

    status = crypto_method_ecdsa.nx_crypto_init(&crypto_method_ecdsa,
                                                (UCHAR *)x509_p256_pubkey,
                                                (NX_CRYPTO_KEY_SIZE)(x509_p256_pubkey_len << 3),
                                                &handler,
                                                &ecdsa_metadata, sizeof(ecdsa_metadata));
    if (status != NX_CRYPTO_SUCCESS)
    {
        return(status);
    }

    status = crypto_method_ecdsa.nx_crypto_operation(NX_CRYPTO_EC_CURVE_SET, handler,
                                                     &crypto_method_ecdsa, NX_CRYPTO_NULL, 0,
                                                     (UCHAR *)&crypto_method_ec_secp256,
                                                     sizeof(NX_CRYPTO_METHOD *),
                                                     NX_CRYPTO_NULL, NX_CRYPTO_NULL, 0,
                                                     &ecdsa_metadata, sizeof(ecdsa_metadata),
                                                     NX_CRYPTO_NULL, NX_CRYPTO_NULL);
    if (status != NX_CRYPTO_SUCCESS)
    {
        return(status);
    }

    return(crypto_method_ecdsa.nx_crypto_operation(NX_CRYPTO_VERIFY, handler,
                                                   &crypto_method_ecdsa,
                                                   (UCHAR *)x509_p256_pubkey,
                                                   (NX_CRYPTO_KEY_SIZE)(x509_p256_pubkey_len << 3),
                                                   (UCHAR *)x509_p256_hash, x509_p256_hash_len,
                                                   NX_CRYPTO_NULL,
                                                   (UCHAR *)sig, sig_length,
                                                   &ecdsa_metadata, sizeof(ecdsa_metadata),
                                                   NX_CRYPTO_NULL, NX_CRYPTO_NULL));
}

static void test_ecdsa(void)
{
unsigned char sig[128];
unsigned      len;

    printf("ecdsa\n");

#ifdef X509_TEST_ECDSA
    memcpy(sig, x509_p256_sig, x509_p256_sig_len);
    len = x509_p256_sig_len;
    check(ecdsa_verify(sig, len) == NX_CRYPTO_SUCCESS, "a real P-256 signature still verifies");
#else
    (void)len;
    printf("  %-46s %s\n", "a real P-256 signature still verifies", "32-bit only, skipped");
#endif

    /* r's INTEGER tag. Nothing used to look at it. */
    memcpy(sig, x509_p256_sig, x509_p256_sig_len);
    sig[2] = 0x04;
    check(ecdsa_verify(sig, x509_p256_sig_len) != NX_CRYPTO_SUCCESS, "r is tagged INTEGER");

    /* s's INTEGER tag. */
    memcpy(sig, x509_p256_sig, x509_p256_sig_len);
    sig[2 + 2 + sig[3]] = 0x04;
    check(ecdsa_verify(sig, x509_p256_sig_len) != NX_CRYPTO_SUCCESS, "s is tagged INTEGER");

    /* A second leading zero on r: same value, different encoding. */
    memcpy(sig, x509_p256_sig, x509_p256_sig_len);
    memmove(&sig[5], &sig[4], x509_p256_sig_len - 4u);
    sig[4] = 0x00;
    sig[3] = (unsigned char)(sig[3] + 1u);
    sig[1] = (unsigned char)(sig[1] + 1u);
    check(ecdsa_verify(sig, x509_p256_sig_len + 1u) != NX_CRYPTO_SUCCESS,
          "r is minimally encoded");

    /* A byte after s, inside a sequence declared long enough to hold it. */
    memcpy(sig, x509_p256_sig, x509_p256_sig_len);
    sig[x509_p256_sig_len] = 0x00;
    sig[1] = (unsigned char)(sig[1] + 1u);
    check(ecdsa_verify(sig, x509_p256_sig_len + 1u) != NX_CRYPTO_SUCCESS,
          "nothing follows s in the sequence");

    /* A two-byte long form, which used to be read as if it were one. */
    memcpy(&sig[3], x509_p256_sig, x509_p256_sig_len);
    sig[0] = 0x30;
    sig[1] = 0x82;
    sig[2] = 0x00;
    sig[3] = (unsigned char)(x509_p256_sig_len - 2u);
    check(ecdsa_verify(sig, x509_p256_sig_len + 2u) != NX_CRYPTO_SUCCESS,
          "a 0x82 length is not read as 0x81");

    /* Not a sequence at all. */
    memcpy(sig, x509_p256_sig, x509_p256_sig_len);
    sig[0] = 0x31;
    check(ecdsa_verify(sig, x509_p256_sig_len) != NX_CRYPTO_SUCCESS, "the outer tag is SEQUENCE");
}

/* sha256WithRSAEncryption, 1.2.840.113549.1.1.11, as it appears in DER. */
static const unsigned char oid_sha256_rsa[] = {
    0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0b
};

static void test_sigalg(void)
{
static unsigned char  copy[8192];
NX_SECURE_X509_CERT   cert;
UINT                  bytes;
UINT                  status;
unsigned              i;
unsigned              last = 0;
unsigned              found = 0;

    printf("sigalg\n");

    memset(&cert, 0, sizeof(cert));
    status = _nx_secure_x509_certificate_parse(test_device_cert_der,
                                               test_device_cert_der_len, &bytes, &cert);
    check(status == NX_SECURE_X509_SUCCESS, "the sample leaf still parses");

    memset(&cert, 0, sizeof(cert));
    status = _nx_secure_x509_certificate_parse(test_ca_cert_der,
                                               test_ca_cert_der_len, &bytes, &cert);
    check(status == NX_SECURE_X509_SUCCESS, "the sample CA still parses");

    memset(&cert, 0, sizeof(cert));
    status = _nx_secure_x509_certificate_parse(isrg_root_x1_der,
                                               isrg_root_x1_der_len, &bytes, &cert);
    check(status == NX_SECURE_X509_SUCCESS, "ISRG Root X1 still parses");

    memcpy(copy, test_device_cert_der, test_device_cert_der_len);

    for (i = 0; i + sizeof(oid_sha256_rsa) <= test_device_cert_der_len; i++)
    {
        if (memcmp(&copy[i], oid_sha256_rsa, sizeof(oid_sha256_rsa)) == 0)
        {
            last = i;
            found++;
        }
    }

    check(found == 2, "the identifier appears exactly twice");

    copy[last + sizeof(oid_sha256_rsa) - 1u] = 0x05;    /* ...1.1.5, sha1WithRSA */

    memset(&cert, 0, sizeof(cert));
    status = _nx_secure_x509_certificate_parse(copy, test_device_cert_der_len, &bytes, &cert);
    check(status == NX_SECURE_X509_SIGNATURE_ALGORITHM_MISMATCH,
          "the outer identifier cannot disagree with the inner");
}

static void test_modulus(void)
{
static unsigned char copy[8192];
NX_SECURE_X509_CERT  cert;
UINT                 bytes;
UINT                 status;
unsigned             i;
unsigned             at = 0;

    printf("modulus\n");

    memcpy(copy, test_device_cert_der, test_device_cert_der_len);

    for (i = 0; i + 4 <= test_device_cert_der_len; i++)
    {
        if (copy[i] == 0x02 && copy[i + 1] == 0x82 &&
            copy[i + 2] == 0x01 && copy[i + 3] == 0x01)
        {
            at = i;
            break;
        }
    }

    check(at != 0, "the leaf carries a 2048-bit modulus");

    memset(&cert, 0, sizeof(cert));
    status = _nx_secure_x509_certificate_parse(copy, test_device_cert_der_len, &bytes, &cert);
    check(status == NX_SECURE_X509_SUCCESS, "and it parses untouched");

    copy[at + 1] = 0x41;    /* short form, 65 bytes */
    memmove(&copy[at + 2], &copy[at + 4], test_device_cert_der_len - (at + 4));

    memset(&cert, 0, sizeof(cert));
    status = _nx_secure_x509_certificate_parse(copy, test_device_cert_der_len, &bytes, &cert);
    check(status != NX_SECURE_X509_SUCCESS, "a 512-bit modulus is refused");
}

static union
{
    NX_CRYPTO_RSA rsa;
    ULONG         align;
} chain_pubkey_metadata;

/* SHA-512 as well as SHA-256: the PSS section verifies a SHA-384 signature,
   and nx_crypto's SHA-384 runs in the SHA-512 metadata block, which is the
   larger of the two. */
static union
{
    NX_CRYPTO_SHA256 sha256;
    NX_CRYPTO_SHA512 sha512;
    ULONG            align;
} chain_hash_metadata;

static void chain_arm(NX_SECURE_X509_CERT *cert)
{
    cert -> nx_secure_x509_cipher_table      = _nx_crypto_x509_cipher_lookup_table;
    cert -> nx_secure_x509_cipher_table_size = _nx_crypto_x509_cipher_lookup_table_size;

    cert -> nx_secure_x509_public_cipher_metadata_area = (VOID *)&chain_pubkey_metadata;
    cert -> nx_secure_x509_public_cipher_metadata_size = sizeof(chain_pubkey_metadata);

    cert -> nx_secure_x509_hash_metadata_area = (VOID *)&chain_hash_metadata;
    cert -> nx_secure_x509_hash_metadata_size = sizeof(chain_hash_metadata);
}

static void test_chain(void)
{
static NX_SECURE_X509_CERT              cert_a;
static NX_SECURE_X509_CERT              cert_b;
static NX_SECURE_X509_CERTIFICATE_STORE store;
UINT                                    status;

    printf("chain\n");

    memset(&cert_a, 0, sizeof(cert_a));
    memset(&cert_b, 0, sizeof(cert_b));
    memset(&store, 0, sizeof(store));

    status = _nx_secure_x509_certificate_initialize(&cert_a,
                                                    (UCHAR *)x509_cross_a, (USHORT)x509_cross_a_len,
                                                    NX_CRYPTO_NULL, 0, NX_CRYPTO_NULL, 0,
                                                    NX_SECURE_X509_KEY_TYPE_NONE);
    check(status == NX_SECURE_X509_SUCCESS, "cross-signed A parses");

    status = _nx_secure_x509_certificate_initialize(&cert_b,
                                                    (UCHAR *)x509_cross_b, (USHORT)x509_cross_b_len,
                                                    NX_CRYPTO_NULL, 0, NX_CRYPTO_NULL, 0,
                                                    NX_SECURE_X509_KEY_TYPE_NONE);
    check(status == NX_SECURE_X509_SUCCESS, "cross-signed B parses");

    chain_arm(&cert_a);
    chain_arm(&cert_b);

    (void)_nx_secure_x509_store_certificate_add(&cert_a, &store,
                                                NX_SECURE_X509_CERT_LOCATION_REMOTE);
    (void)_nx_secure_x509_store_certificate_add(&cert_b, &store,
                                                NX_SECURE_X509_CERT_LOCATION_REMOTE);

    status = _nx_secure_x509_certificate_chain_verify(&store, &cert_a, 0);
    check(status == NX_SECURE_X509_CHAIN_TOO_LONG, "a cross-signed cycle terminates");
}

static NX_SECURE_X509_CERT              ext_root;
static NX_SECURE_X509_CERT              ext_int;
static NX_SECURE_X509_CERT              ext_leaf;
static NX_SECURE_X509_CERTIFICATE_STORE ext_store;

static UINT ext_verify(const unsigned char *leaf, unsigned leaf_len,
                       const unsigned char *intermediate, unsigned int_len)
{
UINT status;

    memset(&ext_root,  0, sizeof(ext_root));
    memset(&ext_int,   0, sizeof(ext_int));
    memset(&ext_leaf,  0, sizeof(ext_leaf));
    memset(&ext_store, 0, sizeof(ext_store));

    status = _nx_secure_x509_certificate_initialize(&ext_root,
                                                    (UCHAR *)x509_ext_root, (USHORT)x509_ext_root_len,
                                                    NX_CRYPTO_NULL, 0, NX_CRYPTO_NULL, 0,
                                                    NX_SECURE_X509_KEY_TYPE_NONE);
    if (status != NX_SECURE_X509_SUCCESS)
    {
        return(status);
    }

    status = _nx_secure_x509_certificate_initialize(&ext_leaf,
                                                    (UCHAR *)leaf, (USHORT)leaf_len,
                                                    NX_CRYPTO_NULL, 0, NX_CRYPTO_NULL, 0,
                                                    NX_SECURE_X509_KEY_TYPE_NONE);
    if (status != NX_SECURE_X509_SUCCESS)
    {
        return(status);
    }

    chain_arm(&ext_root);
    chain_arm(&ext_leaf);

    (void)_nx_secure_x509_store_certificate_add(&ext_root, &ext_store,
                                                NX_SECURE_X509_CERT_LOCATION_TRUSTED);
    (void)_nx_secure_x509_store_certificate_add(&ext_leaf, &ext_store,
                                                NX_SECURE_X509_CERT_LOCATION_REMOTE);

    if (intermediate != NX_CRYPTO_NULL)
    {
        status = _nx_secure_x509_certificate_initialize(&ext_int,
                                                        (UCHAR *)intermediate, (USHORT)int_len,
                                                        NX_CRYPTO_NULL, 0, NX_CRYPTO_NULL, 0,
                                                        NX_SECURE_X509_KEY_TYPE_NONE);
        if (status != NX_SECURE_X509_SUCCESS)
        {
            return(status);
        }

        chain_arm(&ext_int);
        (void)_nx_secure_x509_store_certificate_add(&ext_int, &ext_store,
                                                    NX_SECURE_X509_CERT_LOCATION_REMOTE);
    }

    /* current_time 0: expiry is not what is under test here. */
    return(_nx_secure_x509_certificate_chain_verify(&ext_store, &ext_leaf, 0));
}

static void test_extensions(void)
{
UINT status;

    printf("extensions\n");

    status = ext_verify(x509_ext_leaf_ok, x509_ext_leaf_ok_len, NX_CRYPTO_NULL, 0);
    if (status == NX_SECURE_X509_SUCCESS)
    {
        status = _nx_secure_x509_extended_key_usage_chain_check(
            &ext_store, &ext_leaf,
            NX_SECURE_TLS_X509_TYPE_PKIX_KP_SERVER_AUTH);
    }
    check(status == NX_SECURE_X509_SUCCESS,
          "a serverAuth leaf still verifies");

    status = ext_verify(x509_ext_leaf_clientauth, x509_ext_leaf_clientauth_len, NX_CRYPTO_NULL, 0);
    if (status == NX_SECURE_X509_SUCCESS)
    {
        status = _nx_secure_x509_extended_key_usage_chain_check(
            &ext_store, &ext_leaf,
            NX_SECURE_TLS_X509_TYPE_PKIX_KP_SERVER_AUTH);
    }
    check(status == NX_SECURE_X509_EXT_KEY_USAGE_NOT_FOUND,
          "a clientAuth-only leaf is refused");

    status = ext_verify(x509_ext_leaf_critical, x509_ext_leaf_critical_len, NX_CRYPTO_NULL, 0);
    check(status == NX_SECURE_X509_UNSUPPORTED_CRITICAL_EXTENSION,
          "an unhandled critical extension is refused");

    status = ext_verify(x509_ext_leaf_inside, x509_ext_leaf_inside_len,
                        x509_ext_int, x509_ext_int_len);
    check(status == NX_SECURE_X509_SUCCESS,
          "critical extKeyUsage on a CA is accepted");

    status = ext_verify(x509_ext_leaf_outside, x509_ext_leaf_outside_len,
                        x509_ext_int, x509_ext_int_len);
    check(status == NX_SECURE_X509_NAME_CONSTRAINT_VIOLATION,
          "a name outside permittedSubtrees is refused");

    status = ext_verify(x509_ext_leaf_excluded, x509_ext_leaf_excluded_len,
                        x509_ext_int, x509_ext_int_len);
    check(status == NX_SECURE_X509_NAME_CONSTRAINT_VIOLATION,
          "a name inside excludedSubtrees is refused");
}

static NX_SECURE_X509_CERT              pss_root;
static NX_SECURE_X509_CERT              pss_int;
static NX_SECURE_X509_CERT              pss_leaf;
static NX_SECURE_X509_CERTIFICATE_STORE pss_store;

static UINT pss_verify(const unsigned char *leaf, unsigned leaf_len,
                       const unsigned char *intermediate, unsigned int_len)
{
UINT status;

    memset(&pss_root,  0, sizeof(pss_root));
    memset(&pss_int,   0, sizeof(pss_int));
    memset(&pss_leaf,  0, sizeof(pss_leaf));
    memset(&pss_store, 0, sizeof(pss_store));

    status = _nx_secure_x509_certificate_initialize(&pss_root,
                                                    (UCHAR *)x509_pss_root, (USHORT)x509_pss_root_len,
                                                    NX_CRYPTO_NULL, 0, NX_CRYPTO_NULL, 0,
                                                    NX_SECURE_X509_KEY_TYPE_NONE);
    if (status != NX_SECURE_X509_SUCCESS)
    {
        return(status);
    }

    status = _nx_secure_x509_certificate_initialize(&pss_leaf,
                                                    (UCHAR *)leaf, (USHORT)leaf_len,
                                                    NX_CRYPTO_NULL, 0, NX_CRYPTO_NULL, 0,
                                                    NX_SECURE_X509_KEY_TYPE_NONE);
    if (status != NX_SECURE_X509_SUCCESS)
    {
        return(status);
    }

    chain_arm(&pss_root);
    chain_arm(&pss_leaf);

    (void)_nx_secure_x509_store_certificate_add(&pss_root, &pss_store,
                                                NX_SECURE_X509_CERT_LOCATION_TRUSTED);
    (void)_nx_secure_x509_store_certificate_add(&pss_leaf, &pss_store,
                                                NX_SECURE_X509_CERT_LOCATION_REMOTE);

    if (intermediate != NX_CRYPTO_NULL)
    {
        status = _nx_secure_x509_certificate_initialize(&pss_int,
                                                        (UCHAR *)intermediate, (USHORT)int_len,
                                                        NX_CRYPTO_NULL, 0, NX_CRYPTO_NULL, 0,
                                                        NX_SECURE_X509_KEY_TYPE_NONE);
        if (status != NX_SECURE_X509_SUCCESS)
        {
            return(status);
        }

        chain_arm(&pss_int);
        (void)_nx_secure_x509_store_certificate_add(&pss_int, &pss_store,
                                                    NX_SECURE_X509_CERT_LOCATION_REMOTE);
    }

    /* current_time 0: expiry is not what is under test here. */
    return(_nx_secure_x509_certificate_chain_verify(&pss_store, &pss_leaf, 0));
}

static void test_pss(void)
{
static NX_SECURE_X509_CERT cert;
static unsigned char       tampered[4096];
static const unsigned char pss_oid[] =
    {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0a};
UINT                       status;
UINT                       i;
UINT                       oid_count;
UINT                       params_offset;

    printf("pss\n");

    memset(&cert, 0, sizeof(cert));
    status = _nx_secure_x509_certificate_initialize(&cert,
                                                    (UCHAR *)x509_pss_leaf, (USHORT)x509_pss_leaf_len,
                                                    NX_CRYPTO_NULL, 0, NX_CRYPTO_NULL, 0,
                                                    NX_SECURE_X509_KEY_TYPE_NONE);
    check(status == NX_SECURE_X509_SUCCESS, "a PSS-signed certificate parses");
    check(cert.nx_secure_x509_signature_algorithm == NX_SECURE_TLS_X509_TYPE_RSA_PSS_SHA_256,
          "the digest comes out of the PSS parameters");
    check(cert.nx_secure_x509_signature_salt_length == 32,
          "the salt length comes out of the PSS parameters");

    status = pss_verify(x509_pss_leaf, x509_pss_leaf_len, x509_pss_int, x509_pss_int_len);
    check(status == NX_SECURE_X509_SUCCESS, "a PSS chain verifies to its root");

    status = pss_verify(x509_pss_leaf384, x509_pss_leaf384_len, NX_CRYPTO_NULL, 0);
    check(status == NX_SECURE_X509_SUCCESS, "PSS with SHA-384 verifies");

    status = pss_verify(x509_pss_leaf_salt, x509_pss_leaf_salt_len, NX_CRYPTO_NULL, 0);
    check(status == NX_SECURE_X509_SUCCESS, "PSS with a non-digest salt verifies");

    status = pss_verify(x509_psskey_leaf, x509_psskey_leaf_len,
                        NX_CRYPTO_NULL, 0);
    check(status != NX_SECURE_X509_SUCCESS,
          "a PSS-key leaf is not verified under the unrelated RSA root");

    memset(&pss_root,  0, sizeof(pss_root));
    memset(&pss_leaf,  0, sizeof(pss_leaf));
    memset(&pss_store, 0, sizeof(pss_store));
    status = _nx_secure_x509_certificate_initialize(&pss_root,
                                                    (UCHAR *)x509_psskey_root,
                                                    (USHORT)x509_psskey_root_len,
                                                    NX_CRYPTO_NULL, 0, NX_CRYPTO_NULL, 0,
                                                    NX_SECURE_X509_KEY_TYPE_NONE);
    check(status == NX_SECURE_X509_SUCCESS, "an id-RSASSA-PSS public key parses");
    check(pss_root.nx_secure_x509_public_algorithm == NX_SECURE_TLS_X509_TYPE_RSA,
          "a PSS key uses the existing RSA primitive");
    check(pss_root.nx_secure_x509_public_key_identifier == NX_SECURE_TLS_X509_TYPE_RSA_PSS,
          "the PSS-only key policy is preserved");
    check(pss_root.nx_secure_x509_public_key_pss_algorithm ==
              NX_SECURE_TLS_X509_TYPE_RSA_PSS_SHA_256,
          "the PSS-key digest restriction is parsed");
    check(pss_root.nx_secure_x509_public_key_pss_salt_length == 32,
          "the PSS-key minimum salt is parsed");

    oid_count = 0;
    params_offset = 0;
    if (x509_psskey_root_len <= sizeof(tampered))
    {
        memcpy(tampered, x509_psskey_root, x509_psskey_root_len);
        for (i = 0; i + sizeof(pss_oid) < x509_psskey_root_len; i++)
        {
            if (memcmp(&tampered[i], pss_oid, sizeof(pss_oid)) == 0)
            {
                oid_count++;
                if (oid_count == 2)
                {
                    params_offset = i + sizeof(pss_oid);
                    break;
                }
            }
        }

        check(params_offset != 0 && tampered[params_offset] == 0x30,
              "the PSS-key parameter sequence is located");
        if (params_offset != 0 && tampered[params_offset] == 0x30)
        {
            tampered[params_offset] = 0x05;
            memset(&cert, 0, sizeof(cert));
            status = _nx_secure_x509_certificate_initialize(&cert,
                                                            tampered,
                                                            (USHORT)x509_psskey_root_len,
                                                            NX_CRYPTO_NULL, 0,
                                                            NX_CRYPTO_NULL, 0,
                                                            NX_SECURE_X509_KEY_TYPE_NONE);
            check(status != NX_SECURE_X509_SUCCESS,
                  "non-SEQUENCE PSS-key parameters are refused");
        }
    }
    else
    {
        check(0, "the PSS-key root fits the tamper buffer");
    }

    status = _nx_secure_x509_certificate_initialize(&pss_leaf,
                                                    (UCHAR *)x509_psskey_leaf,
                                                    (USHORT)x509_psskey_leaf_len,
                                                    NX_CRYPTO_NULL, 0, NX_CRYPTO_NULL, 0,
                                                    NX_SECURE_X509_KEY_TYPE_NONE);
    if (status == NX_SECURE_X509_SUCCESS)
    {
        chain_arm(&pss_root);
        chain_arm(&pss_leaf);
        (void)_nx_secure_x509_store_certificate_add(&pss_root, &pss_store,
                                                    NX_SECURE_X509_CERT_LOCATION_TRUSTED);
        (void)_nx_secure_x509_store_certificate_add(&pss_leaf, &pss_store,
                                                    NX_SECURE_X509_CERT_LOCATION_REMOTE);
        status = _nx_secure_x509_certificate_chain_verify(&pss_store, &pss_leaf, 0);
    }
    check(status == NX_SECURE_X509_SUCCESS, "a restricted PSS-key chain verifies");

    pss_root.nx_secure_x509_public_key_pss_salt_length = 31;
    status = _nx_secure_x509_certificate_chain_verify(&pss_store, &pss_leaf, 0);
    check(status == NX_SECURE_X509_SUCCESS,
          "a certificate signature may exceed the PSS-key minimum salt");
    pss_root.nx_secure_x509_public_key_pss_salt_length = 32;
    pss_root.nx_secure_x509_public_key_pss_algorithm =
        NX_SECURE_TLS_X509_TYPE_RSA_PSS_SHA_384;
    status = _nx_secure_x509_certificate_chain_verify(&pss_store, &pss_leaf, 0);
    check(status == NX_SECURE_X509_UNSUPPORTED_SIGNATURE_PARAMETERS,
          "a PSS-key digest mismatch is refused");
    pss_root.nx_secure_x509_public_key_pss_algorithm =
        NX_SECURE_TLS_X509_TYPE_RSA_PSS_SHA_256;
    pss_root.nx_secure_x509_public_key_pss_salt_length = 33;
    status = _nx_secure_x509_certificate_chain_verify(&pss_store, &pss_leaf, 0);
    check(status == NX_SECURE_X509_UNSUPPORTED_SIGNATURE_PARAMETERS,
          "a signature below the PSS-key minimum salt is refused");
    pss_root.nx_secure_x509_public_key_pss_salt_length = 32;
    pss_leaf.nx_secure_x509_signature_algorithm = NX_SECURE_TLS_X509_TYPE_RSA_SHA_256;
    status = _nx_secure_x509_certificate_chain_verify(&pss_store, &pss_leaf, 0);
    check(status == NX_SECURE_X509_WRONG_SIGNATURE_METHOD,
          "PKCS#1 use of a PSS-only key is refused");

    memset(&pss_root,  0, sizeof(pss_root));
    memset(&pss_leaf,  0, sizeof(pss_leaf));
    memset(&pss_store, 0, sizeof(pss_store));
    status = _nx_secure_x509_certificate_initialize(&pss_root,
                                                    (UCHAR *)x509_psskey_any_root,
                                                    (USHORT)x509_psskey_any_root_len,
                                                    NX_CRYPTO_NULL, 0, NX_CRYPTO_NULL, 0,
                                                    NX_SECURE_X509_KEY_TYPE_NONE);
    check(status == NX_SECURE_X509_SUCCESS, "a parameterless PSS public key parses");
    check(pss_root.nx_secure_x509_public_key_pss_algorithm == NX_SECURE_TLS_X509_TYPE_UNKNOWN,
          "absent PSS-key parameters remain unrestricted");
    status = _nx_secure_x509_certificate_initialize(&pss_leaf,
                                                    (UCHAR *)x509_psskey_any_leaf,
                                                    (USHORT)x509_psskey_any_leaf_len,
                                                    NX_CRYPTO_NULL, 0, NX_CRYPTO_NULL, 0,
                                                    NX_SECURE_X509_KEY_TYPE_NONE);
    if (status == NX_SECURE_X509_SUCCESS)
    {
        chain_arm(&pss_root);
        chain_arm(&pss_leaf);
        (void)_nx_secure_x509_store_certificate_add(&pss_root, &pss_store,
                                                    NX_SECURE_X509_CERT_LOCATION_TRUSTED);
        (void)_nx_secure_x509_store_certificate_add(&pss_leaf, &pss_store,
                                                    NX_SECURE_X509_CERT_LOCATION_REMOTE);
        status = _nx_secure_x509_certificate_chain_verify(&pss_store, &pss_leaf, 0);
    }
    check(status == NX_SECURE_X509_SUCCESS,
          "an unrestricted PSS key verifies a SHA-384 signature");

    if (x509_pss_leaf_len <= sizeof(tampered))
    {
        memcpy(tampered, x509_pss_leaf, x509_pss_leaf_len);
        tampered[x509_pss_leaf_len - 1] ^= 0x01;

        status = pss_verify(tampered, x509_pss_leaf_len, x509_pss_int, x509_pss_int_len);
        check(status == NX_SECURE_X509_CERTIFICATE_SIG_CHECK_FAILED,
              "a tampered PSS signature is refused");
    }
    else
    {
        check(0, "the PSS leaf fits the tamper buffer");
    }
}

static void test_pss_schemes(void)
{
NX_SECURE_TLS_SESSION session;
NX_SECURE_X509_CRYPTO method;
NX_SECURE_X509_CERT   certificate;
UCHAR                  verify_message[4];
USHORT                 rsae;
USHORT                 pss;
USHORT                 legacy;
UINT                   status;

    printf("pss schemes\n");

    memset(&session, 0, sizeof(session));
    memset(&method, 0, sizeof(method));
    session.nx_secure_tls_1_3 = 1;
    method.nx_secure_x509_public_cipher_method = &crypto_method_rsa;

    method.nx_secure_x509_hash_method = &crypto_method_sha256;
    _nx_secure_tls_get_signature_algorithm(&session, &method, &rsae, &pss, &legacy);
    check(rsae == 0x0804u && pss == 0x0809u && legacy == NX_SECURE_TLS_SIGNATURE_RSA_SHA256,
          "SHA-256 advertises RSAE, PSS-key and TLS 1.2 schemes");

    method.nx_secure_x509_hash_method = &crypto_method_sha384;
    _nx_secure_tls_get_signature_algorithm(&session, &method, &rsae, &pss, &legacy);
    check(rsae == 0x0805u && pss == 0x080au && legacy == NX_SECURE_TLS_SIGNATURE_RSA_SHA384,
          "SHA-384 advertises both PSS key encodings");

    method.nx_secure_x509_hash_method = &crypto_method_sha512;
    _nx_secure_tls_get_signature_algorithm(&session, &method, &rsae, &pss, &legacy);
    check(rsae == 0x0806u && pss == 0x080bu && legacy == NX_SECURE_TLS_SIGNATURE_RSA_SHA512,
          "SHA-512 advertises both PSS key encodings");

    memset(&session, 0, sizeof(session));
    memset(&certificate, 0, sizeof(certificate));
    status = _nx_secure_x509_certificate_initialize(&certificate,
                                                    (UCHAR *)x509_psskey_root,
                                                    (USHORT)x509_psskey_root_len,
                                                    NX_CRYPTO_NULL, 0, NX_CRYPTO_NULL, 0,
                                                    NX_SECURE_X509_KEY_TYPE_NONE);
    if (status == NX_SECURE_X509_SUCCESS)
    {
        status = _nx_secure_x509_store_certificate_add(
            &certificate,
            &session.nx_secure_tls_credentials.nx_secure_tls_certificate_store,
            NX_SECURE_X509_CERT_LOCATION_REMOTE);
    }
    session.nx_secure_tls_1_3 = 1;
    verify_message[0] = 0x08;
    verify_message[1] = 0x04; /* rsa_pss_rsae_sha256 */
    verify_message[2] = 0;
    verify_message[3] = 0;
    if (status == NX_SECURE_X509_SUCCESS)
    {
        status = _nx_secure_tls_process_certificate_verify(&session, verify_message,
                                                           sizeof(verify_message));
    }
    check(status == NX_SECURE_TLS_UNSUPPORTED_CERT_SIGN_ALG,
          "an RSAE scheme is refused for a PSS-only key");

    certificate.nx_secure_x509_public_key_identifier = NX_SECURE_TLS_X509_TYPE_RSA;
    verify_message[1] = 0x09; /* rsa_pss_pss_sha256 */
    status = _nx_secure_tls_process_certificate_verify(&session, verify_message,
                                                       sizeof(verify_message));
    check(status == NX_SECURE_TLS_UNSUPPORTED_CERT_SIGN_ALG,
          "a PSS-key scheme is refused for an RSAE key");

    certificate.nx_secure_x509_public_key_identifier = NX_SECURE_TLS_X509_TYPE_RSA_PSS;
    certificate.nx_secure_x509_public_key_pss_algorithm =
        NX_SECURE_TLS_X509_TYPE_RSA_PSS_SHA_256;
    certificate.nx_secure_x509_public_key_pss_salt_length = 31;
    status = _nx_secure_tls_process_certificate_verify(&session, verify_message,
                                                       sizeof(verify_message));
    check(status == NX_SECURE_TLS_UNSUPPORTED_CERT_SIGN_ALG,
          "TLS requires PSS-key salt parameters to match exactly");

    certificate.nx_secure_x509_public_key_pss_algorithm =
        NX_SECURE_TLS_X509_TYPE_RSA_PSS_SHA_384;
    certificate.nx_secure_x509_public_key_pss_salt_length = 32;
    status = _nx_secure_tls_process_certificate_verify(&session, verify_message,
                                                       sizeof(verify_message));
    check(status == NX_SECURE_TLS_UNSUPPORTED_CERT_SIGN_ALG,
          "TLS CertificateVerify obeys the PSS-key digest restriction");
}

static UINT ku_chain_ok(NX_SECURE_X509_CERTIFICATE_STORE *store,
                        NX_SECURE_X509_CERT *certificate, ULONG current_time)
{
    (void)store;
    (void)certificate;
    (void)current_time;
    return NX_SECURE_X509_SUCCESS;
}

static UINT ku_verify(UCHAR usage, UINT algorithm, UINT tls_1_3,
                      UINT socket_type)
{
    static const UCHAR key_usage_prefix[] = {
        0x06, 0x03, 0x55, 0x1d, 0x0f, 0x01, 0x01,
        0xff, 0x04, 0x04, 0x03, 0x02, 0x05
    };
    UCHAR                           leaf[(sizeof(x509_ext_leaf_ok) >
                                          sizeof(x509_ext_leaf_clientauth)) ?
                                         sizeof(x509_ext_leaf_ok) :
                                         sizeof(x509_ext_leaf_clientauth)];
    const UCHAR                    *leaf_source;
    unsigned                       leaf_length;
    NX_SECURE_X509_CERT             certificate;
    NX_SECURE_X509_CERT             issuer;
    NX_SECURE_TLS_SESSION           session;
    NX_SECURE_TLS_CIPHERSUITE_INFO  ciphersuite;
    NX_CRYPTO_METHOD                public_cipher;
    UINT                            status;
    unsigned                        i;

    if (socket_type == NX_SECURE_TLS_SESSION_TYPE_SERVER)
    {
        leaf_source = x509_ext_leaf_clientauth;
        leaf_length = x509_ext_leaf_clientauth_len;
    }
    else
    {
        leaf_source = x509_ext_leaf_ok;
        leaf_length = x509_ext_leaf_ok_len;
    }

    memcpy(leaf, leaf_source, leaf_length);
    for (i = 0; i + sizeof(key_usage_prefix) < leaf_length; i++)
    {
        if (memcmp(&leaf[i], key_usage_prefix, sizeof(key_usage_prefix)) == 0)
        {
            leaf[i + sizeof(key_usage_prefix)] = usage;
            break;
        }
    }
    if (i + sizeof(key_usage_prefix) >= leaf_length)
    {
        return NX_SECURE_X509_EXTENSION_NOT_FOUND;
    }

    memset(&certificate, 0, sizeof(certificate));
    memset(&issuer, 0, sizeof(issuer));
    memset(&session, 0, sizeof(session));
    memset(&ciphersuite, 0, sizeof(ciphersuite));
    memset(&public_cipher, 0, sizeof(public_cipher));

    status = _nx_secure_x509_certificate_initialize(&certificate,
                                                     leaf, (USHORT)leaf_length,
                                                     NX_CRYPTO_NULL, 0,
                                                     NX_CRYPTO_NULL, 0,
                                                     NX_SECURE_X509_KEY_TYPE_NONE);
    if (status != NX_SECURE_X509_SUCCESS)
    {
        return status;
    }

    status = _nx_secure_x509_certificate_initialize(&issuer,
                                                     (UCHAR *)x509_ext_root,
                                                     (USHORT)x509_ext_root_len,
                                                     NX_CRYPTO_NULL, 0,
                                                     NX_CRYPTO_NULL, 0,
                                                     NX_SECURE_X509_KEY_TYPE_NONE);
    if (status != NX_SECURE_X509_SUCCESS)
    {
        return status;
    }

    status = _nx_secure_x509_store_certificate_add(
        &issuer,
        &session.nx_secure_tls_credentials.nx_secure_tls_certificate_store,
        NX_SECURE_X509_CERT_LOCATION_TRUSTED);
    if (status != NX_SECURE_X509_SUCCESS)
    {
        return status;
    }

    status = _nx_secure_x509_store_certificate_add(
        &certificate,
        &session.nx_secure_tls_credentials.nx_secure_tls_certificate_store,
        NX_SECURE_X509_CERT_LOCATION_REMOTE);
    if (status != NX_SECURE_X509_SUCCESS)
    {
        return status;
    }

    public_cipher.nx_crypto_algorithm = algorithm;
    ciphersuite.nx_secure_tls_public_cipher = &public_cipher;
    session.nx_secure_tls_session_ciphersuite = &ciphersuite;
    session.nx_secure_tls_protocol_version = NX_SECURE_TLS_VERSION_TLS_1_2;
    session.nx_secure_tls_1_3 = (UCHAR)(tls_1_3 ? 1u : 0u);
    session.nx_secure_tls_socket_type = socket_type;
    session.nx_secure_remote_certificate_verify = ku_chain_ok;

    return _nx_secure_tls_remote_certificate_verify(&session);
}

static void test_tls_key_usage(void)
{
    printf("tls keyUsage\n");

    check(ku_verify(0x20, NX_CRYPTO_KEY_EXCHANGE_RSA,
                    0 /* TLS 1.2 */,
                    NX_SECURE_TLS_SESSION_TYPE_CLIENT) == NX_SECURE_X509_SUCCESS,
          "TLS 1.2 static RSA permits keyEncipherment");
    check(ku_verify(0x80, NX_CRYPTO_KEY_EXCHANGE_RSA,
                    0 /* TLS 1.2 */,
                    NX_SECURE_TLS_SESSION_TYPE_CLIENT) == NX_SECURE_X509_KEY_USAGE_ERROR,
          "TLS 1.2 static RSA refuses signing-only key");

    check(ku_verify(0x80, NX_CRYPTO_KEY_EXCHANGE_ECDHE,
                    0 /* TLS 1.2 */,
                    NX_SECURE_TLS_SESSION_TYPE_CLIENT) == NX_SECURE_X509_SUCCESS,
          "TLS 1.2 ECDHE permits digitalSignature");
    check(ku_verify(0x20, NX_CRYPTO_KEY_EXCHANGE_ECDHE,
                    0 /* TLS 1.2 */,
                    NX_SECURE_TLS_SESSION_TYPE_CLIENT) == NX_SECURE_X509_KEY_USAGE_ERROR,
          "TLS 1.2 ECDHE refuses encryption-only key");

    check(ku_verify(0x80, NX_CRYPTO_KEY_EXCHANGE_RSA,
                    1 /* TLS 1.3 */,
                    NX_SECURE_TLS_SESSION_TYPE_CLIENT) == NX_SECURE_X509_SUCCESS,
          "TLS 1.3 requires a signing key");
    check(ku_verify(0x20, NX_CRYPTO_KEY_EXCHANGE_RSA,
                    1 /* TLS 1.3 */,
                    NX_SECURE_TLS_SESSION_TYPE_CLIENT) == NX_SECURE_X509_KEY_USAGE_ERROR,
          "TLS 1.3 refuses encryption-only key");

    check(ku_verify(0x80, NX_CRYPTO_KEY_EXCHANGE_RSA,
                    0 /* TLS 1.2 */,
                    NX_SECURE_TLS_SESSION_TYPE_SERVER) == NX_SECURE_X509_SUCCESS,
          "a client certificate permits digitalSignature");
    check(ku_verify(0x20, NX_CRYPTO_KEY_EXCHANGE_RSA,
                    0 /* TLS 1.2 */,
                    NX_SECURE_TLS_SESSION_TYPE_SERVER) == NX_SECURE_X509_KEY_USAGE_ERROR,
          "a client certificate refuses encryption-only key");
}

/* FIPS-197 appendix C.1, AES-128.  Its key schedule feeds S-box outputs of
   0x80 and above into the top byte of a 32-bit word (SubWord, and the final
   round's byte packing), which the vendored code once shifted as a promoted
   signed int: undefined behaviour that -fsanitize=undefined stops on. */
static const UCHAR fips197_c1_key[16] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
static const UCHAR fips197_c1_plain[16] = {
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
static const UCHAR fips197_c1_cipher[16] = {
    0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
    0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a};

static NX_CRYPTO_AES aes_ctx;

static void test_aes128_block(void)
{
    UCHAR key[16];
    UCHAR in[16];
    UCHAR out[16];
    UCHAR back[16];

    printf("aes-128 (FIPS-197 C.1)\n");

    memcpy(key, fips197_c1_key, sizeof(key));
    memcpy(in, fips197_c1_plain, sizeof(in));
    memset(&aes_ctx, 0, sizeof(aes_ctx));

    check(_nx_crypto_aes_key_set(&aes_ctx, key, NX_CRYPTO_AES_KEY_SIZE_128_BITS) == NX_CRYPTO_SUCCESS,
          "key expansion");
    check(_nx_crypto_aes_encrypt(&aes_ctx, in, out, sizeof(out)) == NX_CRYPTO_SUCCESS
          && memcmp(out, fips197_c1_cipher, sizeof(out)) == 0,
          "encrypt one block");
    check(_nx_crypto_aes_decrypt(&aes_ctx, out, back, sizeof(back)) == NX_CRYPTO_SUCCESS
          && memcmp(back, fips197_c1_plain, sizeof(back)) == 0,
          "decrypt one block");
}

/* RFC 8448 section 3, "Simple 1-RTT Handshake", TLS_AES_128_GCM_SHA256: the
   ECDHE shared secret, the ClientHello..ServerHello transcript hash, and what
   RFC 8446 7.1/7.3 derive from them.  The Finished keys are HKDF-Expand-Label
   output of Hash.length = 32 bytes into 32-byte members of
   NX_SECURE_TLS_KEY_SECRETS; the expand-label helper used to expand to the
   remaining key-block capacity (104 bytes) instead, running 68 bytes past the
   end of the secrets into nx_secure_tls_transcript_hashes (N106). */
static const UCHAR rfc8448_ecdhe[32] = {
    0x8b, 0xd4, 0x05, 0x4f, 0xb5, 0x5b, 0x9d, 0x63, 0xfd, 0xfb, 0xac, 0xf9,
    0xf0, 0x4b, 0x9f, 0x0d, 0x35, 0xe6, 0xd6, 0x3f, 0x53, 0x75, 0x63, 0xef,
    0xd4, 0x62, 0x72, 0x90, 0x0f, 0x89, 0x49, 0x2d};
static const UCHAR rfc8448_hello_hash[32] = {
    0x86, 0x0c, 0x06, 0xed, 0xc0, 0x78, 0x58, 0xee, 0x8e, 0x78, 0xf0, 0xe7,
    0x42, 0x8c, 0x58, 0xed, 0xd6, 0xb4, 0x3f, 0x2c, 0xa3, 0xe6, 0xe9, 0x5f,
    0x02, 0xed, 0x06, 0x3c, 0xf0, 0xe1, 0xca, 0xd8};
static const UCHAR rfc8448_c_hs_traffic[32] = {
    0xb3, 0xed, 0xdb, 0x12, 0x6e, 0x06, 0x7f, 0x35, 0xa7, 0x80, 0xb3, 0xab,
    0xf4, 0x5e, 0x2d, 0x8f, 0x3b, 0x1a, 0x95, 0x07, 0x38, 0xf5, 0x2e, 0x96,
    0x00, 0x74, 0x6a, 0x0e, 0x27, 0xa5, 0x5a, 0x21};
static const UCHAR rfc8448_s_hs_traffic[32] = {
    0xb6, 0x7b, 0x7d, 0x69, 0x0c, 0xc1, 0x6c, 0x4e, 0x75, 0xe5, 0x42, 0x13,
    0xcb, 0x2d, 0x37, 0xb4, 0xe9, 0xc9, 0x12, 0xbc, 0xde, 0xd9, 0x10, 0x5d,
    0x42, 0xbe, 0xfd, 0x59, 0xd3, 0x91, 0xad, 0x38};
static const UCHAR rfc8448_c_key[16] = {
    0xdb, 0xfa, 0xa6, 0x93, 0xd1, 0x76, 0x2c, 0x5b, 0x66, 0x6a, 0xf5, 0xd9,
    0x50, 0x25, 0x8d, 0x01};
static const UCHAR rfc8448_c_iv[12] = {
    0x5b, 0xd3, 0xc7, 0x1b, 0x83, 0x6e, 0x0b, 0x76, 0xbb, 0x73, 0x26, 0x5f};
static const UCHAR rfc8448_s_key[16] = {
    0x3f, 0xce, 0x51, 0x60, 0x09, 0xc2, 0x17, 0x27, 0xd0, 0xf2, 0xe4, 0xe8,
    0x6e, 0xe4, 0x03, 0xbc};
static const UCHAR rfc8448_s_iv[12] = {
    0x5d, 0x31, 0x3e, 0xb2, 0x67, 0x12, 0x76, 0xee, 0x13, 0x00, 0x0b, 0x30};
static const UCHAR rfc8448_c_finished_key[32] = {
    0xb8, 0x0a, 0xd0, 0x10, 0x15, 0xfb, 0x2f, 0x0b, 0xd6, 0x5f, 0xf7, 0xd4,
    0xda, 0x5d, 0x6b, 0xf8, 0x3f, 0x84, 0x82, 0x1d, 0x1f, 0x87, 0xfd, 0xc7,
    0xd3, 0xc7, 0x5b, 0x5a, 0x7b, 0x42, 0xd9, 0xc4};
static const UCHAR rfc8448_s_finished_key[32] = {
    0x00, 0x8d, 0x3b, 0x66, 0xf8, 0x16, 0xea, 0x55, 0x9f, 0x96, 0xb5, 0x37,
    0xe8, 0x85, 0xc3, 0x1f, 0xc0, 0x68, 0xbf, 0x49, 0x2c, 0x65, 0x2f, 0x01,
    0xf2, 0x88, 0xa1, 0xd8, 0xcd, 0xc1, 0x9f, 0xc8};

#define TLS13_GUARD 0xA5

/* _nx_secure_tls_session_create_ext installs _nx_secure_verify_mac, which
   reads records out of an NX_PACKET.  No record is processed here. */
UINT _nx_packet_data_extract_offset(NX_PACKET *packet_ptr, ULONG offset, VOID *buffer_start,
                                    ULONG buffer_length, ULONG *bytes_copied)
{
    (void)packet_ptr;
    (void)offset;
    (void)buffer_start;
    (void)buffer_length;
    *bytes_copied = 0;
    return(NX_NOT_SUCCESSFUL);
}

static NX_SECURE_TLS_SESSION tls13_session;
static UCHAR                 tls13_metadata[32768];

static int all_guard(const UCHAR *p, unsigned n)
{
    unsigned i;

    for (i = 0; i < n; i++)
    {
        if (p[i] != TLS13_GUARD)
        {
            return 0;
        }
    }
    return 1;
}

static void test_tls13_key_schedule(void)
{
    NX_SECURE_TLS_KEY_MATERIAL *km = &tls13_session.nx_secure_tls_key_material;
    NX_SECURE_TLS_KEY_SECRETS  *ks = &km->nx_secure_tls_key_secrets;
    NX_SECURE_TLS_CRYPTO       *table;
    ULONG                       metadata_size = 0;
    UINT                        status;
    USHORT                      i;
    int                         found = 0;

    printf("tls 1.3 key schedule (RFC 8448 3)\n");

    status = _nx_secure_tls_metadata_size_calculate(&nx_crypto_tls_ciphers_ecc, &metadata_size);
    check(status == NX_SUCCESS && metadata_size <= sizeof(tls13_metadata),
          "metadata fits");
    if (status != NX_SUCCESS || metadata_size > sizeof(tls13_metadata))
    {
        return;
    }

    status = _nx_secure_tls_session_create(&tls13_session, &nx_crypto_tls_ciphers_ecc,
                                           tls13_metadata, sizeof(tls13_metadata));
    check(status == NX_SUCCESS, "session create");
    if (status != NX_SUCCESS)
    {
        return;
    }

    table = tls13_session.nx_secure_tls_crypto_table;
    for (i = 0; i < table->nx_secure_tls_ciphersuite_lookup_table_size; i++)
    {
        if (table->nx_secure_tls_ciphersuite_lookup_table[i].nx_secure_tls_ciphersuite == TLS_AES_128_GCM_SHA256)
        {
            tls13_session.nx_secure_tls_session_ciphersuite = &table->nx_secure_tls_ciphersuite_lookup_table[i];
            found = 1;
            break;
        }
    }
    check(found, "TLS_AES_128_GCM_SHA256 in table");
    if (!found)
    {
        return;
    }

    tls13_session.nx_secure_tls_1_3 = 1;
    memcpy(km->nx_secure_tls_pre_master_secret, rfc8448_ecdhe, sizeof(rfc8448_ecdhe));
    km->nx_secure_tls_pre_master_secret_size = sizeof(rfc8448_ecdhe);

    /* Guards: everything after the client Finished key up to the end of the
       transcript hashes, except the ServerHello hash, which is an input. */
    memset(km->nx_secure_tls_transcript_hashes, TLS13_GUARD, sizeof(km->nx_secure_tls_transcript_hashes));
    memcpy(km->nx_secure_tls_transcript_hashes[NX_SECURE_TLS_TRANSCRIPT_IDX_SERVERHELLO],
           rfc8448_hello_hash, sizeof(rfc8448_hello_hash));
    memset(ks->tls_server_finished_key, TLS13_GUARD, sizeof(ks->tls_server_finished_key));
    memset(ks->tls_client_finished_key, TLS13_GUARD, sizeof(ks->tls_client_finished_key));

    status = _nx_secure_tls_1_3_generate_handshake_keys(&tls13_session);
    check(status == NX_SUCCESS, "generate handshake keys");

    check(memcmp(ks->tls_client_handshake_traffic_secret, rfc8448_c_hs_traffic, 32) == 0,
          "client_handshake_traffic_secret");
    check(memcmp(ks->tls_server_handshake_traffic_secret, rfc8448_s_hs_traffic, 32) == 0,
          "server_handshake_traffic_secret");
    check(km->nx_secure_tls_client_write_key != NX_NULL
          && memcmp(km->nx_secure_tls_client_write_key, rfc8448_c_key, 16) == 0,
          "client handshake write key");
    check(km->nx_secure_tls_client_iv != NX_NULL
          && memcmp(km->nx_secure_tls_client_iv, rfc8448_c_iv, 12) == 0,
          "client handshake write iv");
    check(km->nx_secure_tls_server_write_key != NX_NULL
          && memcmp(km->nx_secure_tls_server_write_key, rfc8448_s_key, 16) == 0,
          "server handshake write key");
    check(km->nx_secure_tls_server_iv != NX_NULL
          && memcmp(km->nx_secure_tls_server_iv, rfc8448_s_iv, 12) == 0,
          "server handshake write iv");
    check(ks->tls_server_finished_key_len == 32
          && memcmp(ks->tls_server_finished_key, rfc8448_s_finished_key, 32) == 0,
          "server finished_key");
    check(ks->tls_client_finished_key_len == 32
          && memcmp(ks->tls_client_finished_key, rfc8448_c_finished_key, 32) == 0,
          "client finished_key");

    check(all_guard(km->nx_secure_tls_transcript_hashes[NX_SECURE_TLS_TRANSCRIPT_IDX_CLIENTHELLO], 32),
          "guard: ClientHello transcript hash intact");
    check(memcmp(km->nx_secure_tls_transcript_hashes[NX_SECURE_TLS_TRANSCRIPT_IDX_SERVERHELLO],
                 rfc8448_hello_hash, 32) == 0,
          "guard: ServerHello transcript hash intact");
    check(all_guard(km->nx_secure_tls_transcript_hashes[NX_SECURE_TLS_TRANSCRIPT_IDX_CERTIFICATE],
                    (NX_SECURE_TLS_1_3_MAX_TRANSCRIPT_HASHES - NX_SECURE_TLS_TRANSCRIPT_IDX_CERTIFICATE) * 32u),
          "guard: later transcript hashes intact");
}

/* N-108: _nx_secure_tls_1_3_transcript_hash_save writes a hash_size digest
   into nx_secure_tls_transcript_hashes[hash_index].  An index equal to
   NX_SECURE_TLS_1_3_MAX_TRANSCRIPT_HASHES, or a ciphersuite hash longer than
   NX_SECURE_TLS_MAX_HASH_SIZE, has to be refused before anything is written.
   The rows and the start of nx_secure_tls_handshake_cache, which follows
   them, are guard-filled. */
#define N108_ROW_GUARD   0xA5
#define N108_CACHE_GUARD 0x5A
#define N108_CACHE_BYTES 64u

/* FIPS 180-2 B.1: SHA-256("abc"). */
static const UCHAR n108_sha256_abc[32] = {
    0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde,
    0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
    0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};

static void n108_fill(NX_SECURE_TLS_KEY_MATERIAL *km)
{
    memset(km->nx_secure_tls_transcript_hashes, N108_ROW_GUARD, sizeof(km->nx_secure_tls_transcript_hashes));
    memset(km->nx_secure_tls_handshake_cache, N108_CACHE_GUARD, N108_CACHE_BYTES);
}

static int n108_rows_intact(NX_SECURE_TLS_KEY_MATERIAL *km, int skip_row)
{
    int row;

    for (row = 0; row < NX_SECURE_TLS_1_3_MAX_TRANSCRIPT_HASHES; row++)
    {
        if (row != skip_row && !all_guard(km->nx_secure_tls_transcript_hashes[row], NX_SECURE_TLS_MAX_HASH_SIZE))
        {
            return 0;
        }
    }
    return 1;
}

static int n108_cache_intact(NX_SECURE_TLS_KEY_MATERIAL *km)
{
    unsigned i;

    for (i = 0; i < N108_CACHE_BYTES; i++)
    {
        if (km->nx_secure_tls_handshake_cache[i] != N108_CACHE_GUARD)
        {
            return 0;
        }
    }
    return 1;
}

/* Its own session: _nx_secure_tls_session_create links the control block into
   the created list, so creating tls13_session a second time would corrupt it. */
static NX_SECURE_TLS_SESSION n108_session;
static UCHAR                 n108_metadata[32768];

static void test_n108_transcript_hash_save(void)
{
    NX_SECURE_TLS_KEY_MATERIAL     *km = &n108_session.nx_secure_tls_key_material;
    NX_SECURE_TLS_CRYPTO           *table;
    NX_SECURE_TLS_CIPHERSUITE_INFO *sha256_suite = NX_NULL;
    NX_SECURE_TLS_CIPHERSUITE_INFO  long_hash_suite;
    UCHAR                           message[3] = {'a', 'b', 'c'};
    UINT                            status;
    USHORT                          i;

    printf("n108: tls 1.3 transcript hash save\n");

    status = _nx_secure_tls_session_create(&n108_session, &nx_crypto_tls_ciphers_ecc,
                                           n108_metadata, sizeof(n108_metadata));
    check(status == NX_SUCCESS, "n108: session create");
    if (status != NX_SUCCESS)
    {
        return;
    }

    table = n108_session.nx_secure_tls_crypto_table;
    for (i = 0; i < table->nx_secure_tls_ciphersuite_lookup_table_size; i++)
    {
        if (table->nx_secure_tls_ciphersuite_lookup_table[i].nx_secure_tls_ciphersuite == TLS_AES_128_GCM_SHA256)
        {
            sha256_suite = &table->nx_secure_tls_ciphersuite_lookup_table[i];
            break;
        }
    }
    check(sha256_suite != NX_NULL, "n108: TLS_AES_128_GCM_SHA256 in table");
    if (sha256_suite == NX_NULL)
    {
        return;
    }

    n108_session.nx_secure_tls_1_3 = 1;
    n108_session.nx_secure_tls_session_ciphersuite = sha256_suite;
    status = _nx_secure_tls_handshake_hash_init(&n108_session);
    if (status == NX_SUCCESS)
    {
        status = _nx_secure_tls_handshake_hash_update(&n108_session, message, sizeof(message));
    }
    check(status == NX_SUCCESS, "n108: handshake hash over \"abc\"");
    if (status != NX_SUCCESS)
    {
        return;
    }

    /* Control: the last row, SHA-256, exactly 32 bytes. */
    n108_fill(km);
    status = _nx_secure_tls_1_3_transcript_hash_save(&n108_session, NX_SECURE_TLS_TRANSCRIPT_IDX_SERVER_FINISHED, NX_TRUE);
    check(status == NX_SUCCESS, "n108: index 4 SHA-256 saves");
    check(memcmp(km->nx_secure_tls_transcript_hashes[NX_SECURE_TLS_TRANSCRIPT_IDX_SERVER_FINISHED],
                 n108_sha256_abc, sizeof(n108_sha256_abc)) == 0,
          "n108: index 4 holds SHA-256(\"abc\")");
    check(n108_rows_intact(km, NX_SECURE_TLS_TRANSCRIPT_IDX_SERVER_FINISHED),
          "n108: index 4 leaves rows 0-3 intact");
    check(n108_cache_intact(km), "n108: index 4 leaves the cache intact");

    /* One past the last row. */
    n108_fill(km);
    status = _nx_secure_tls_1_3_transcript_hash_save(&n108_session, NX_SECURE_TLS_1_3_MAX_TRANSCRIPT_HASHES, NX_TRUE);
    check(status == NX_INVALID_PARAMETERS, "n108: index 5 refused");
    check(n108_rows_intact(km, -1), "n108: index 5 leaves all rows intact");
    check(n108_cache_intact(km), "n108: index 5 leaves the cache intact");

    /* A 48-byte digest into a 32-byte row. */
    long_hash_suite = *sha256_suite;
    long_hash_suite.nx_secure_tls_hash = &crypto_method_sha384;
    n108_session.nx_secure_tls_session_ciphersuite = &long_hash_suite;
    n108_fill(km);
    status = _nx_secure_tls_1_3_transcript_hash_save(&n108_session, NX_SECURE_TLS_TRANSCRIPT_IDX_SERVER_FINISHED, NX_TRUE);
    check(status == NX_INVALID_PARAMETERS, "n108: SHA-384 refused");
    check(n108_rows_intact(km, -1), "n108: SHA-384 leaves all rows intact");
    check(n108_cache_intact(km), "n108: SHA-384 leaves the cache intact");

    n108_session.nx_secure_tls_session_ciphersuite = sha256_suite;
}

/* N-112, N-120, N-121: the ClientHello extension builder and the handshake
   cache, called directly on a zeroed session.  No record leaves: the record
   layer is replaced below, which keeps nx_secure_tls_send_record.o (and the
   TCP socket it reaches) out of the link. */
#define HRR_SENTINEL 0xA5
#define HRR_BUF      1024u
#define HRR_COOKIE   200u
#define HRR_KEYLEN   65u

static NX_SECURE_TLS_SESSION hrr_session;
static UCHAR                 hrr_buf[HRR_BUF];
static UCHAR                 hrr_ref[HRR_BUF];
static UCHAR                 hrr_cookie[HRR_COOKIE];
static unsigned              hrr_records_sent;

UINT _nx_secure_tls_send_record(NX_SECURE_TLS_SESSION *tls_session, NX_PACKET *send_packet,
                                UCHAR record_type, ULONG wait_option)
{
    (void)tls_session;
    (void)send_packet;
    (void)record_type;
    (void)wait_option;
    hrr_records_sent++;
    return(NX_SUCCESS);
}

/* The packet stays the caller's on every error: nothing here releases it. */
static unsigned hrr_packets_released;

UINT _nx_packet_release(NX_PACKET *packet_ptr)
{
    (void)packet_ptr;
    hrr_packets_released++;
    return(NX_SUCCESS);
}

static int all_byte(const UCHAR *p, unsigned n, UCHAR v)
{
    unsigned i;

    for (i = 0; i < n; i++)
    {
        if (p[i] != v)
        {
            return 0;
        }
    }
    return 1;
}

/* A TLS 1.3 session whose public key is HRR_KEYLEN bytes, in the first or
   the second (HelloRetryRequest) ClientHello state. */
static void hrr_session_reset(int retry, UINT cookie_length)
{
    NX_SECURE_TLS_ECDHE_HANDSHAKE_DATA *ecdhe;
    unsigned                            i;

    memset(&hrr_session, 0, sizeof(hrr_session));
    hrr_session.nx_secure_tls_1_3 = NX_TRUE;
    hrr_session.nx_secure_tls_protocol_version = NX_SECURE_TLS_VERSION_TLS_1_3;
    hrr_session.nx_secure_tls_client_state = retry ? NX_SECURE_TLS_CLIENT_STATE_HELLO_RETRY
                                                   : NX_SECURE_TLS_CLIENT_STATE_IDLE;
    ecdhe = &hrr_session.nx_secure_tls_key_material.nx_secure_tls_ecc_key_data[0];
    ecdhe->nx_secure_tls_ecdhe_named_curve = 0x0017;
    ecdhe->nx_secure_tls_ecdhe_public_key_length = HRR_KEYLEN;
    for (i = 0; i < HRR_KEYLEN; i++)
    {
        ecdhe->nx_secure_tls_ecdhe_public_key[i] = (UCHAR)(0x40 + i);
    }
    for (i = 0; i < HRR_COOKIE; i++)
    {
        hrr_cookie[i] = (UCHAR)(i * 7u + 1u);
    }
    hrr_session.nx_secure_tls_cookie = hrr_cookie;
    hrr_session.nx_secure_tls_cookie_length = cookie_length;
}

static UINT hrr_build(ULONG available, ULONG *out_length)
{
    ULONG offset = 0;
    ULONG ext_length = 0;
    UINT  status;

    memset(hrr_buf, HRR_SENTINEL, sizeof(hrr_buf));
    status = _nx_secure_tls_send_clienthello_extensions(&hrr_session, hrr_buf, &offset,
                                                        &ext_length, available);
    if (out_length)
    {
        *out_length = offset;
    }
    return(status);
}

/* Offset of the first extension of the given type in hrr_buf[0..len). */
static ULONG hrr_find(ULONG len, USHORT type)
{
    ULONG at = 0;

    while (at + 4u <= len)
    {
        USHORT t = (USHORT)((hrr_buf[at] << 8) | hrr_buf[at + 1]);
        USHORT l = (USHORT)((hrr_buf[at + 2] << 8) | hrr_buf[at + 3]);

        if (t == type)
        {
            return at;
        }
        at += 4u + l;
    }
    return HRR_BUF;
}

static void test_hrr_cookie_builder(void)
{
    ULONG full = 0, with_cookie = 0, cookie_at, key_share_at, len = 0, need;
    UINT  status;

    printf("clienthello extensions: hrr cookie and key_share (N-120, N-121)\n");

    /* Control: first ClientHello, no cookie. */
    hrr_session_reset(0, 0);
    status = hrr_build(HRR_BUF, &full);
    check(status == NX_SUCCESS && full > 0 && full < HRR_BUF, "first ClientHello builds");
    memcpy(hrr_ref, hrr_buf, sizeof(hrr_ref));
    key_share_at = hrr_find(full, NX_SECURE_TLS_EXTENSION_KEY_SHARE);
    check(key_share_at < full, "key_share present");
    check(hrr_find(full, NX_SECURE_TLS_EXTENSION_COOKIE) == HRR_BUF, "no cookie without HRR");

    status = hrr_build(full, &len);
    check(status == NX_SUCCESS && len == full && memcmp(hrr_buf, hrr_ref, full) == 0,
          "first ClientHello fits exactly, same bytes");

    /* Retry state, cookie 0: the same extensions. */
    hrr_session_reset(1, 0);
    status = hrr_build(HRR_BUF, &len);
    check(status == NX_SUCCESS && len == full && memcmp(hrr_buf, hrr_ref, full) == 0,
          "retry ClientHello, cookie 0, unchanged");

    /* Retry with a cookie: the whole thing fits exactly. */
    hrr_session_reset(1, HRR_COOKIE);
    status = hrr_build(HRR_BUF, &with_cookie);
    check(status == NX_SUCCESS && with_cookie == full + 6u + HRR_COOKIE, "cookie echoed, 6 + cookie bytes");
    cookie_at = hrr_find(with_cookie, NX_SECURE_TLS_EXTENSION_COOKIE);
    check(cookie_at < with_cookie && memcmp(&hrr_buf[cookie_at + 6u], hrr_cookie, HRR_COOKIE) == 0,
          "cookie bytes match");
    if (cookie_at >= with_cookie)
    {
        return;
    }
    memcpy(hrr_ref, hrr_buf, sizeof(hrr_ref));

    hrr_session_reset(1, HRR_COOKIE);
    status = hrr_build(with_cookie, &len);
    check(status == NX_SUCCESS && len == with_cookie && memcmp(hrr_buf, hrr_ref, with_cookie) == 0 &&
          all_byte(&hrr_buf[with_cookie], HRR_BUF - with_cookie, HRR_SENTINEL),
          "cookie ClientHello fits exactly");

    /* The cookie extension itself ends exactly at available_size. */
    need = cookie_at + 6u + HRR_COOKIE;
    hrr_session_reset(1, HRR_COOKIE);
    status = hrr_build(need, NX_NULL);
    check(status == NX_SECURE_TLS_PACKET_BUFFER_TOO_SMALL &&
          memcmp(&hrr_buf[cookie_at], &hrr_ref[cookie_at], 6u + HRR_COOKIE) == 0 &&
          all_byte(&hrr_buf[need], HRR_BUF - need, HRR_SENTINEL),
          "cookie fits to the byte, nothing past it");

    /* One byte short of the cookie: nothing of it is written. */
    hrr_session_reset(1, HRR_COOKIE);
    status = hrr_build(need - 1u, NX_NULL);
    check(status == NX_SECURE_TLS_PACKET_BUFFER_TOO_SMALL, "cookie one byte short rejected");
    check(all_byte(&hrr_buf[cookie_at], HRR_BUF - cookie_at, HRR_SENTINEL),
          "cookie one byte short writes nothing");

    /* Six bytes of header only. */
    hrr_session_reset(1, HRR_COOKIE);
    status = hrr_build(cookie_at + 6u, NX_NULL);
    check(status == NX_SECURE_TLS_PACKET_BUFFER_TOO_SMALL &&
          all_byte(&hrr_buf[cookie_at], HRR_BUF - cookie_at, HRR_SENTINEL),
          "cookie header only rejected");

    /* N-120: key_share one byte short.  The 8 bytes of the two empty
       extensions after it still fit, so only its own status can fail. */
    hrr_session_reset(0, 0);
    status = hrr_build(key_share_at + 10u + HRR_KEYLEN - 1u, NX_NULL);
    check(status == NX_SECURE_TLS_PACKET_BUFFER_TOO_SMALL, "key_share failure propagates");
    check(all_byte(&hrr_buf[key_share_at], HRR_BUF - key_share_at, HRR_SENTINEL),
          "key_share failure writes nothing");

    /* Last: unbounded, this one writes 64 KB past the buffer. */
    hrr_session_reset(1, 0xFFFFu);
    status = hrr_build(cookie_at, NX_NULL);
    check(status == NX_SECURE_TLS_PACKET_BUFFER_TOO_SMALL &&
          all_byte(&hrr_buf[cookie_at], HRR_BUF - cookie_at, HRR_SENTINEL),
          "64 KB cookie rejected");
}

/* An NX_PACKET with room for the 4-byte handshake header in front. */
static NX_PACKET hrr_packet;
static UCHAR     hrr_packet_data[1024];

static UINT hrr_send(ULONG body_length)
{
    ULONG i;

    memset(&hrr_packet, 0, sizeof(hrr_packet));
    for (i = 0; i < body_length; i++)
    {
        hrr_packet_data[16 + i] = (UCHAR)(i * 13u + 5u);
    }
    hrr_packet.nx_packet_data_start = hrr_packet_data;
    hrr_packet.nx_packet_data_end = hrr_packet_data + sizeof(hrr_packet_data);
    hrr_packet.nx_packet_prepend_ptr = hrr_packet_data + 16;
    hrr_packet.nx_packet_append_ptr = hrr_packet_data + 16 + body_length;
    hrr_packet.nx_packet_length = body_length;
    return(_nx_secure_tls_send_handshake_record(&hrr_session, &hrr_packet,
                                                NX_SECURE_TLS_CLIENT_HELLO, NX_NO_WAIT));
}

#define HRR_CACHE_SIZE \
    (sizeof(hrr_session.nx_secure_tls_key_material.nx_secure_tls_handshake_cache))

/* Everything from the cache length to the end of the key material. */
#define HRR_TAIL_OFFSET \
    (offsetof(NX_SECURE_TLS_KEY_MATERIAL, nx_secure_tls_handshake_cache_length))
#define HRR_TAIL_SIZE (sizeof(NX_SECURE_TLS_KEY_MATERIAL) - HRR_TAIL_OFFSET)

static UCHAR hrr_tail[sizeof(NX_SECURE_TLS_KEY_MATERIAL)];

static void hrr_cache_prefill(UINT cached)
{
    NX_SECURE_TLS_KEY_MATERIAL *km = &hrr_session.nx_secure_tls_key_material;

    hrr_session_reset(0, 0);
    memset((UCHAR *)km + HRR_TAIL_OFFSET, HRR_SENTINEL, HRR_TAIL_SIZE);
    km->nx_secure_tls_handshake_cache_length = cached;
    memcpy(hrr_tail, (UCHAR *)km + HRR_TAIL_OFFSET, HRR_TAIL_SIZE);
    hrr_records_sent = 0;
    hrr_packets_released = 0;
}

static int hrr_tail_intact(void)
{
    return memcmp(hrr_tail, (UCHAR *)&hrr_session.nx_secure_tls_key_material + HRR_TAIL_OFFSET,
                  HRR_TAIL_SIZE) == 0;
}

static void test_hrr_handshake_cache(void)
{
    NX_SECURE_TLS_KEY_MATERIAL *km = &hrr_session.nx_secure_tls_key_material;
    const ULONG                 body = 196u;    /* 200 with the header */
    UINT                        status;

    printf("clienthello handshake cache (N-112)\n");

    /* Control: an empty cache takes the ClientHello as sent. */
    hrr_cache_prefill(0);
    status = hrr_send(body);
    check(status == NX_SUCCESS && hrr_records_sent == 1 &&
          km->nx_secure_tls_handshake_cache_length == body + 4u &&
          memcmp(km->nx_secure_tls_handshake_cache, hrr_packet_data + 12, body + 4u) == 0,
          "first ClientHello cached as sent");

    hrr_cache_prefill((UINT)(HRR_CACHE_SIZE - (body + 4u)));
    status = hrr_send(body);
    check(status == NX_SUCCESS && hrr_records_sent == 1 &&
          km->nx_secure_tls_handshake_cache_length == HRR_CACHE_SIZE,
          "cache filled exactly");

    hrr_cache_prefill((UINT)(HRR_CACHE_SIZE - (body + 4u) + 1u));
    status = hrr_send(body);
    check(status == NX_SECURE_TLS_PACKET_BUFFER_TOO_SMALL && hrr_records_sent == 0 && hrr_packets_released == 0 && hrr_tail_intact(),
          "one byte over rejected, length intact");

    hrr_cache_prefill(0);
    status = hrr_send(HRR_CACHE_SIZE - 4u + 1u);
    check(status == NX_SECURE_TLS_PACKET_BUFFER_TOO_SMALL && hrr_records_sent == 0 && hrr_packets_released == 0 && hrr_tail_intact(),
          "501-byte ClientHello rejected, tail intact");

    hrr_cache_prefill((UINT)HRR_CACHE_SIZE + 1u);
    status = hrr_send(16);
    check(status == NX_SECURE_TLS_PACKET_BUFFER_TOO_SMALL && hrr_records_sent == 0 && hrr_packets_released == 0 && hrr_tail_intact(),
          "corrupt cache length rejected");
}

int main(void)
{
    _nx_crypto_initialize();

    test_pkcs1();
    test_ecdsa();
    test_sigalg();
    test_modulus();
    test_chain();
    test_extensions();
    test_pss();
    test_pss_schemes();
    test_tls_key_usage();
    test_aes128_block();
    test_tls13_key_schedule();
    test_n108_transcript_hash_save();
    test_hrr_handshake_cache();
    test_hrr_cookie_builder();

    if (failures != 0)
    {
        printf("test_tls_x509: %d failure(s)\n", failures);
        return 1;
    }

    printf("test_tls_x509: all checks passed\n");
    return 0;
}
