/*
 * AmiNetXDuo -- host fuzz driver for the three TLS handshake messages that
 * dispatch through a negotiated ciphersuite into NX_CRYPTO_METHOD entries.
 *
 * fuzz_tls_record stops where the crypto starts: it carries a ciphersuite
 * table of six IDs and no method pointers, because none of the parsers it
 * drives calls one. ServerKeyExchange, CertificateVerify and Finished all do,
 * so they need the real tables -- ami_crypto_tls_ciphers_ecc and
 * ami_crypto_ecc_curves from src/tls/ami_tls_crypto.c, the ones tls_conn.c
 * hands to _nx_secure_tls_session_create(). That is this driver.
 *
 * WHY IT IS A 32-BIT BUILD, like fuzz_mdns
 *
 * ami_tls_crypto.c includes exec/types.h and checks metadata alignment with
 * ((ULONG)crypto_metadata & 3), and _nx_secure_tls_process_certificate_verify()
 * bounds its signature with ((ULONG)packet_buffer + message_length) <
 * ((ULONG)received_signature + length). ULONG is the target's 32 bits, so on
 * an LP64 host both truncate a pointer -- the second one is the bounds check
 * under test, and a truncated bounds check is not the one that ships. The
 * -m32 build makes ULONG and the pointer the same width again, which is what
 * the m68k has. tools/ci.sh's host32 stage is where this runs.
 *
 * WHAT IS DRIVEN, AND AT WHICH BOUNDARY
 *
 *   _nx_secure_tls_process_server_key_exchange()  -> _nx_secure_process_server_key_exchange()
 *   _nx_secure_tls_process_certificate_verify()
 *   _nx_secure_tls_process_finished()
 *
 * each on a session built the way tls_conn.c builds one: session_create with
 * ami_crypto_tls_ciphers_ecc, ecc_initialize with ami_crypto_ecc_curves, a
 * record buffer of TLS_DEFAULT_RECORD_BUFFER, four remote certificate slots,
 * and the server's Certificate message already processed so the remote
 * endpoint certificate is in the store -- which is where all three of these
 * read the peer's public key from. Everything after that is bytes off the wire.
 *
 * THE LENGTH CONTRACT IS THE RECORD PAYLOAD, NOT THE MESSAGE.
 *
 * Same as fuzz_tls_record. The message is copied into an allocation sized to
 * itself, which models the record payload ending exactly where the message
 * does -- and a hostile server produces that at will by putting filler
 * messages in front of it until the target message ends at the record buffer's
 * last byte. An over-read this reports is one a real record reproduces.
 *
 * REACHING THE ECDH IS NOT FREE
 *
 * _nx_secure_process_server_key_exchange() verifies the server's signature
 * over (client_random || server_random || params) before it touches the peer's
 * ECDHE point, so a ServerKeyExchange with a made-up signature stops at the
 * verify and the ECDH import is never exercised. The party that gets past it
 * is a server holding a certificate -- which is exactly the hostile server
 * this driver models. So the seeds are SIGNED, at startup, with the private
 * key of the sample leaf in tests/tls/tls_test_certs.h: the same key, the same
 * PKCS#1 v1.5 construction and the same RSA operation
 * nx_secure_tls_send_certificate_verify.c uses. Five parameter shapes are
 * signed once and reused, which is cheap; re-signing every mutation would be
 * an RSA private operation per case, so mutations of the signed region stop at
 * the verify by design. -r reports both counts, so "clean" says which.
 *
 * WHO CAN SEND WHICH OF THESE
 *
 * ServerKeyExchange and Finished are what a server sends a client, so
 * tls.library reaches both on any HTTPS fetch. CertificateVerify is the other
 * direction -- only _nx_secure_tls_server_handshake() calls it -- and
 * tls_conn.c only ever starts a client session, so nothing in the shipped
 * library reaches it today. It is driven anyway: the function is linked into
 * tls.library, the ciphersuite tables that reach it are ours, and a driver
 * that skipped it would have to be written the day a server session appears.
 * It is also where the first over-read this driver found was.
 *
 * NOT COVERED HERE
 *
 *   The ECDSA signature arm of ServerKeyExchange, and the EC arm of
 *   CertificateVerify. Both need the remote endpoint to carry an EC public
 *   key, and the only certificates in this tree are RSA. The length arithmetic
 *   in front of both arms is driven; the verify itself is not.
 *
 *   TLS 1.3. NX_SECURE_TLS_TLS_1_3_ENABLED is off, so the 1.3 arms of all
 *   three functions are not compiled.
 *
 * Usage, matching fuzz_dhcp:
 *   fuzz_tls_crypto -s                every seed case, named
 *   fuzz_tls_crypto -c NAME           one seed case by name
 *   fuzz_tls_crypto -r SEED COUNT     seeds plus mutations
 *   fuzz_tls_crypto < message         one ServerKeyExchange from stdin
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nx_secure_tls.h"
#include "nx_secure_x509.h"
#include "nx_crypto_sha2.h"

#include "ami_tls_crypto.h"
#include "tls.h"

/* tls_test_certs.h carries the leaf's private key, which this driver does use
   -- see the signing note above -- and the CA's, which it does not. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "tls_test_certs.h"
#pragma GCC diagnostic pop

/*
 * ami_tls_crypto.c times every operation through timer.device. There is no
 * timer.device here and the counters it keeps are still exact without one --
 * ami_tls_timer_is_open() answering FALSE is the documented "counts but no
 * microseconds" case, and this driver reads only the counts.
 */
BOOL ami_tls_timer_is_open(VOID)
{
    return 0;
}

ULONG ami_tls_eclock(VOID)
{
    return 0;
}

ULONG ami_tls_eclock_micros(ULONG ticks)
{
    return ticks;
}

/* tls_internal.h's sizes, restated: that header pulls in the AmigaOS library
   ABI. These are what tls_conn.c allocates. */
#define FC_RECORD_BUFFER    10240
#define FC_REMOTE_DER_MAX   2560
#define FC_CHAIN            4

