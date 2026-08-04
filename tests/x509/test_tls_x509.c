/*
 * AmiNetXDuo -- host unit tests for the certificate and signature checks.
 *
 * tests/fuzz/fuzz_tls_x509 answers "does this parser stay inside its buffer".
 * This answers the other question: does it reject what it is supposed to
 * reject, and still accept what it is supposed to accept. Both matter, and a
 * sanitizer cannot see the second one -- a verifier that returns success on
 * everything is perfectly memory-safe.
 *
 * Each section is one thing a peer controls:
 *
 *   pkcs1     the RSA signature block, after decryption. Every byte of it is
 *             fixed by RFC 8017 9.2, and a verifier that skims it accepts
 *             Bleichenbacher's e=3 forgery, which needs no private key.
 *
 *   ecdsa     the DER SEQUENCE { INTEGER r, INTEGER s } off the wire, against
 *             a real P-256 signature that must keep verifying.
 *
 *   sigalg    the signature algorithm identifier appears twice in a
 *             certificate and only one copy is signed. They have to agree.
 *
 *   chain     two CAs that have cross-signed each other. The issuer walk has
 *             a cycle in it, and before the depth cap it did not come back.
 *
 * The real certificates from tests/tls are parsed first, as a guard: if a
 * strictness change starts rejecting the sample leaf, the sample CA or ISRG
 * Root X1, that is a regression and not a hardening.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include "nx_secure_tls.h"
#include "nx_secure_x509.h"
#include "nx_crypto_rsa.h"
#include "nx_crypto_sha2.h"
#include "nx_crypto_ecdsa.h"

/*
 * tls_test_certs.h also carries the leaf's private key, which nothing here
 * needs; the push/pop is around the include alone, as in fuzz_tls_x509.c.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "tls_test_certs.h"
#pragma GCC diagnostic pop

#include "tls_root_isrg_x1.h"
#include "x509_test_vectors.h"

/* The vendored code takes this around anything that can suspend. Nothing
   suspends here, so the object only has to exist. */
TX_MUTEX _nx_secure_tls_protection;

extern NX_SECURE_X509_CRYPTO _nx_crypto_x509_cipher_lookup_table[];
extern const UINT            _nx_crypto_x509_cipher_lookup_table_size;
extern NX_CRYPTO_METHOD      crypto_method_ecdsa;
extern NX_CRYPTO_METHOD      crypto_method_ec_secp256;

static int failures = 0;

static void check(int ok, const char *what)
{
    printf("  %-46s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
    {
        failures++;
    }
}

/* ------------------------------------------------------------- pkcs1 ------ */

/*
 * EM = 0x00 || 0x01 || PS || 0x00 || T, where T is the DER DigestInfo for
 * SHA-256 and PS fills the modulus. 256 bytes is RSA-2048.
 */
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

    /*
     * The forgery shape: a short run of padding, the terminator, a DigestInfo,
     * and whatever the attacker likes after it. Both halves are checked
     * separately below, because either one alone closes it.
     */
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

/* ------------------------------------------------------------- ecdsa ------ */

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

    /*
     * The one case that reaches the curve arithmetic, and so 32-bit builds
     * only: nx_crypto_ec.c does pointer arithmetic through ULONG, and ULONG is
     * the target's 32 bits everywhere in this tree, so on an LP64 host the
     * curve code truncates every pointer it touches. See this directory's
     * CMakeLists.txt; tools/ci.sh's host32 stage runs it.
     *
     * Every rejection below returns from the DER parse, which is above the
     * point setup, so those run in every build.
     */
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

/* ------------------------------------------------------------ sigalg ------ */

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

    /*
     * The leaf names sha256WithRSAEncryption twice, and the second one is the
     * copy outside the signed body. Rewrite that one to sha1WithRSA -- the
     * change a man in the middle can make without touching the signature.
     */
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

/* ------------------------------------------------------------ modulus ----- */

static void test_modulus(void)
{
static unsigned char copy[8192];
NX_SECURE_X509_CERT  cert;
UINT                 bytes;
UINT                 status;
unsigned             i;
unsigned             at = 0;

    printf("modulus\n");

    /*
     * The leaf's RSA-2048 modulus is a 257-byte INTEGER (256 bytes plus the
     * DER sign pad). Shrink the INTEGER's declared length to 65, which is 512
     * bits -- factorable on a laptop -- and leave the bytes where they are.
     * Everything after it is then garbage, which is the point: the key is
     * refused before any of it is used.
     */
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

/* ------------------------------------------------------------- chain ------ */

static union
{
    NX_CRYPTO_RSA rsa;
    ULONG         align;
} chain_pubkey_metadata;

static union
{
    NX_CRYPTO_SHA256 sha;
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

    /*
     * A's issuer is B and B's issuer is A, both are CAs with keyCertSign, and
     * neither is in the trusted store -- so every signature along the way
     * verifies and the walk never arrives anywhere. Without a depth cap this
     * call does not return.
     */
    status = _nx_secure_x509_certificate_chain_verify(&store, &cert_a, 0);
    check(status == NX_SECURE_X509_CHAIN_TOO_LONG, "a cross-signed cycle terminates");
}

/* -------------------------------------------------------------- main ------ */

int main(void)
{
    _nx_crypto_initialize();

    test_pkcs1();
    test_ecdsa();
    test_sigalg();
    test_modulus();
    test_chain();

    if (failures != 0)
    {
        printf("test_tls_x509: %d failure(s)\n", failures);
        return 1;
    }

    printf("test_tls_x509: all checks passed\n");
    return 0;
}
