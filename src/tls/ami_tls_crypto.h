/*
 * AmiNetXDuo -- the nx_secure crypto table that actually uses src/crypto68k/.
 *
 *   src/crypto68k/ made RSA-2048 2.9x/2.2x faster and P-256 3.6-3.9x faster,
 *   measured, and none of it reached a handshake: nothing outside
 *   src/crypto68k/ referenced the module, so tests/tls/tls_handshake ran
 *   entirely on the vendored arithmetic.  This file is the wire.
 *
 *   nx_secure already has an application-supplied extension point and it is
 *   exactly the right shape.  nx_secure_tls_session_create() takes a
 *   `const NX_SECURE_TLS_CRYPTO *` -- a table of NX_CRYPTO_METHOD pointers --
 *   and nx_secure_tls_ecc_initialize() takes an application-supplied array of
 *   curve methods.  Every path that reaches a big-number operation goes through
 *   one of those two, so supplying our own tables redirects all of them with no
 *   vendored source touched:
 *
 *     - RSA:  ami_crypto_method_rsa replaces crypto_method_rsa in both the
 *             ciphersuite table (public cipher and public auth) and the X.509
 *             signature table.  Its operation is the vendored dispatch with
 *             _nx_crypto_rsa_operation() replaced by one that calls
 *             c68k_crt_power_modulus() / c68k_huge_number_mont_power_modulus().
 *
 *     - P-256: ami_crypto_method_ec_secp256 replaces crypto_method_ec_secp256
 *             in the curve array.  ECDH and ECDSA obtain their curve ONLY by
 *             calling NX_CRYPTO_EC_CURVE_GET on whichever curve method they
 *             were handed (verified: every _nx_secure_tls_find_curve_method
 *             and _nx_secure_x509_find_curve_method site indexes the array the
 *             application passed to nx_secure_tls_ecc_initialize), and they
 *             hold the returned pointer rather than copying the struct.  So a
 *             curve method that returns a private, MUTABLE copy of
 *             _nx_crypto_ec_secp256r1 with nx_crypto_ec_multiple swapped is
 *             the whole integration.
 *
 *   The alternative -- casting away the `const` on _nx_crypto_ec_secp256r1 and
 *   writing to it -- was rejected.  It is undefined behaviour, the object is
 *   shared by every session in the process, and it would silently defeat the
 *   differential tests that check us against the vendored path.  Our own copy
 *   costs one NX_CRYPTO_EC (about 200 bytes) and leaves the reference intact,
 *   which is what tests/crypto68k needs and what the interop test below needs.
 *
 *   NX_CRYPTO_SET_PRIME_P appears exactly once in all of nx_secure/src, in
 *   nx_secure_process_client_key_exchange.c.  The ECDHE_RSA ServerKeyExchange
 *   signature (nx_secure_tls_ecc_generate_keys.c:773) and
 *   nx_secure_tls_send_certificate_verify.c:670 both hand the full 2048-bit
 *   private exponent to the RSA method with p and q left NULL, so they take the
 *   non-CRT path -- a measured 3.6x penalty on the operation that is most of a
 *   server handshake.
 *
 *   We cannot add the two missing nx_crypto_operation() calls without editing
 *   vendored sources, so the primes are supplied from the other end instead:
 *   ami_tls_rsa_key_register() records (modulus, p, q) from a parsed
 *   certificate, and our RSA operation, when asked for a private-key
 *   exponentiation with no p/q set, looks the modulus up and uses CRT anyway.
 *   The pairing comes from one certificate object, so it cannot be mismatched
 *   any more than the vendored CRT path's can.
 *
 * CONSTANT TIME: NO -- see the note above c68k_mont_power_modulus().  This
 * changes nothing about that; both the vendored and the fast exponentiation
 * branch on exponent bits.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_TLS_CRYPTO_H
#define AMINETXDUO_TLS_CRYPTO_H

#include "nx_secure_tls.h"
#include "nx_secure_x509.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * RFC 7905's two ciphersuite numbers.  nx_secure_tls.h has a define for every
 * suite the vendored tables mention and these were never among them, because
 * nx_secure has no ChaCha20-Poly1305 at all -- src/crypto68k/c68k_chacha20.c
 * is where it now comes from.
 */
#define TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256     0xCCA8
#define TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256   0xCCA9

