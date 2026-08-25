/* AmiNetXDuo: nx_secure crypto tables routing RSA and P-256 through src/crypto68k/
 * as replacement NX_CRYPTO_METHOD tables; the vendored const curve object is
 * shared process-wide and must never be written.  SPDX-License-Identifier: MIT */

#ifndef AMINETXDUO_TLS_CRYPTO_H
#define AMINETXDUO_TLS_CRYPTO_H

#include "nx_secure_tls.h"
#include "nx_secure_x509.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * RFC 7905's two ciphersuite numbers.  nx_secure has no ChaCha20-Poly1305 at
 * all, so nx_secure_tls.h defines neither; the cipher is src/crypto68k's.
 */
#define TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256     0xCCA8
#define TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256   0xCCA9

/*
 * Drop-in replacements for the vendored ECC tables: ami_crypto_tls_ciphers_ecc
 * goes to nx_secure_tls_session_create(), and the ami_crypto_ecc_* trio to
 * nx_secure_tls_ecc_initialize().
 */
extern const NX_SECURE_TLS_CRYPTO   ami_crypto_tls_ciphers_ecc;
extern const USHORT                 ami_crypto_ecc_supported_groups[];
extern const NX_CRYPTO_METHOD      *ami_crypto_ecc_curves[];
extern const UINT                   ami_crypto_ecc_supported_groups_size;

/*
 * The subset the ClientHello offers.  nx_secure_tls_ecc_initialize() sets the
 * TLS and X.509 lists from one argument; a client wants them apart, because
 * offering a group costs a key generation and recognising one does not.
 */
extern const USHORT                 ami_crypto_ecc_offered_groups[];
extern const NX_CRYPTO_METHOD      *ami_crypto_ecc_offered_curves[];
extern const UINT                   ami_crypto_ecc_offered_groups_size;

/*
 * Build the private secp256r1 curve.  Call once, before any session is created;
 * idempotent but not thread safe, matching nx_secure_tls_initialize().  On
 * failure the tables still work and fall through to the vendored arithmetic.
 */
UINT ami_tls_crypto_initialize(VOID);

/*
 * Record a certificate's RSA primes so private-key operations on its modulus
 * can use CRT.  Call after nx_secure_x509_certificate_initialize() and before
 * the handshake; nothing is copied, so the certificate must outlive the sessions.
 */
UINT ami_tls_rsa_key_register(const NX_SECURE_X509_CERT *certificate);

/* Forget every registered key.  Private-key operations then fall back to the
   full-width exponentiation, which is correct and 3.6x slower. */
VOID ami_tls_rsa_key_reset(VOID);

/*
 * nx_secure_tls_local_certificate_add() plus ami_tls_rsa_key_register().  Returns
 * the certificate-add status; failing to register the primes is only slower.
 */
UINT ami_tls_local_certificate_add(NX_SECURE_TLS_SESSION *tls_session,
                                   NX_SECURE_X509_CERT *certificate);

/* ------------------------------------------------ measurement and modes --- */

/*
 * Which arithmetic the methods above use; both settings compute the same values.
 * Not for production use -- the shipping configuration is C68K, the default.
 */
#define AMI_TLS_ARITH_C68K          0u
#define AMI_TLS_ARITH_REFERENCE     1u

VOID ami_tls_crypto_set_arithmetic(UINT mode);

/*
 * Whether a private-key operation with no primes set may recover them from
 * ami_tls_rsa_key_register().  On by default.
 */
VOID ami_tls_crypto_set_crt(UINT enable);

/*
 * Per-operation counters and elapsed microseconds.  Timing comes from
 * ami_tls_eclock(): if ami_tls_timer_open() was never called the counts are
 * still right and the microseconds are zero.
 */
typedef struct AMI_TLS_CRYPTO_COUNTERS_STRUCT
{
    ULONG   ami_rsa_public_count;           /* short exponent               */
    ULONG   ami_rsa_public_us;
    ULONG   ami_rsa_private_crt_count;      /* long exponent, CRT taken     */
    ULONG   ami_rsa_private_crt_us;
    ULONG   ami_rsa_private_plain_count;    /* long exponent, no primes     */
    ULONG   ami_rsa_private_plain_us;
    ULONG   ami_ec_multiple_count;          /* every k*P through our curve  */
    ULONG   ami_ec_multiple_us;
} AMI_TLS_CRYPTO_COUNTERS;

/*
 * Split by role: `client` accumulates work done on the thread registered with
 * ami_tls_crypto_set_client_thread(), `other` is everything else.  Either
 * pointer can be NX_NULL.
 */
VOID ami_tls_crypto_counters_get(AMI_TLS_CRYPTO_COUNTERS *client,
                                 AMI_TLS_CRYPTO_COUNTERS *other);
VOID ami_tls_crypto_counters_reset(VOID);

/* Pass a TX_THREAD *.  VOID * so that this header does not drag in tx_api.h. */
VOID ami_tls_crypto_set_client_thread(VOID *thread);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_TLS_CRYPTO_H */