#define FC_MAX              2048

/* RSA-2048, which is what the sample leaf carries. */
#define FC_SIG_BYTES        256

/* secp256r1, and an uncompressed point on it: 0x04 || X || Y. */
#define FC_CURVE_SECP256R1  23
#define FC_POINT_BYTES      65

typedef struct
{
    unsigned char b[FC_MAX];
    unsigned      len;
} FcBuf;

static void fc_reset(FcBuf *w)
{
    memset(w->b, 0, sizeof(w->b));
    w->len = 0;
}

static void fc_u8(FcBuf *w, unsigned v)
{
    if (w->len < FC_MAX)
        w->b[w->len++] = (unsigned char)v;
}

static void fc_u16(FcBuf *w, unsigned v)
{
    fc_u8(w, v >> 8);
    fc_u8(w, v);
}

static void fc_bytes(FcBuf *w, unsigned n, unsigned char fill)
{
    while (n-- > 0)
        fc_u8(w, fill);
}

static void fc_raw(FcBuf *w, const unsigned char *p, unsigned n)
{
    while (n-- > 0)
        fc_u8(w, *p++);
}

/* ------------------------------------------------------------ the session -- */

static UCHAR               *fc_metadata;
static ULONG                fc_metadata_size;
static UCHAR               *fc_record;
static NX_SECURE_X509_CERT  fc_remote[FC_CHAIN];
static UCHAR               *fc_remote_der;

/* The randoms the signature is computed over. Fixed, because the signed seeds
   are built once against them. */
static UCHAR                fc_client_random[32];
static UCHAR                fc_server_random[32];

/* The chain verify a real session does needs a trust anchor this driver has
   no reason to supply. Answering NX_SUCCESS is "the chain checked out", which
   is the branch that installs the remote endpoint -- and the endpoint is what
   all three parsers under test read the peer's public key from. */
static UINT fc_verify_ok(NX_SECURE_X509_CERTIFICATE_STORE *store,
                         NX_SECURE_X509_CERT *certificate, ULONG current_time)
{
    NX_PARAMETER_NOT_USED(store);
    NX_PARAMETER_NOT_USED(certificate);
    NX_PARAMETER_NOT_USED(current_time);
    return NX_SUCCESS;
}

/* The server's Certificate message, into the record buffer, so that
   _nx_secure_x509_remote_endpoint_certificate_get() finds a certificate. */
static UINT fc_install_certificate(NX_SECURE_TLS_SESSION *s)
{
    unsigned len   = test_device_cert_der_len;
    unsigned total = 3u + len;
    unsigned at    = 0;

    memset(fc_record, 0, FC_RECORD_BUFFER);

    fc_record[at++] = (UCHAR)(total >> 16);
    fc_record[at++] = (UCHAR)(total >> 8);
    fc_record[at++] = (UCHAR)total;
    fc_record[at++] = (UCHAR)(len >> 16);
    fc_record[at++] = (UCHAR)(len >> 8);
    fc_record[at++] = (UCHAR)len;
    memcpy(&fc_record[at], test_device_cert_der, len);
    at += len;

    return _nx_secure_tls_process_remote_certificate(s, fc_record, at, at);
}

/*
 * A session at the moment the server's key exchange arrives: ServerHello
 * processed, ciphersuite chosen, Certificate processed. Rebuilt for every case
 * because every one of these parsers writes into it.
 */
static UINT fc_session_open(NX_SECURE_TLS_SESSION *s, UINT suite)
{
    const NX_SECURE_TLS_CIPHERSUITE_INFO *info = NX_NULL;
    USHORT                                priority = 0;
    UINT                                  status;
    unsigned                              i;

    memset(s, 0, sizeof(*s));
    memset(fc_remote, 0, sizeof(fc_remote));

    status = _nx_secure_tls_session_create(s, &ami_crypto_tls_ciphers_ecc,
                                           fc_metadata, fc_metadata_size);
    if (status != NX_SUCCESS)
        return status;

    (void)_nx_secure_tls_ecc_initialize(s, ami_crypto_ecc_supported_groups,
                                        (USHORT)ami_crypto_ecc_supported_groups_size,
                                        ami_crypto_ecc_curves);

    (void)_nx_secure_tls_session_packet_buffer_set(s, fc_record,
                                                   FC_RECORD_BUFFER);

    for (i = 0; i < FC_CHAIN; i++)
    {
        (void)_nx_secure_tls_remote_certificate_allocate(
                  s, &fc_remote[i], &fc_remote_der[i * FC_REMOTE_DER_MAX],
                  FC_REMOTE_DER_MAX);
    }

    s->nx_secure_remote_certificate_verify = fc_verify_ok;
    s->nx_secure_tls_socket_type           = NX_SECURE_TLS_SESSION_TYPE_CLIENT;
    s->nx_secure_tls_protocol_version      = NX_SECURE_TLS_VERSION_TLS_1_2;

    status = _nx_secure_tls_ciphersuite_lookup(s, suite, &info, &priority);
    if (status != NX_SUCCESS)
    {
        (void)_nx_secure_tls_session_delete(s);
        return status;
    }
    s->nx_secure_tls_session_ciphersuite = info;

    memcpy(s->nx_secure_tls_key_material.nx_secure_tls_client_random,
           fc_client_random, sizeof(fc_client_random));
    memcpy(s->nx_secure_tls_key_material.nx_secure_tls_server_random,
           fc_server_random, sizeof(fc_server_random));

    /* The Finished hash is a PRF over the master secret; any value reaches it,
       and none of them makes a hostile server's Finished match. */
    memset(s->nx_secure_tls_key_material.nx_secure_tls_master_secret, 0x5A,
           sizeof(s->nx_secure_tls_key_material.nx_secure_tls_master_secret));

    status = _nx_secure_tls_handshake_hash_init(s);
    if (status == NX_SUCCESS)
        status = fc_install_certificate(s);

    if (status != NX_SUCCESS)
    {
        (void)_nx_secure_tls_session_delete(s);
        return status;
    }

    /* Where the client is when the ServerKeyExchange arrives; the ECDHE arm
       rejects any other state. */
    s->nx_secure_tls_client_state = NX_SECURE_TLS_CLIENT_STATE_SERVER_CERTIFICATE;

    return NX_SUCCESS;
}

