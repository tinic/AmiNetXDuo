/*
 * AmiNetXDuo, host unit tests for the certificate and signature checks.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include "nx_secure_tls.h"
#include "nx_secure_x509.h"
#include "nx_crypto_rsa.h"
#include "nx_crypto_sha2.h"
#include "nx_crypto_sha5.h"
#include "nx_crypto_ecdsa.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "tls_test_certs.h"
#pragma GCC diagnostic pop

#include "tls_root_isrg_x1.h"
#include "x509_test_vectors.h"
#include "x509_ext_vectors.h"
#include "x509_pss_vectors.h"

/* The vendored code takes this around anything that can suspend. Nothing
   suspends here, so the object only has to exist. */
TX_MUTEX _nx_secure_tls_protection;

extern NX_SECURE_X509_CRYPTO _nx_crypto_x509_cipher_lookup_table[];
extern const UINT            _nx_crypto_x509_cipher_lookup_table_size;
extern NX_CRYPTO_METHOD      crypto_method_ecdsa;
extern NX_CRYPTO_METHOD      crypto_method_ec_secp256;
extern NX_CRYPTO_METHOD      crypto_method_rsa;
extern NX_CRYPTO_METHOD      crypto_method_sha256;
extern NX_CRYPTO_METHOD      crypto_method_sha384;
extern NX_CRYPTO_METHOD      crypto_method_sha512;

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

    if (failures != 0)
    {
        printf("test_tls_x509: %d failure(s)\n", failures);
        return 1;
    }

    printf("test_tls_x509: all checks passed\n");
    return 0;
}
