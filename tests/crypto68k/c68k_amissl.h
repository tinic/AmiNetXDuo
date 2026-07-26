/*
 * AmiNetXDuo -- the AmiSSL half of the crypto68k-versus-AmiSSL benchmark.
 *
 * Two translation units, not one, and the split is deliberate.  nx_crypto's
 * headers and OpenSSL's headers both want to own names like SHA256_CTX and
 * both arrive through <exec/types.h>, which defines BOOL, LONG and VOID for
 * everyone.  Keeping them in separate files means neither has to be taught
 * about the other, and this header -- plain C types only, no nx_crypto and no
 * OpenSSL -- is the whole interface between them.
 *
 * Every function returns 0 for success and non-zero for failure, so the
 * orchestrator can check without knowing what an OpenSSL error looks like.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_C68K_AMISSL_H
#define AMINETXDUO_C68K_AMISSL_H

/* --------------------------------------------------------- library open -- */

/*
 * Opens amisslmaster.library and the versioned amissl library behind it.
 * `err` receives a human-readable reason on failure.
 *
 * Split in two because the two halves have wildly different costs and the
 * first version of this program looked hung for minutes with no way to tell
 * which: LoadSeg of a 3.5 MB library and OpenSSL 3.x's own initialisation are
 * both slow on a 14 MHz 68020, and a benchmark that cannot say which stage it
 * is in is a benchmark nobody can debug.  The caller times and logs both.
 */
int         a_ossl_open_master(char *err, unsigned long err_len);
int         a_ossl_open_ssl(char *err, unsigned long err_len);
int         a_ossl_init_ssl(char *err, unsigned long err_len);
void        a_ossl_close(void);

/* "OpenSSL 3.6.2 ..." from the library itself, never from a header constant --
   the point is to record what actually ran. */
const char *a_ossl_version(void);
/* Which CPU build of amissl_v*.library got opened, from its version string. */
const char *a_ossl_library_id(void);

/* ------------------------------------------------------------------ RSA -- */

/*
 * n, d are 256 bytes big-endian; p, q are 128; e is the 3-byte 0x010001.
 * dmp1, dmq1 and iqmp are derived here rather than passed in, because the
 * vectors this benchmark shares with tests/crypto68k do not carry them and
 * OpenSSL's CRT path refuses to run without them.
 */
int a_ossl_rsa_setup(const unsigned char *n, const unsigned char *e,
                     const unsigned char *d, const unsigned char *p,
                     const unsigned char *q, int blinding);
void a_ossl_rsa_free(void);

/* in and out are RSA_size() == 256 bytes.  RSA_NO_PADDING, so these are the
   raw modular exponentiations and nothing else. */
int a_ossl_rsa_public(const unsigned char *in, unsigned char *out);
int a_ossl_rsa_private(const unsigned char *in, unsigned char *out);

/* The bare bignum exponentiation, for the comparison that does not involve
   OpenSSL's RSA object at all.  cached == 0 makes BN_mod_exp_mont build its
   own Montgomery context inside the timed region, which is what our
   c68k_huge_number_mont_power_modulus() does on every call. */
int a_ossl_modexp_setup(const unsigned char *n, const unsigned char *x);
int a_ossl_modexp_public(int cached, unsigned char *out);
int a_ossl_modexp_private(int cached, const unsigned char *d,
                          unsigned char *out);
void a_ossl_modexp_free(void);

/* ---------------------------------------------------------------- P-256 -- */

int  a_ossl_ec_setup(const unsigned char *pub65,
                     const unsigned char *ecdh_priv32,
                     const unsigned char *ecdh_pub65);
void a_ossl_ec_free(void);

int  a_ossl_ecdsa_verify(const unsigned char *hash32,
                         const unsigned char *sig, int sig_len);
int  a_ossl_ecdh(const unsigned char *peer65, unsigned char *out32);

/* k * G, the operation an ECDHE key generation is.  consttime selects whether
   the scalar carries BN_FLG_CONSTTIME, which is what decides between OpenSSL's
   Montgomery ladder and its wNAF. */
int  a_ossl_ec_mul_g(const unsigned char *k32, int consttime,
                     unsigned char *out65);

/* Was a generator precomputation table built?  Asked rather than assumed,
   because it is worth several times the scalar multiplication. */
int  a_ossl_ec_have_precompute(void);

/* ----------------------------------------------------------------- bulk -- */

int a_ossl_aes128_cbc(const unsigned char *key16, const unsigned char *iv16,
                      const unsigned char *in, unsigned char *out,
                      unsigned long len);
int a_ossl_hmac_sha256(const unsigned char *key, int key_len,
                       const unsigned char *in, unsigned long len,
                       unsigned char *out32);

#endif /* AMINETXDUO_C68K_AMISSL_H */