/* nx_secure keeps every created session on a global ring, so a driver that
   builds one per case has to take each one off again -- the next
   session_create() walks the ring and follows whatever the last one left. */
static void fc_session_close(NX_SECURE_TLS_SESSION *s)
{
    (void)_nx_secure_tls_session_delete(s);
}

/* -------------------------------------------------------------- signing ---- */

/*
 * The sample leaf's private key, and the RSA method out of the ciphersuite
 * table -- the same object ami_tls_crypto.c puts in nx_secure_tls_public_auth
 * for the ECDHE_RSA suites, which is what verifies what this signs.
 */
static NX_SECURE_X509_CERT      fc_signer;
static const NX_CRYPTO_METHOD  *fc_rsa;
static UCHAR                   *fc_rsa_metadata;
static ULONG                    fc_rsa_metadata_size;

static const NX_CRYPTO_METHOD *fc_suite_auth(UINT suite)
{
    const NX_SECURE_TLS_CIPHERSUITE_INFO *table =
        ami_crypto_tls_ciphers_ecc.nx_secure_tls_ciphersuite_lookup_table;
    USHORT n = ami_crypto_tls_ciphers_ecc.nx_secure_tls_ciphersuite_lookup_table_size;
    USHORT i;

    for (i = 0; i < n; i++)
    {
        if (table[i].nx_secure_tls_ciphersuite == suite)
            return table[i].nx_secure_tls_public_auth;
    }

    return NX_NULL;
}

/* The hash method the verifier will use, taken from the same X.509 table it
   looks it up in rather than named directly. */
static const NX_CRYPTO_METHOD *fc_x509_hash(USHORT identifier)
{
    NX_SECURE_X509_CRYPTO *table =
        ami_crypto_tls_ciphers_ecc.nx_secure_tls_x509_cipher_table;
    USHORT n = ami_crypto_tls_ciphers_ecc.nx_secure_tls_x509_cipher_table_size;
    USHORT i;

    for (i = 0; i < n; i++)
    {
        if (table[i].nx_secure_x509_crypto_identifier == identifier)
            return table[i].nx_secure_x509_hash_method;
    }

    return NX_NULL;
}

/* SHA-256 over the three pieces the verifier hashes: both randoms, then the
   key-exchange parameters. */
static UINT fc_params_hash(const unsigned char *params, unsigned params_len,
                           UCHAR out[32])
{
    NX_CRYPTO_METHOD *m = (NX_CRYPTO_METHOD *)
                          fc_x509_hash(NX_SECURE_TLS_X509_TYPE_RSA_SHA_256);
    /* Whichever SHA-256 the table holds -- ami_tls_crypto.c's or nx_crypto's
       -- states its own metadata size, so the buffer is checked against it
       rather than sized from one of the two structures. */
    static ULONG      metadata[256];
    VOID             *handler = NX_NULL;
    UINT              status;

    if (m == NX_NULL || m->nx_crypto_metadata_area_size > sizeof(metadata))
        return NX_CRYPTO_NOT_SUCCESSFUL;

    status = m->nx_crypto_init(m, NX_NULL, 0, &handler, (UCHAR *)metadata,
                               sizeof(metadata));
    if (status != NX_CRYPTO_SUCCESS)
        return status;

    status = m->nx_crypto_operation(NX_CRYPTO_HASH_INITIALIZE, handler, m,
                                    NX_NULL, 0, NX_NULL, 0, NX_NULL, NX_NULL, 0,
                                    (UCHAR *)metadata, sizeof(metadata), NX_NULL, NX_NULL);
    if (status != NX_CRYPTO_SUCCESS)
        return status;

    status = m->nx_crypto_operation(NX_CRYPTO_HASH_UPDATE, handler, m, NX_NULL,
                                    0, fc_client_random, 32, NX_NULL, NX_NULL, 0,
                                    (UCHAR *)metadata, sizeof(metadata), NX_NULL, NX_NULL);
    if (status != NX_CRYPTO_SUCCESS)
        return status;

    status = m->nx_crypto_operation(NX_CRYPTO_HASH_UPDATE, handler, m, NX_NULL,
                                    0, fc_server_random, 32, NX_NULL, NX_NULL, 0,
                                    (UCHAR *)metadata, sizeof(metadata), NX_NULL, NX_NULL);
    if (status != NX_CRYPTO_SUCCESS)
        return status;

    status = m->nx_crypto_operation(NX_CRYPTO_HASH_UPDATE, handler, m, NX_NULL,
                                    0, (UCHAR *)params, params_len, NX_NULL,
                                    NX_NULL, 0, metadata, sizeof(metadata),
                                    NX_NULL, NX_NULL);
    if (status != NX_CRYPTO_SUCCESS)
        return status;

    return m->nx_crypto_operation(NX_CRYPTO_HASH_CALCULATE, handler, m, NX_NULL,
                                  0, NX_NULL, 0, NX_NULL, out, 32, metadata,
                                  sizeof(metadata), NX_NULL, NX_NULL);
}

/*
 * PKCS#1 v1.5 over the SHA-256 DigestInfo, then the RSA private operation --
 * nx_secure_tls_send_certificate_verify.c's construction, spelled out here
 * because that function needs a packet and a whole session to reach.
 */