/*
 * The drop-in replacements for the vendored ECC ciphersuite tables, with
 * crypto_method_rsa and crypto_method_ec_secp256 swapped for ours and the two
 * ChaCha20-Poly1305 suites added at the head of the list.
 *
 *     nx_secure_tls_session_create(&session, &ami_crypto_tls_ciphers_ecc, ...)
 *     nx_secure_tls_ecc_initialize(&session, ami_crypto_ecc_supported_groups,
 *                                  ami_crypto_ecc_supported_groups_size,
 *                                  ami_crypto_ecc_curves);
 */
extern const NX_SECURE_TLS_CRYPTO   ami_crypto_tls_ciphers_ecc;
extern const USHORT                 ami_crypto_ecc_supported_groups[];
extern const NX_CRYPTO_METHOD      *ami_crypto_ecc_curves[];
extern const UINT                   ami_crypto_ecc_supported_groups_size;

/*
 * Build the private secp256r1 curve and, if AMINETXDUO_TLS_CRYPTO68K_SELFCHECK
 * is set, verify the generated comb table belongs to it.  Call once, before any
 * session is created; it is idempotent but not thread safe, which is the same
 * contract nx_secure_tls_initialize() has.
 *
 * Returns NX_SUCCESS, or NX_NOT_SUCCESSFUL if the curve could not be found or
 * the self check failed -- in which case the tables still work, they just fall
 * through to the vendored arithmetic.
 */
UINT ami_tls_crypto_initialize(VOID);

/*
 * Record a certificate's RSA primes so that private-key operations against its
 * modulus can use CRT.  Call after nx_secure_x509_certificate_initialize() and
 * before the handshake; the certificate must outlive the sessions that use it,
 * because nothing is copied (the vendored CRT path aliases the same memory).
 *
 * Returns NX_SUCCESS, NX_NOT_SUCCESSFUL if the certificate carries no RSA
 * private primes (an EC certificate, or a PKCS#1 key without p and q), or
 * NX_NO_MORE_ENTRIES if the table is full.
 */
UINT ami_tls_rsa_key_register(const NX_SECURE_X509_CERT *certificate);

/* Forget every registered key.  Private-key operations then fall back to the
   full-width exponentiation, which is correct and 3.6x slower. */
VOID ami_tls_rsa_key_reset(VOID);

/*
 * nx_secure_tls_local_certificate_add() plus ami_tls_rsa_key_register(), which
 * is what a server or a client with a client certificate actually wants.
 * Returns the certificate-add status; a failure to register the primes is not
 * an error, it is only slower.
 */
UINT ami_tls_local_certificate_add(NX_SECURE_TLS_SESSION *tls_session,
                                   NX_SECURE_X509_CERT *certificate);

/* ------------------------------------------------ measurement and modes --- */

/*
 * Which arithmetic the methods above use.  Both settings compute the same
 * values; REFERENCE exists so that "before" and "after" can be measured through
 * IDENTICAL instrumentation in one process, which is the only way to get a
 * decomposition that is measured rather than composed from separate runs.
 *
 * Not for production use -- the shipping configuration is C68K, which is the
 * default and does not need to be set.
 */
#define AMI_TLS_ARITH_C68K          0u
#define AMI_TLS_ARITH_REFERENCE     1u

VOID ami_tls_crypto_set_arithmetic(UINT mode);

/*
 * Whether a private-key operation with no primes set may recover them from
 * ami_tls_rsa_key_register().  ON by default; OFF reproduces stock nx_secure
 * behaviour, which is what isolates the CRT win from the crypto68k win.
 */
VOID ami_tls_crypto_set_crt(UINT enable);

/*
 * Per-operation counters and elapsed microseconds, so a test can prove which
 * path ran and how long it took rather than inferring both from one stopwatch.
 *
 * Timing comes from ami_tls_eclock(); if ami_tls_timer_open() was never called
 * the counts are still right and the microseconds are zero.
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
 * Split the counters by role.  `client` accumulates work done on the thread
 * registered with ami_tls_crypto_set_client_thread(); `other` is everything
 * else, which in the loopback handshake is the server.  Either pointer may be
 * NX_NULL.
 *
 * This is what produces the client-only figure -- a client fetching a page
 * never performs the private-key operation, so the whole-machine wall time of a
 * loopback handshake overstates its cost by an order of magnitude.
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