static UINT fc_sign(const unsigned char *params, unsigned params_len,
                    UCHAR out[FC_SIG_BYTES])
{
    static const UCHAR sha256_der[19] =
    {
        0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
        0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20
    };
    UCHAR  padded[FC_SIG_BYTES];
    UCHAR  hash[32];
    VOID  *handler = NX_NULL;
    UINT   status;

    status = fc_params_hash(params, params_len, hash);
    if (status != NX_CRYPTO_SUCCESS)
        return status;

    memset(padded, 0xFF, sizeof(padded));
    padded[0] = 0x00;
    padded[1] = 0x01;
    padded[FC_SIG_BYTES - 19 - 32 - 1] = 0x00;
    memcpy(&padded[FC_SIG_BYTES - 19 - 32], sha256_der, 19);
    memcpy(&padded[FC_SIG_BYTES - 32], hash, 32);

    status = fc_rsa->nx_crypto_init(
                 (NX_CRYPTO_METHOD *)fc_rsa,
                 (UCHAR *)fc_signer.nx_secure_x509_public_key.rsa_public_key.nx_secure_rsa_public_modulus,
                 (NX_CRYPTO_KEY_SIZE)(fc_signer.nx_secure_x509_public_key.rsa_public_key.nx_secure_rsa_public_modulus_length << 3),
                 &handler, fc_rsa_metadata, fc_rsa_metadata_size);
    if (status != NX_CRYPTO_SUCCESS)
        return status;

    status = fc_rsa->nx_crypto_operation(
                 NX_CRYPTO_DECRYPT, handler, (NX_CRYPTO_METHOD *)fc_rsa,
                 (UCHAR *)fc_signer.nx_secure_x509_private_key.rsa_private_key.nx_secure_rsa_private_exponent,
                 (NX_CRYPTO_KEY_SIZE)(fc_signer.nx_secure_x509_private_key.rsa_private_key.nx_secure_rsa_private_exponent_length << 3),
                 padded, FC_SIG_BYTES, NX_NULL, out, FC_SIG_BYTES,
                 fc_rsa_metadata, fc_rsa_metadata_size, NX_NULL, NX_NULL);
    if (status != NX_CRYPTO_SUCCESS)
        return status;

    if (fc_rsa->nx_crypto_cleanup)
        (void)fc_rsa->nx_crypto_cleanup(fc_rsa_metadata);

    return NX_CRYPTO_SUCCESS;
}

/* ------------------------------------------------------------- the seeds --- */

/*
 * secp256r1's generator, uncompressed: 0x04 || Gx || Gy. A point off the curve
 * is rejected by the ECDH import before it does any arithmetic, so a seed that
 * carries one stops a step short of the shared-secret calculation. This is a
 * point the curve actually has.
 */
static const unsigned char fc_p256_g[FC_POINT_BYTES] =
{
    0x04,
    0x6b, 0x17, 0xd1, 0xf2, 0xe1, 0x2c, 0x42, 0x47,
    0xf8, 0xbc, 0xe6, 0xe5, 0x63, 0xa4, 0x40, 0xf2,
    0x77, 0x03, 0x7d, 0x81, 0x2d, 0xeb, 0x33, 0xa0,
    0xf4, 0xa1, 0x39, 0x45, 0xd8, 0x98, 0xc2, 0x96,
    0x4f, 0xe3, 0x42, 0xe2, 0xfe, 0x1a, 0x7f, 0x9b,
    0x8e, 0xe7, 0xeb, 0x4a, 0x7c, 0x0f, 0x9e, 0x16,
    0x2b, 0xce, 0x33, 0x57, 0x6b, 0x31, 0x5e, 0xce,
    0xcb, 0xb6, 0x40, 0x68, 0x37, 0xbf, 0x51, 0xf5
};

/*
 * A ServerKeyExchange, signed. `key_len` bytes of ECDHE point are written
 * starting at offset 4, and the signature covers exactly the range the
 * verifier hashes -- packet_buffer[0 .. 4 + key_len).
 */
static void fcs_ske(FcBuf *w, unsigned curve, unsigned key_len, int sign_it)
{
    UCHAR    sig[FC_SIG_BYTES];
    unsigned i;

    fc_reset(w);
    fc_u8(w, 3);                        /* named_curve */
    fc_u16(w, curve);
    fc_u8(w, key_len);

    /* The generator, or as much of it as key_len allows; past its end, a
       pattern, because a longer point is one of the shapes worth sending. */
    for (i = 0; i < key_len; i++)
        fc_u8(w, (i < FC_POINT_BYTES) ? fc_p256_g[i] : (0x10 + (i & 0x7F)));

    if (sign_it && fc_sign(w->b, w->len, sig) != NX_CRYPTO_SUCCESS)
        sign_it = 0;

    fc_u8(w, NX_SECURE_TLS_HASH_ALGORITHM_SHA256);
    fc_u8(w, NX_SECURE_TLS_SIGNATURE_ALGORITHM_RSA);
    fc_u16(w, FC_SIG_BYTES);

    if (sign_it)
        fc_raw(w, sig, FC_SIG_BYTES);
    else
        fc_bytes(w, FC_SIG_BYTES, 0x5C);
}

static void fcs_ske_signed(FcBuf *w)
{
    fcs_ske(w, FC_CURVE_SECP256R1, FC_POINT_BYTES, 1);
}

/* The same message with a signature nobody made: the verify path, and nothing
   past it. */
static void fcs_ske_unsigned(FcBuf *w)
{
    fcs_ske(w, FC_CURVE_SECP256R1, FC_POINT_BYTES, 0);
}

/* A point of no length at all, signed, so the ECDH import is reached with
   nothing to import. */
static void fcs_ske_key_empty(FcBuf *w)
{
    fcs_ske(w, FC_CURVE_SECP256R1, 0, 1);
}

/* A point one byte short of a P-256 point, and one byte long. */
static void fcs_ske_key_short(FcBuf *w)
{
    fcs_ske(w, FC_CURVE_SECP256R1, FC_POINT_BYTES - 1, 1);
}

static void fcs_ske_key_long(FcBuf *w)
{
    fcs_ske(w, FC_CURVE_SECP256R1, FC_POINT_BYTES + 1, 1);
}

/* The longest point the one-byte length field can promise. */
static void fcs_ske_key_max(FcBuf *w)
{
    fcs_ske(w, FC_CURVE_SECP256R1, 255, 1);
}

/* A curve nobody offered. */
static void fcs_ske_bad_curve(FcBuf *w)
{
    fcs_ske(w, 0xBEEF, FC_POINT_BYTES, 0);
}

/* Not named_curve: the first byte the parser reads. */
static void fcs_ske_bad_format(FcBuf *w)
{
    fcs_ske_unsigned(w);
    w->b[0] = 1;
}

/* Exactly the four bytes the ECDHE arm demands before it reads a key length,
   and no more. */
static void fcs_ske_min(FcBuf *w)
{
    fc_reset(w);
    fc_u8(w, 3);
    fc_u16(w, FC_CURVE_SECP256R1);
    fc_u8(w, 0);
}

/* One byte short of that. */
static void fcs_ske_truncated(FcBuf *w)
{
    fcs_ske_min(w);
    w->len = 3;
}

/* A signature length that runs past the message. */
static void fcs_ske_sig_past_end(FcBuf *w)
{
    fcs_ske_unsigned(w);
    w->b[4 + FC_POINT_BYTES + 2] = 0xFF;
    w->b[4 + FC_POINT_BYTES + 3] = 0xFF;
}

/* A key length that swallows the rest of the message. */
static void fcs_ske_key_past_end(FcBuf *w)
{
    fcs_ske_unsigned(w);
    w->b[3] = 0xFF;
}

/*
 * A CertificateVerify, TLS 1.2 RSA shape: (hash, signature) algorithm, then a
 * 16-bit length, then the signature. The length has to equal the certificate's
 * modulus size or the parser rejects it before reading a signature byte, so
 * FC_SIG_BYTES is not arbitrary here.
 */
static void fcs_cv(FcBuf *w, unsigned claimed, unsigned present)
{
    fc_reset(w);
    fc_u8(w, NX_SECURE_TLS_HASH_ALGORITHM_SHA256);
    fc_u8(w, NX_SECURE_TLS_SIGNATURE_ALGORITHM_RSA);
    fc_u16(w, claimed);
    fc_bytes(w, present, 0xA5);
}

static void fcs_cv_full(FcBuf *w)
{
    fcs_cv(w, FC_SIG_BYTES, FC_SIG_BYTES);
}

/* The signature is as long as the certificate's modulus and the message stops
   four bytes short of holding it -- length is checked against message_length,
   which does not account for the four-byte header in front of it. */
static void fcs_cv_exact(FcBuf *w)
{
    fcs_cv(w, FC_SIG_BYTES, FC_SIG_BYTES - 4);
}

/* A length the modulus check rejects. */
static void fcs_cv_short(FcBuf *w)
{
    fcs_cv(w, 16, 16);
}

/* The header and nothing else. */
static void fcs_cv_header_only(FcBuf *w)
{
    fcs_cv(w, FC_SIG_BYTES, 0);
}

/* Fewer bytes than the four the parser reads before it checks anything. */
static void fcs_cv_stub(FcBuf *w)
{
    fc_reset(w);
    fc_u8(w, NX_SECURE_TLS_HASH_ALGORITHM_SHA256);
}

/* A Finished, at the only length TLS 1.2 accepts. */
static void fcs_finished(FcBuf *w)
{
    fc_reset(w);
    fc_bytes(w, NX_SECURE_TLS_FINISHED_HASH_SIZE, 0x33);
}

static void fcs_finished_short(FcBuf *w)
{
    fcs_finished(w);
    w->len = NX_SECURE_TLS_FINISHED_HASH_SIZE - 1;
}

static void fcs_finished_long(FcBuf *w)
{
    fcs_finished(w);
    fc_bytes(w, 8, 0x44);
}

static void fcs_empty(FcBuf *w)
{
    fc_reset(w);
}

static void fcs_ones(FcBuf *w)
{
    fc_reset(w);
    fc_bytes(w, 512, 0xFF);
}

/* Which parser a seed is for. A ServerKeyExchange is not a Finished and
   feeding one to the other only tests the length gate, so each seed names its
   own message -- and the sweep still crosses them, because every mutation is
   run through all three. */
#define FC_SKE          0
#define FC_CV           1
#define FC_FIN          2

typedef void (*FcSeedFn)(FcBuf *);

typedef struct
{
    const char *name;
    FcSeedFn    build;
    int         message;
} FcSeed;

static const FcSeed fc_seeds[] =
{
    { "ske_signed",      fcs_ske_signed,    FC_SKE },
    { "ske_unsigned",    fcs_ske_unsigned,  FC_SKE },
    { "ske_key_empty",   fcs_ske_key_empty, FC_SKE },
    { "ske_key_short",   fcs_ske_key_short, FC_SKE },
    { "ske_key_long",    fcs_ske_key_long,  FC_SKE },
    { "ske_key_max",     fcs_ske_key_max,   FC_SKE },
    { "ske_bad_curve",   fcs_ske_bad_curve, FC_SKE },
    { "ske_bad_format",  fcs_ske_bad_format, FC_SKE },
    { "ske_min",         fcs_ske_min,       FC_SKE },
    { "ske_truncated",   fcs_ske_truncated, FC_SKE },
    { "ske_sig_past_end", fcs_ske_sig_past_end, FC_SKE },
    { "ske_key_past_end", fcs_ske_key_past_end, FC_SKE },
    { "cv_full",         fcs_cv_full,       FC_CV  },
    { "cv_exact",        fcs_cv_exact,      FC_CV  },
    { "cv_short",        fcs_cv_short,      FC_CV  },
    { "cv_header_only",  fcs_cv_header_only, FC_CV },
    { "cv_stub",         fcs_cv_stub,       FC_CV  },
    { "finished",        fcs_finished,      FC_FIN },
    { "finished_short",  fcs_finished_short, FC_FIN },
    { "finished_long",   fcs_finished_long, FC_FIN },
    { "empty",           fcs_empty,         FC_SKE },
    { "ones",            fcs_ones,          FC_SKE }
};

#define FC_SEED_COUNT   (int)(sizeof(fc_seeds) / sizeof(fc_seeds[0]))

/* ------------------------------------------------------- the reach counts -- */

/*
 * "Clean" means nothing if the sweep never got past a length check, so every
 * case is measured rather than assumed. The crypto counts come from
 * ami_tls_crypto.c's own instrumentation -- the code under test reporting that
 * one of its NX_CRYPTO_METHOD entries ran -- and the rest from the status,
 * which for these three functions names how far the parse got.
 */
static unsigned long fc_n_ske;          /* ServerKeyExchange calls           */
static unsigned long fc_n_ske_crypto;   /* ... that ran a public-key op      */
static unsigned long fc_n_ske_dh;       /* ... that completed the ECDH       */
static unsigned long fc_n_cv;
static unsigned long fc_n_cv_crypto;
static unsigned long fc_n_fin;
static unsigned long fc_n_fin_prf;      /* ... that reached the Finished PRF */

static ULONG fc_crypto_ops(void)
{
    AMI_TLS_CRYPTO_COUNTERS a;
    AMI_TLS_CRYPTO_COUNTERS b;

    ami_tls_crypto_counters_get(&a, &b);

    return a.ami_rsa_public_count + a.ami_rsa_private_crt_count +
           a.ami_rsa_private_plain_count + a.ami_ec_multiple_count +
           b.ami_rsa_public_count + b.ami_rsa_private_crt_count +
           b.ami_rsa_private_plain_count + b.ami_ec_multiple_count;
}

/* ------------------------------------------------------------- the driver -- */

/*
 * One message, in an allocation sized to itself. See the length-contract note
 * at the top: this is the record payload, not a scratch buffer, so a parser
 * that reads past the message lands in the redzone.
 */
static void fc_run(int message, const unsigned char *msg, unsigned len)
{
    NX_SECURE_TLS_SESSION s;
    UCHAR                *payload;
    ULONG                 before;
    UINT                  status;

    if (len > FC_MAX)
        len = FC_MAX;

    if (fc_session_open(&s, TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256) != NX_SUCCESS)
        return;

    /* malloc(0) may or may not return a block; a zero-length message is still
       worth driving, so it gets one byte it is not allowed to read. */
    payload = (UCHAR *)malloc(len == 0 ? 1u : len);
    if (payload == NX_NULL)
    {
        fc_session_close(&s);
        return;
    }
    if (len > 0)
        memcpy(payload, msg, len);

    before = fc_crypto_ops();

    switch (message)
    {
    case FC_SKE:
        fc_n_ske++;
        status = _nx_secure_tls_process_server_key_exchange(&s, payload, len);
        if (fc_crypto_ops() != before)
            fc_n_ske_crypto++;
        if (status == NX_SUCCESS)
            fc_n_ske_dh++;
        break;

    case FC_CV:
        fc_n_cv++;
        (void)_nx_secure_tls_process_certificate_verify(&s, payload, len);
        if (fc_crypto_ops() != before)
            fc_n_cv_crypto++;
        break;

    default:
        /* A Finished only means anything once the peer's ChangeCipherSpec has
           switched the read side over and its credentials have been accepted;
           the parser refuses one before that, so the flags are set here the
           way the state machine would have set them. */
        s.nx_secure_tls_remote_session_active       = 1;
        s.nx_secure_tls_received_remote_credentials = 1;

        fc_n_fin++;
        status = _nx_secure_tls_process_finished(&s, payload, len);
        if (status == NX_SECURE_TLS_FINISHED_HASH_FAILURE ||
            status == NX_SECURE_TLS_SUCCESS)
        {
            fc_n_fin_prf++;
        }
        break;
    }

    free(payload);
    fc_session_close(&s);
}

/* ---------------------------------------------------------- the selftest --- */

static void fc_fail(const char *what)
{
    printf("fuzz_tls_crypto: SELFTEST FAILED -- %s\n", what);
    exit(2);
}

/*
 * Proof that the driver reaches the parsers.
 *
 * A driver that stopped at the first length check -- a ciphersuite that never
 * resolved, a certificate that never reached the store -- would report "clean"
 * for every input and read as coverage. So each of the three is run on a case
 * it must get all the way through, and the thing it must have produced is
 * checked by name:
 *
 *   ServerKeyExchange   a signed message must return NX_SUCCESS and leave a
 *                       pre-master secret, which only the completed ECDH does
 *   CertificateVerify   must run a public-key operation, which is past the
 *                       algorithm check, the modulus-length check and the
 *                       signature bounds
 *   Finished            a 12-byte message must reach the PRF and fail the
 *                       comparison; an 11-byte one must be refused for length
 */
static void fc_selftest(void)
{
    NX_SECURE_TLS_SESSION s;
    FcBuf                 w;
    UCHAR                *payload;
    ULONG                 before;
    UINT                  status;

    /* ---- ServerKeyExchange ---- */

    fcs_ske_signed(&w);

    if (fc_session_open(&s, TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256) != NX_SUCCESS)
        fc_fail("session would not open");

    if (s.nx_secure_tls_session_ciphersuite == NX_NULL)
        fc_fail("ciphersuite not resolved");

    payload = (UCHAR *)malloc(w.len);
    if (payload == NX_NULL)
        fc_fail("out of memory");
    memcpy(payload, w.b, w.len);

    status = _nx_secure_tls_process_server_key_exchange(&s, payload, w.len);
    free(payload);

    if (status != NX_SUCCESS)
    {
        printf("fuzz_tls_crypto: SELFTEST FAILED -- signed ServerKeyExchange"
               " returned %u\n", (unsigned)status);
        exit(2);
    }

    if (s.nx_secure_tls_key_material.nx_secure_tls_pre_master_secret_size == 0)
        fc_fail("pre_master_secret_size still zero after ServerKeyExchange");

    if (s.nx_secure_tls_client_state !=
            NX_SECURE_TLS_CLIENT_STATE_SERVER_KEY_EXCHANGE)
        fc_fail("client_state not advanced to SERVER_KEY_EXCHANGE");

    fc_session_close(&s);

    /* ---- CertificateVerify ---- */

    fcs_cv_full(&w);

    if (fc_session_open(&s, TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256) != NX_SUCCESS)
        fc_fail("session would not open");

    payload = (UCHAR *)malloc(w.len);
    if (payload == NX_NULL)
        fc_fail("out of memory");
    memcpy(payload, w.b, w.len);

    before = fc_crypto_ops();
    (void)_nx_secure_tls_process_certificate_verify(&s, payload, w.len);
    free(payload);

    if (fc_crypto_ops() == before)
        fc_fail("CertificateVerify ran no public-key operation");

    fc_session_close(&s);

    /* ---- Finished ---- */

    if (fc_session_open(&s, TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256) != NX_SUCCESS)
        fc_fail("session would not open");

    s.nx_secure_tls_remote_session_active       = 1;
    s.nx_secure_tls_received_remote_credentials = 1;

    fcs_finished(&w);
    payload = (UCHAR *)malloc(w.len);
    if (payload == NX_NULL)
        fc_fail("out of memory");
    memcpy(payload, w.b, w.len);

    status = _nx_secure_tls_process_finished(&s, payload, w.len);
    free(payload);

    if (status != NX_SECURE_TLS_FINISHED_HASH_FAILURE)
    {
        printf("fuzz_tls_crypto: SELFTEST FAILED -- 12-byte Finished returned"
               " %u, expected the hash comparison to fail\n", (unsigned)status);
        exit(2);
    }

    fcs_finished_short(&w);
    payload = (UCHAR *)malloc(w.len);
    if (payload == NX_NULL)
        fc_fail("out of memory");
    memcpy(payload, w.b, w.len);

    status = _nx_secure_tls_process_finished(&s, payload, w.len);
    free(payload);

    if (status != NX_SECURE_TLS_INCORRECT_MESSAGE_LENGTH)
        fc_fail("11-byte Finished was not refused for length");

    fc_session_close(&s);

    /* The counts above are the selftest's, not the sweep's. */
    fc_n_ske = fc_n_ske_crypto = fc_n_ske_dh = 0;
    fc_n_cv  = fc_n_cv_crypto  = 0;
    fc_n_fin = fc_n_fin_prf    = 0;
}

/* ---------------------------------------------------------------- setup ---- */

static void fc_env_init(void)
{
    const NX_CRYPTO_METHOD *rsa;
    UINT                    status;
    unsigned                i;

    for (i = 0; i < 32; i++)
    {
        fc_client_random[i] = (UCHAR)(0x10 + i);
        fc_server_random[i] = (UCHAR)(0xA0 + i);
    }

    /* nx_secure owns _nx_secure_tls_protection here rather than the driver
       declaring one, because linking the whole archive means
       nx_secure_tls_initialize.c is in it. The mutex it creates is uncontended
       by construction -- one thread, nothing suspends. */
    _nx_secure_tls_initialize();

    status = ami_tls_crypto_initialize();
    if (status != NX_SUCCESS)
    {
        printf("fuzz_tls_crypto: ami_tls_crypto_initialize() returned %u\n",
               (unsigned)status);
        exit(2);
    }

    status = _nx_secure_tls_metadata_size_calculate(&ami_crypto_tls_ciphers_ecc,
                                                    &fc_metadata_size);
    if (status != NX_SUCCESS || fc_metadata_size == 0)
        fc_fail("metadata size could not be calculated");

    fc_metadata   = (UCHAR *)malloc(fc_metadata_size);
    fc_record     = (UCHAR *)malloc(FC_RECORD_BUFFER);
    fc_remote_der = (UCHAR *)malloc(FC_CHAIN * FC_REMOTE_DER_MAX);

    if (fc_metadata == NX_NULL || fc_record == NX_NULL ||
        fc_remote_der == NX_NULL)
    {
        fc_fail("out of memory");
    }

    /* The signer: the sample leaf, with its private key, and the primes
       registered so the signature is a CRT operation rather than a full-width
       one. Nothing about the messages changes either way; it is the difference
       between a startup that takes a moment and one that is noticed. */
    rsa = fc_suite_auth(TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256);
    if (rsa == NX_NULL)
        fc_fail("no RSA auth method in the ciphersuite table");
    fc_rsa = rsa;

    fc_rsa_metadata_size = rsa->nx_crypto_metadata_area_size;
    fc_rsa_metadata      = (UCHAR *)malloc(fc_rsa_metadata_size);
    if (fc_rsa_metadata == NX_NULL)
        fc_fail("out of memory");

    memset(&fc_signer, 0, sizeof(fc_signer));
    status = _nx_secure_x509_certificate_initialize(
                 &fc_signer, (UCHAR *)test_device_cert_der,
                 (USHORT)test_device_cert_der_len, NX_NULL, 0,
                 test_device_cert_key_der,
                 (USHORT)test_device_cert_key_der_len,
                 NX_SECURE_X509_KEY_TYPE_RSA_PKCS1_DER);
    if (status != NX_SUCCESS)
        fc_fail("the sample leaf's private key would not parse");

    (void)ami_tls_rsa_key_register(&fc_signer);
}

/* --------------------------------------------------------- the mutations --- */

static unsigned long fc_state = 1;

static unsigned fc_rand(void)
{
    fc_state = fc_state * 1103515245UL + 12345UL;
    return (unsigned)((fc_state >> 16) & 0x7FFFUL);
}

static unsigned fc_below(unsigned n)
{
    return (n == 0) ? 0 : (fc_rand() % n);
}

/*
 * Aimed at the bytes that decide how far a walk goes -- the curve id, the
 * one-byte key length, the two-byte signature length, the algorithm pair --
 * rather than at the signature, where a flipped byte only fails a comparison
 * that was going to fail anyway.
 */
static void fc_mutate(FcBuf *w)
{
    unsigned rounds = fc_below(6) + 1u;

    while (rounds-- > 0)
    {
        unsigned at;

        if (w->len == 0)
            return;

        switch (fc_below(9))
        {
        case 0:     /* any byte */
            w->b[fc_below(w->len)] = (unsigned char)fc_rand();
            break;
        case 1:     /* a byte to its maximum */
            w->b[fc_below(w->len)] = 0xFF;
            break;
        case 2:     /* a byte to zero */
            w->b[fc_below(w->len)] = 0;
            break;
        case 3:     /* the key length, which sets where everything after starts */
            if (w->len > 3)
                w->b[3] = (unsigned char)fc_rand();
            break;
        case 4:     /* a 16-bit field anywhere */
            if (w->len > 2)
            {
                at = fc_below(w->len - 2);
                w->b[at]     = (unsigned char)fc_rand();
                w->b[at + 1] = (unsigned char)fc_rand();
            }
            break;
        case 5:
            /*
             * A ServerKeyExchange head the ECDHE arm will accept, most of the
             * time. A uniformly random one is rejected on the first byte
             * roughly 255 times in 256, and a sweep that never gets past the
             * first byte is a sweep of the first byte.
             */
            if (w->len > 3)
            {
                static const unsigned short curves[] =
                    { FC_CURVE_SECP256R1, 24, 25, 29, 0, 0xFFFF };

                unsigned c = curves[fc_below((unsigned)(sizeof(curves) /
                                                        sizeof(curves[0])))];

                w->b[0] = (unsigned char)((fc_below(4) == 0) ? fc_rand() : 3);
                w->b[1] = (unsigned char)(c >> 8);
                w->b[2] = (unsigned char)c;
            }
            break;
        case 6:
            /*
             * A CertificateVerify head, likewise: the algorithm pair has to be
             * (SHA-256, RSA) and the length has to equal the certificate's
             * modulus before a signature byte is read at all. The lengths
             * offered are the ones the bounds arithmetic turns on -- the
             * modulus, its neighbours, and the message's own size.
             */
            if (w->len > 3)
            {
                unsigned pick = fc_below(6);
                unsigned n;

                switch (pick)
                {
                case 0:  n = FC_SIG_BYTES;          break;
                case 1:  n = FC_SIG_BYTES - 1;      break;
                case 2:  n = FC_SIG_BYTES + 1;      break;
                case 3:  n = w->len;                break;
                case 4:  n = w->len - 4;            break;
                default: n = 0xFFFF;                break;
                }

                w->b[0] = NX_SECURE_TLS_HASH_ALGORITHM_SHA256;
                w->b[1] = NX_SECURE_TLS_SIGNATURE_ALGORITHM_RSA;
                w->b[2] = (unsigned char)(n >> 8);
                w->b[3] = (unsigned char)n;
            }
            break;
        case 7:     /* truncate */
            w->len = fc_below(w->len) + 1u;
            break;
        default:    /* extend, with whatever the buffer already held */
            if (w->len < FC_MAX)
                w->len += fc_below(FC_MAX - w->len);
            break;
        }
    }
}

static void fc_run_seed(int s)
{
    FcBuf w;

    fc_seeds[s].build(&w);
    fc_run(fc_seeds[s].message, w.b, w.len);
}

static void fc_report(const char *prefix)
{
    printf("%s reached: ServerKeyExchange %lu/%lu crypto, %lu ECDH;"
           " CertificateVerify %lu/%lu crypto; Finished %lu/%lu PRF\n",
           prefix, fc_n_ske_crypto, fc_n_ske, fc_n_ske_dh,
           fc_n_cv_crypto, fc_n_cv, fc_n_fin_prf, fc_n_fin);
}

int main(int argc, char **argv)
{
    int i;

    fc_env_init();
    fc_selftest();

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-s") == 0)
        {
            int s;

            for (s = 0; s < FC_SEED_COUNT; s++)
            {
                fc_run_seed(s);
                printf("  %-18s ok\n", fc_seeds[s].name);
            }

            printf("fuzz_tls_crypto: %d seed case(s), clean\n", FC_SEED_COUNT);
            fc_report("fuzz_tls_crypto:");
            return 0;
        }

        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
        {
            const char *want = argv[++i];
            int         s;

            for (s = 0; s < FC_SEED_COUNT; s++)
            {
                if (strcmp(fc_seeds[s].name, want) == 0)
                {
                    fc_run_seed(s);
                    printf("fuzz_tls_crypto: seed '%s', clean\n", want);
                    return 0;
                }
            }

            printf("fuzz_tls_crypto: no seed case named '%s'\n", want);
            return 2;
        }

        if (strcmp(argv[i], "-r") == 0 && i + 2 < argc)
        {
            unsigned long seed  = strtoul(argv[++i], NULL, 0);
            unsigned long count = strtoul(argv[++i], NULL, 0);
            unsigned long n;
            int           s;

            /* The seeds first, always: a sweep that never runs the known
               shapes is a sweep whose coverage nobody can state. */
            for (s = 0; s < FC_SEED_COUNT; s++)
                fc_run_seed(s);

            fc_state = seed ? seed : 1;

            for (n = 0; n < count; n++)
            {
                FcBuf w;
                int   pick = (int)fc_below((unsigned)FC_SEED_COUNT);

                fc_seeds[pick].build(&w);
                fc_mutate(&w);

                /* Every mutation through all three, not only the parser its
                   seed was written for: a ServerKeyExchange arriving where a
                   Finished was expected is something a server can send. */
                fc_run(FC_SKE, w.b, w.len);
                fc_run(FC_CV,  w.b, w.len);
                fc_run(FC_FIN, w.b, w.len);
            }

            printf("fuzz_tls_crypto: %d seed(s) + %lu mutation(s) from %lu,"
                   " clean\n", FC_SEED_COUNT, count, seed);
            fc_report("fuzz_tls_crypto:");
            return 0;
        }
    }

    /* One ServerKeyExchange on stdin. */
    {
        unsigned char buf[FC_MAX];
        size_t        got = fread(buf, 1, sizeof(buf), stdin);

        fc_run(FC_SKE, buf, (unsigned)got);
    }

    return 0;
}
