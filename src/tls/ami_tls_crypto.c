/*
 * AmiNetXDuo -- nx_secure crypto tables wired to src/crypto68k/.
 *
 * See ami_tls_crypto.h for why this exists and why the mechanism is a private
 * set of NX_CRYPTO_METHOD entries rather than a patch to third_party/.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nx_secure_tls.h"
#include "nx_secure_tls_api.h"
#include "nx_secure_x509.h"

#include "nx_crypto_rsa.h"
#include "nx_crypto_ec.h"
#include "nx_crypto_huge_number.h"

#include "tx_api.h"

#include "crypto68k.h"
#include "c68k_p256.h"

#include "tls.h"
#include "ami_tls_crypto.h"


/* ------------------------------------------------ measurement and modes --- */

static UINT     ami_arithmetic  = AMI_TLS_ARITH_C68K;
static UINT     ami_crt_enabled = 1u;

VOID ami_tls_crypto_set_arithmetic(UINT mode)
{

    ami_arithmetic =  (mode == AMI_TLS_ARITH_REFERENCE) ?
                      AMI_TLS_ARITH_REFERENCE : AMI_TLS_ARITH_C68K;
}

VOID ami_tls_crypto_set_crt(UINT enable)
{

    ami_crt_enabled =  (enable != 0u) ? 1u : 0u;
}

/* Bank 0 is the registered client thread, bank 1 is everything else. */
#define AMI_BANK_CLIENT     0u
#define AMI_BANK_OTHER      1u

static AMI_TLS_CRYPTO_COUNTERS  ami_counters[2];
static VOID                    *ami_client_thread;

VOID ami_tls_crypto_set_client_thread(VOID *thread)
{

    ami_client_thread =  thread;
}

static UINT ami_bank(VOID)
{

    if ((ami_client_thread != NX_NULL) &&
        ((VOID *)tx_thread_identify() == ami_client_thread))
    {
        return(AMI_BANK_CLIENT);
    }

    return(AMI_BANK_OTHER);
}

VOID ami_tls_crypto_counters_get(AMI_TLS_CRYPTO_COUNTERS *client,
                                 AMI_TLS_CRYPTO_COUNTERS *other)
{

    if (client != NX_NULL)
    {
        *client = ami_counters[AMI_BANK_CLIENT];
    }

    if (other != NX_NULL)
    {
        *other = ami_counters[AMI_BANK_OTHER];
    }
}

VOID ami_tls_crypto_counters_reset(VOID)
{

    NX_CRYPTO_MEMSET(ami_counters, 0, sizeof(ami_counters));
}


/* ------------------------------------------------------------ P-256 curve -- */

/*
 * Our own, mutable, copy of the vendored secp256r1 curve.  The vendored object
 * is `const` and is aliased by every ECDH and ECDSA context in the process, so
 * it is not ours to write to; this copy is handed out by the curve method
 * below and nothing else can reach it.
 *
 * A shallow copy is correct and is what tests/crypto68k/c68k_ec_bench.c has
 * been doing since the EC work landed: every NX_CRYPTO_HUGE_NUMBER inside
 * NX_CRYPTO_EC points at static limb arrays that no operation writes, and the
 * fixed-point table is likewise read-only.  What matters for the fixed-base
 * comb is pointer identity with `&curve -> nx_crypto_ec_g`, and callers derive
 * that from whichever curve they were given -- so it holds for the copy too.
 */
static NX_CRYPTO_EC     ami_p256_curve;
static UINT             ami_p256_ready;

static VOID ami_p256_ec_multiple(NX_CRYPTO_EC *curve,
                                 NX_CRYPTO_EC_POINT *g,
                                 NX_CRYPTO_HUGE_NUMBER *d,
                                 NX_CRYPTO_EC_POINT *r,
                                 HN_UBASE *scratch)
{

AMI_TLS_CRYPTO_COUNTERS    *bank;
ULONG                       start;


    bank  =  &ami_counters[ami_bank()];
    start =  ami_tls_eclock();

    if (ami_arithmetic == AMI_TLS_ARITH_REFERENCE)
    {
        _nx_crypto_ec_fp_projective_multiple(curve, g, d, r, scratch);
    }
    else
    {
        c68k_p256_ec_multiple(curve, g, d, r, scratch);
    }

    bank -> ami_ec_multiple_count++;
    bank -> ami_ec_multiple_us += ami_tls_eclock_micros(ami_tls_eclock() - start);
}

UINT ami_tls_crypto_initialize(VOID)
{

NX_CRYPTO_EC   *reference;
UINT            status;


    if (ami_p256_ready)
    {
        return(NX_SUCCESS);
    }

    status =  _nx_crypto_ec_get_named_curve(&reference, NX_CRYPTO_EC_SECP256R1);
    if (status != NX_CRYPTO_SUCCESS)
    {
        return(NX_NOT_SUCCESSFUL);
    }

    ami_p256_curve = *reference;
    ami_p256_curve.nx_crypto_ec_multiple = ami_p256_ec_multiple;

    /*
     * The comb table in c68k_p256_table.c is generated, so a table built for a
     * different curve would produce points that are self-consistent and wrong.
     * Two generic scalar multiplications settle it; on the floor target that is
     * under three seconds, once, at startup.
     */
#ifdef AMINETXDUO_TLS_CRYPTO68K_SELFCHECK
    if (c68k_p256_self_check() != NX_CRYPTO_SUCCESS)
    {
        ami_p256_curve.nx_crypto_ec_multiple = reference -> nx_crypto_ec_multiple;
        ami_p256_ready = 1u;
        return(NX_NOT_SUCCESSFUL);
    }
#endif

    ami_p256_ready = 1u;

    return(NX_SUCCESS);
}

/*
 * The curve method.  Signature-compatible with
 * _nx_crypto_method_ec_secp256r1_operation(); the only difference is which
 * NX_CRYPTO_EC comes back.
 */
static UINT ami_crypto_method_ec_secp256_operation(UINT op,
                                                   VOID *handle,
                                                   struct NX_CRYPTO_METHOD_STRUCT *method,
                                                   UCHAR *key,
                                                   NX_CRYPTO_KEY_SIZE key_size_in_bits,
                                                   UCHAR *input,
                                                   ULONG input_length_in_byte,
                                                   UCHAR *iv_ptr,
                                                   UCHAR *output,
                                                   ULONG output_length_in_byte,
                                                   VOID *crypto_metadata,
                                                   ULONG crypto_metadata_size,
                                                   VOID *packet_ptr,
                                                   VOID (*callback)(VOID *, UINT))
{

    NX_CRYPTO_PARAMETER_NOT_USED(handle);
    NX_CRYPTO_PARAMETER_NOT_USED(method);
    NX_CRYPTO_PARAMETER_NOT_USED(key);
    NX_CRYPTO_PARAMETER_NOT_USED(key_size_in_bits);
    NX_CRYPTO_PARAMETER_NOT_USED(input);
    NX_CRYPTO_PARAMETER_NOT_USED(input_length_in_byte);
    NX_CRYPTO_PARAMETER_NOT_USED(iv_ptr);
    NX_CRYPTO_PARAMETER_NOT_USED(output_length_in_byte);
    NX_CRYPTO_PARAMETER_NOT_USED(crypto_metadata);
    NX_CRYPTO_PARAMETER_NOT_USED(crypto_metadata_size);
    NX_CRYPTO_PARAMETER_NOT_USED(packet_ptr);
    NX_CRYPTO_PARAMETER_NOT_USED(callback);

    if (op != NX_CRYPTO_EC_CURVE_GET)
    {
        return(NX_CRYPTO_NOT_SUCCESSFUL);
    }

    /*
     * Nobody should reach here without ami_tls_crypto_initialize(), but a curve
     * struct full of zeroes is a null-pointer dereference three calls deep, so
     * build it now rather than trust the caller.  Idempotent.
     */
    if (!ami_p256_ready)
    {
        (VOID) ami_tls_crypto_initialize();

        if (!ami_p256_ready)
        {
            return(NX_CRYPTO_NOT_SUCCESSFUL);
        }
    }

    *((NX_CRYPTO_EC **)output) = &ami_p256_curve;

    return(NX_CRYPTO_SUCCESS);
}

NX_CRYPTO_METHOD ami_crypto_method_ec_secp256 =
{
    NX_CRYPTO_EC_SECP256R1,                     /* EC placeholder             */
    256,                                        /* Key size in bits           */
    0,                                          /* IV size in bits            */
    0,                                          /* ICV size in bits, not used */
    0,                                          /* Block size in bytes        */
    0,                                          /* Metadata size in bytes     */
    NX_CRYPTO_NULL,                             /* Initialization routine     */
    NX_CRYPTO_NULL,                             /* Cleanup routine, not used  */
    ami_crypto_method_ec_secp256_operation      /* Operation                  */
};


/* ------------------------------------------------------- the RSA primes --- */

/*
 * Where a certificate's p and q live between "the application parsed a
 * certificate" and "nx_secure asked for a private-key exponentiation on that
 * modulus without telling us the primes".
 *
 * Four entries: a local certificate chain that needs more than a leaf and a
 * client certificate is not a shape this stack offers yet.  Nothing is copied
 * -- the pointers are into the caller's DER, exactly as
 * nx_secure_process_client_key_exchange.c's own CRT setup does.
 */
#define AMI_TLS_RSA_KEY_SLOTS   4

typedef struct AMI_TLS_RSA_KEY_STRUCT
{
    const UCHAR    *ami_modulus;
    UINT            ami_modulus_length;
    const UCHAR    *ami_prime_p;
    UINT            ami_prime_p_length;
    const UCHAR    *ami_prime_q;
    UINT            ami_prime_q_length;
} AMI_TLS_RSA_KEY;

static AMI_TLS_RSA_KEY  ami_rsa_keys[AMI_TLS_RSA_KEY_SLOTS];
static UINT             ami_rsa_key_count;

UINT ami_tls_rsa_key_register(const NX_SECURE_X509_CERT *certificate)
{

const NX_SECURE_RSA_PRIVATE_KEY    *key;
AMI_TLS_RSA_KEY                    *slot;
UINT                                i;


    if (certificate == NX_NULL)
    {
        return(NX_PTR_ERROR);
    }

    key =  &certificate -> nx_secure_x509_private_key.rsa_private_key;

    if ((key -> nx_secure_rsa_private_prime_p == NX_NULL) ||
        (key -> nx_secure_rsa_private_prime_q == NX_NULL) ||
        (key -> nx_secure_rsa_public_modulus  == NX_NULL))
    {
        return(NX_NOT_SUCCESSFUL);
    }

    /* Re-registering the same certificate must not consume a second slot. */
    for (i = 0; i < ami_rsa_key_count; i++)
    {
        if (ami_rsa_keys[i].ami_modulus == key -> nx_secure_rsa_public_modulus)
        {
            return(NX_SUCCESS);
        }
    }

    if (ami_rsa_key_count >= (UINT)AMI_TLS_RSA_KEY_SLOTS)
    {
        return(NX_NO_MORE_ENTRIES);
    }

    slot =  &ami_rsa_keys[ami_rsa_key_count];

    slot -> ami_modulus         = key -> nx_secure_rsa_public_modulus;
    slot -> ami_modulus_length  = (UINT)key -> nx_secure_rsa_public_modulus_length;
    slot -> ami_prime_p         = key -> nx_secure_rsa_private_prime_p;
    slot -> ami_prime_p_length  = (UINT)key -> nx_secure_rsa_private_prime_p_length;
    slot -> ami_prime_q         = key -> nx_secure_rsa_private_prime_q;
    slot -> ami_prime_q_length  = (UINT)key -> nx_secure_rsa_private_prime_q_length;

    ami_rsa_key_count++;

    return(NX_SUCCESS);
}

VOID ami_tls_rsa_key_reset(VOID)
{

    NX_CRYPTO_MEMSET(ami_rsa_keys, 0, sizeof(ami_rsa_keys));
    ami_rsa_key_count = 0;
}

UINT ami_tls_local_certificate_add(NX_SECURE_TLS_SESSION *tls_session,
                                   NX_SECURE_X509_CERT *certificate)
{

UINT    status;


    status =  nx_secure_tls_local_certificate_add(tls_session, certificate);
    if (status == NX_SUCCESS)
    {
        (VOID) ami_tls_rsa_key_register(certificate);
    }

    return(status);
}

/*
 * Find the primes for this modulus.  The comparison is on the bytes and not on
 * the pointer: nx_secure hands the RSA method the modulus out of
 * nx_secure_x509_public_key for signing and out of nx_secure_x509_private_key
 * for decryption, and those are two different pointers to the same number in
 * the same certificate.
 */
static const AMI_TLS_RSA_KEY *ami_rsa_find_primes(const UCHAR *modulus,
                                                  UINT modulus_length)
{

UINT    i;


    if ((modulus == NX_NULL) || (modulus_length == 0u))
    {
        return(NX_NULL);
    }

    for (i = 0; i < ami_rsa_key_count; i++)
    {
        if ((ami_rsa_keys[i].ami_modulus_length == modulus_length) &&
            (NX_CRYPTO_MEMCMP(ami_rsa_keys[i].ami_modulus, modulus,
                              modulus_length) == 0))
        {
            return(&ami_rsa_keys[i]);
        }
    }

    return(NX_NULL);
}


/* ------------------------------------------------------------------- RSA --- */

/*
 * Scratch for c68k, in limbs, on top of the vendored NX_CRYPTO_RSA context.
 *
 * c68k_mont_power_modulus() picks the largest sliding window that fits what it
 * is given -- C68K_POWM_SCRATCH_LIMBS(m_len, w) = (10 + 2^(w-1)) * m_len + 8 --
 * and 1536 limbs buys:
 *
 *     CRT half of a 2048-bit key (m_len 32)   w = 6   the case that matters
 *     full-width 2048-bit        (m_len 64)   w = 4   ~4% off w = 6
 *     full-width 4096-bit        (m_len 128)  w = 1   plain square-and-multiply
 *
 * and the w = 4 line only runs when a private key's primes are unknown.  6 KB
 * per RSA context, and a TLS session carries two (public cipher and public
 * auth), so this is 12 KB of the per-session metadata -- measured and reported
 * by tests/tls/tls_handshake rather than estimated.
 *
 * Undersizing is safe rather than wrong: c68k_huge_number_mont_power_modulus()
 * falls back to the vendored routine when the window will not fit at all.
 */
#define AMI_TLS_POWM_SCRATCH_LIMBS  1536u

typedef struct AMI_CRYPTO_RSA_STRUCT
{
    NX_CRYPTO_RSA   ami_rsa_base;
    HN_UBASE        ami_rsa_powm_scratch[AMI_TLS_POWM_SCRATCH_LIMBS];
} AMI_CRYPTO_RSA;

/*
 * Anything longer than this is a private exponent.  Real public exponents are
 * 3 (0x010001) or 1 byte; the largest that has ever been seen in the wild is a
 * handful of bytes.  The test only decides whether it is worth consulting the
 * prime table -- getting it wrong costs a table walk, not a wrong answer.
 */
#define AMI_TLS_RSA_PUBLIC_EXPONENT_MAX  16u

/*
 * The vendored _nx_crypto_rsa_operation() with the two exponentiation calls
 * replaced.  The buffer carving is deliberately identical, including the
 * modulus_length << 1 on the output: NX_CRYPTO_HUGE_NUMBER_INITIALIZE is a bump
 * allocator over the same scratch the CRT routine then continues to allocate
 * from, so the layout is load bearing.
 */
static UINT ami_rsa_exponentiate(const UCHAR *exponent, UINT exponent_length,
                                 const UCHAR *modulus, UINT modulus_length,
                                 const UCHAR *p, UINT p_length,
                                 const UCHAR *q, UINT q_length,
                                 const UCHAR *input, UINT input_length,
                                 UCHAR *output,
                                 USHORT *scratch_buf_ptr,
                                 HN_UBASE *powm_scratch, UINT powm_scratch_limbs)
{

HN_UBASE               *scratch;
UINT                    mod_length;
NX_CRYPTO_HUGE_NUMBER   modulus_hn, exponent_hn, input_hn, output_hn, p_hn, q_hn;


    scratch =  (HN_UBASE *)scratch_buf_ptr;

    NX_CRYPTO_HUGE_NUMBER_INITIALIZE(&modulus_hn,  scratch, modulus_length);
    NX_CRYPTO_HUGE_NUMBER_INITIALIZE(&input_hn,    scratch, modulus_length);
    NX_CRYPTO_HUGE_NUMBER_INITIALIZE(&exponent_hn, scratch, modulus_length);
    NX_CRYPTO_HUGE_NUMBER_INITIALIZE(&output_hn,   scratch, modulus_length << 1);

    _nx_crypto_huge_number_setup(&exponent_hn, exponent, exponent_length);
    _nx_crypto_huge_number_setup(&input_hn,    input,    input_length);
    _nx_crypto_huge_number_setup(&modulus_hn,  modulus,  modulus_length);

    if ((p != NX_CRYPTO_NULL) && (q != NX_CRYPTO_NULL))
    {
        NX_CRYPTO_HUGE_NUMBER_INITIALIZE(&p_hn, scratch, modulus_length >> 1);
        NX_CRYPTO_HUGE_NUMBER_INITIALIZE(&q_hn, scratch, modulus_length >> 1);

        _nx_crypto_huge_number_setup(&p_hn, p, p_length);
        _nx_crypto_huge_number_setup(&q_hn, q, q_length);

        if (ami_arithmetic == AMI_TLS_ARITH_REFERENCE)
        {
            _nx_crypto_huge_number_crt_power_modulus(&input_hn, &exponent_hn,
                                                     &p_hn, &q_hn, &modulus_hn,
                                                     &output_hn, scratch);
        }
        else
        {
            c68k_crt_power_modulus(&input_hn, &exponent_hn, &p_hn, &q_hn,
                                   &modulus_hn, &output_hn, scratch,
                                   powm_scratch, powm_scratch_limbs);
        }
    }
    else
    {
        if (ami_arithmetic == AMI_TLS_ARITH_REFERENCE)
        {
            _nx_crypto_huge_number_mont_power_modulus(&input_hn, &exponent_hn,
                                                      &modulus_hn, &output_hn,
                                                      scratch);
        }
        else
        {
            c68k_huge_number_mont_power_modulus(&input_hn, &exponent_hn,
                                                &modulus_hn, &output_hn,
                                                powm_scratch, powm_scratch_limbs);
        }
    }

    _nx_crypto_huge_number_extract(&output_hn, output, modulus_length, &mod_length);

    return(NX_CRYPTO_SUCCESS);
}

static UINT ami_crypto_method_rsa_init(struct NX_CRYPTO_METHOD_STRUCT *method,
                                       UCHAR *key,
                                       NX_CRYPTO_KEY_SIZE key_size_in_bits,
                                       VOID **handle,
                                       VOID *crypto_metadata,
                                       ULONG crypto_metadata_size)
{

    /*
     * The vendored init does exactly the right thing -- record the modulus,
     * null the primes -- and NX_CRYPTO_RSA is the first member of our context,
     * so it can operate on ours unchanged.  It checks the size against
     * sizeof(NX_CRYPTO_RSA), which ours exceeds.
     */
    if (crypto_metadata_size < sizeof(AMI_CRYPTO_RSA))
    {
        return(NX_CRYPTO_PTR_ERROR);
    }

    return(_nx_crypto_method_rsa_init(method, key, key_size_in_bits, handle,
                                      crypto_metadata, crypto_metadata_size));
}

static UINT ami_crypto_method_rsa_cleanup(VOID *crypto_metadata)
{

#ifdef NX_SECURE_KEY_CLEAR
    if (crypto_metadata != NX_CRYPTO_NULL)
    {
        NX_CRYPTO_MEMSET(crypto_metadata, 0, sizeof(AMI_CRYPTO_RSA));
    }
    return(NX_CRYPTO_SUCCESS);
#else
    return(_nx_crypto_method_rsa_cleanup(crypto_metadata));
#endif
}

static UINT ami_crypto_method_rsa_operation(UINT op,
                                            VOID *handle,
                                            struct NX_CRYPTO_METHOD_STRUCT *method,
                                            UCHAR *key,
                                            NX_CRYPTO_KEY_SIZE key_size_in_bits,
                                            UCHAR *input,
                                            ULONG input_length_in_byte,
                                            UCHAR *iv_ptr,
                                            UCHAR *output,
                                            ULONG output_length_in_byte,
                                            VOID *crypto_metadata,
                                            ULONG crypto_metadata_size,
                                            VOID *packet_ptr,
                                            VOID (*callback)(VOID *, UINT))
{

AMI_CRYPTO_RSA             *ctx;
NX_CRYPTO_RSA              *base;
const AMI_TLS_RSA_KEY      *registered;
AMI_TLS_CRYPTO_COUNTERS    *bank;
const UCHAR                *p;
const UCHAR                *q;
UINT                        p_length;
UINT                        q_length;
UINT                        exponent_length;
UINT                        status;
ULONG                       start;
ULONG                       elapsed;


    NX_CRYPTO_PARAMETER_NOT_USED(handle);
    NX_CRYPTO_PARAMETER_NOT_USED(iv_ptr);
    NX_CRYPTO_PARAMETER_NOT_USED(packet_ptr);
    NX_CRYPTO_PARAMETER_NOT_USED(callback);

    NX_CRYPTO_STATE_CHECK

    if ((method == NX_CRYPTO_NULL) || (crypto_metadata == NX_CRYPTO_NULL) ||
        ((((ULONG)crypto_metadata) & 0x3UL) != 0UL))
    {
        return(NX_CRYPTO_PTR_ERROR);
    }

    if (crypto_metadata_size < sizeof(AMI_CRYPTO_RSA))
    {
        return(NX_CRYPTO_PTR_ERROR);
    }

    ctx  =  (AMI_CRYPTO_RSA *)crypto_metadata;
    base =  &ctx -> ami_rsa_base;

    if (op == NX_CRYPTO_SET_PRIME_P)
    {
        base -> nx_crypto_rsa_prime_p        = input;
        base -> nx_crypto_rsa_prime_p_length = input_length_in_byte;

        return(NX_CRYPTO_SUCCESS);
    }

    if (op == NX_CRYPTO_SET_PRIME_Q)
    {
        base -> nx_crypto_rsa_prime_q        = input;
        base -> nx_crypto_rsa_prime_q_length = input_length_in_byte;

        return(NX_CRYPTO_SUCCESS);
    }

    if (key == NX_CRYPTO_NULL)
    {
        return(NX_CRYPTO_PTR_ERROR);
    }

    if (output_length_in_byte < (ULONG)(key_size_in_bits >> 3))
    {
        return(NX_CRYPTO_INVALID_BUFFER_SIZE);
    }

    if (input_length_in_byte > (ULONG)(base -> nx_crypto_rsa_modulus_length))
    {
        return(NX_CRYPTO_PTR_ERROR);
    }

    exponent_length =  (UINT)(key_size_in_bits >> 3);

    p        =  base -> nx_crypto_rsa_prime_p;
    p_length =  base -> nx_crypto_rsa_prime_p_length;
    q        =  base -> nx_crypto_rsa_prime_q;
    q_length =  base -> nx_crypto_rsa_prime_q_length;

    if ((p == NX_CRYPTO_NULL) || (q == NX_CRYPTO_NULL))
    {
        p =  NX_CRYPTO_NULL;
        q =  NX_CRYPTO_NULL;

        /*
         * THE MISSING 3.6x.  nx_secure sets the primes on one path out of
         * three; on the other two -- the ECDHE_RSA ServerKeyExchange signature
         * and CertificateVerify -- it hands over the full private exponent and
         * takes the 158 s branch.  If the application registered the
         * certificate, the primes are recoverable here.
         */
        if (ami_crt_enabled &&
            (exponent_length > AMI_TLS_RSA_PUBLIC_EXPONENT_MAX))
        {
            registered =  ami_rsa_find_primes(base -> nx_crypto_rsa_modulus,
                                              base -> nx_crypto_rsa_modulus_length);
            if (registered != NX_NULL)
            {
                p        =  registered -> ami_prime_p;
                p_length =  registered -> ami_prime_p_length;
                q        =  registered -> ami_prime_q;
                q_length =  registered -> ami_prime_q_length;
            }
        }
    }

    bank  =  &ami_counters[ami_bank()];
    start =  ami_tls_eclock();

    status =  ami_rsa_exponentiate(key, exponent_length,
                                   base -> nx_crypto_rsa_modulus,
                                   base -> nx_crypto_rsa_modulus_length,
                                   p, p_length, q, q_length,
                                   input, (UINT)input_length_in_byte,
                                   output,
                                   base -> nx_crypto_rsa_scratch_buffer,
                                   ctx -> ami_rsa_powm_scratch,
                                   AMI_TLS_POWM_SCRATCH_LIMBS);

    elapsed =  ami_tls_eclock_micros(ami_tls_eclock() - start);

    if (exponent_length <= AMI_TLS_RSA_PUBLIC_EXPONENT_MAX)
    {
        bank -> ami_rsa_public_count++;
        bank -> ami_rsa_public_us += elapsed;
    }
    else if (p != NX_CRYPTO_NULL)
    {
        bank -> ami_rsa_private_crt_count++;
        bank -> ami_rsa_private_crt_us += elapsed;
    }
    else
    {
        bank -> ami_rsa_private_plain_count++;
        bank -> ami_rsa_private_plain_us += elapsed;
    }

    return(status);
}

NX_CRYPTO_METHOD ami_crypto_method_rsa =
{
    NX_CRYPTO_KEY_EXCHANGE_RSA,             /* RSA crypto algorithm           */
    0,                                      /* Key size in bits               */
    0,                                      /* IV size in bits                */
    0,                                      /* ICV size in bits, not used     */
    0,                                      /* Block size in bytes            */
    sizeof(AMI_CRYPTO_RSA),                 /* Metadata size in bytes         */
    ami_crypto_method_rsa_init,             /* RSA initialization routine     */
    ami_crypto_method_rsa_cleanup,          /* RSA cleanup routine            */
    ami_crypto_method_rsa_operation         /* RSA operation                  */
};


/* ------------------------------------------------------------- the tables -- */

/* Everything we do not touch comes straight from nx_crypto_methods.c. */
extern NX_CRYPTO_METHOD crypto_method_none;
extern NX_CRYPTO_METHOD crypto_method_null;
extern NX_CRYPTO_METHOD crypto_method_aes_cbc_128;
extern NX_CRYPTO_METHOD crypto_method_aes_cbc_256;
extern NX_CRYPTO_METHOD crypto_method_aes_ccm_8;
extern NX_CRYPTO_METHOD crypto_method_aes_ccm_16;
extern NX_CRYPTO_METHOD crypto_method_aes_128_gcm_16;
extern NX_CRYPTO_METHOD crypto_method_ecdsa;
extern NX_CRYPTO_METHOD crypto_method_ecdhe;
extern NX_CRYPTO_METHOD crypto_method_hmac_sha1;
extern NX_CRYPTO_METHOD crypto_method_hmac_sha256;
extern NX_CRYPTO_METHOD crypto_method_auth_psk;
extern NX_CRYPTO_METHOD crypto_method_ec_secp384;
extern NX_CRYPTO_METHOD crypto_method_ec_secp521;
extern NX_CRYPTO_METHOD crypto_method_md5;
extern NX_CRYPTO_METHOD crypto_method_sha1;
extern NX_CRYPTO_METHOD crypto_method_sha224;
extern NX_CRYPTO_METHOD crypto_method_sha256;
extern NX_CRYPTO_METHOD crypto_method_sha384;
extern NX_CRYPTO_METHOD crypto_method_sha512;
extern NX_CRYPTO_METHOD crypto_method_tls_prf_1;
extern NX_CRYPTO_METHOD crypto_method_tls_prf_sha256;
extern NX_CRYPTO_METHOD crypto_method_hkdf;
extern NX_CRYPTO_METHOD crypto_method_hmac;

/*
 * The X.509 signature table.  Row for row the vendored
 * _nx_crypto_x509_cipher_lookup_table_ecc with crypto_method_rsa swapped; the
 * ECDSA rows need no change because crypto_method_ecdsa reaches P-256 through
 * the curve method, which is ours.
 */
static NX_SECURE_X509_CRYPTO ami_x509_cipher_table[] =
{
    /* OID identifier,                       public cipher,             hash method */
    {NX_SECURE_TLS_X509_TYPE_ECDSA_SHA_256,  &crypto_method_ecdsa,      &crypto_method_sha256},
    {NX_SECURE_TLS_X509_TYPE_ECDSA_SHA_384,  &crypto_method_ecdsa,      &crypto_method_sha384},
    {NX_SECURE_TLS_X509_TYPE_ECDSA_SHA_512,  &crypto_method_ecdsa,      &crypto_method_sha512},
    {NX_SECURE_TLS_X509_TYPE_RSA_SHA_256,    &ami_crypto_method_rsa,    &crypto_method_sha256},
    {NX_SECURE_TLS_X509_TYPE_RSA_SHA_384,    &ami_crypto_method_rsa,    &crypto_method_sha384},
    {NX_SECURE_TLS_X509_TYPE_RSA_SHA_512,    &ami_crypto_method_rsa,    &crypto_method_sha512},
    {NX_SECURE_TLS_X509_TYPE_ECDSA_SHA_224,  &crypto_method_ecdsa,      &crypto_method_sha224},
    {NX_SECURE_TLS_X509_TYPE_ECDSA_SHA_1,    &crypto_method_ecdsa,      &crypto_method_sha1},
    {NX_SECURE_TLS_X509_TYPE_RSA_SHA_1,      &ami_crypto_method_rsa,    &crypto_method_sha1},
    {NX_SECURE_TLS_X509_TYPE_RSA_MD5,        &ami_crypto_method_rsa,    &crypto_method_md5},
};

/*
 * The ciphersuite table.  Same suites, same ORDER as the vendored ECC table --
 * nx_secure negotiates top-down, so reordering would silently change which
 * suite a handshake picks and make every before/after comparison meaningless.
 */
static NX_SECURE_TLS_CIPHERSUITE_INFO ami_ciphersuite_table[] =
{
    /* Ciphersuite,                           public cipher,            public_auth,              session cipher & mode,          iv, key, hash method,                hash size, TLS PRF */
#if (NX_SECURE_TLS_TLS_1_3_ENABLED)
    {TLS_AES_128_GCM_SHA256,                  &crypto_method_ecdhe,     &crypto_method_ecdsa,     &crypto_method_aes_128_gcm_16,  96, 16,  &crypto_method_sha256,      32,        &crypto_method_hkdf},
    {TLS_AES_128_CCM_SHA256,                  &crypto_method_ecdhe,     &crypto_method_ecdsa,     &crypto_method_aes_ccm_16,      96, 16,  &crypto_method_sha256,      32,        &crypto_method_hkdf},
    {TLS_AES_128_CCM_8_SHA256,                &crypto_method_ecdhe,     &crypto_method_ecdsa,     &crypto_method_aes_ccm_8,       96, 16,  &crypto_method_sha256,      32,        &crypto_method_hkdf},
#endif

#ifdef NX_SECURE_ENABLE_AEAD_CIPHER
    {TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256, &crypto_method_ecdhe,     &crypto_method_ecdsa,     &crypto_method_aes_128_gcm_16,  16, 16,  &crypto_method_null,         0,        &crypto_method_tls_prf_sha256},
    {TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,   &crypto_method_ecdhe,     &ami_crypto_method_rsa,   &crypto_method_aes_128_gcm_16,  16, 16,  &crypto_method_null,         0,        &crypto_method_tls_prf_sha256},
#endif

    {TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256, &crypto_method_ecdhe,     &crypto_method_ecdsa,     &crypto_method_aes_cbc_128,     16, 16,  &crypto_method_hmac_sha256, 32,        &crypto_method_tls_prf_sha256},
    {TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256,   &crypto_method_ecdhe,     &ami_crypto_method_rsa,   &crypto_method_aes_cbc_128,     16, 16,  &crypto_method_hmac_sha256, 32,        &crypto_method_tls_prf_sha256},

#ifdef NX_SECURE_ENABLE_AEAD_CIPHER
    {TLS_RSA_WITH_AES_128_GCM_SHA256,         &ami_crypto_method_rsa,   &ami_crypto_method_rsa,   &crypto_method_aes_128_gcm_16,  16, 16,  &crypto_method_null,         0,        &crypto_method_tls_prf_sha256},
#endif

    {TLS_RSA_WITH_AES_256_CBC_SHA256,         &ami_crypto_method_rsa,   &ami_crypto_method_rsa,   &crypto_method_aes_cbc_256,     16, 32,  &crypto_method_hmac_sha256, 32,        &crypto_method_tls_prf_sha256},
    {TLS_RSA_WITH_AES_128_CBC_SHA256,         &ami_crypto_method_rsa,   &ami_crypto_method_rsa,   &crypto_method_aes_cbc_128,     16, 16,  &crypto_method_hmac_sha256, 32,        &crypto_method_tls_prf_sha256},
};

const NX_SECURE_TLS_CRYPTO ami_crypto_tls_ciphers_ecc =
{
    ami_ciphersuite_table,
    sizeof(ami_ciphersuite_table) / sizeof(NX_SECURE_TLS_CIPHERSUITE_INFO),

#ifndef NX_SECURE_DISABLE_X509
    ami_x509_cipher_table,
    sizeof(ami_x509_cipher_table) / sizeof(NX_SECURE_X509_CRYPTO),
#endif

#if (NX_SECURE_TLS_TLS_1_0_ENABLED || NX_SECURE_TLS_TLS_1_1_ENABLED)
    &crypto_method_md5,
    &crypto_method_sha1,
    &crypto_method_tls_prf_1,
#endif

#if (NX_SECURE_TLS_TLS_1_2_ENABLED)
    &crypto_method_sha256,
    &crypto_method_tls_prf_sha256,
#endif

#if (NX_SECURE_TLS_TLS_1_3_ENABLED)
    &crypto_method_hkdf,
    &crypto_method_hmac,
    &crypto_method_ecdhe,
#endif
};

/*
 * The curve list.  P-256 first, and it is the only entry crypto68k accelerates;
 * P-384 and P-521 stay on the vendored arithmetic, where they are slow enough
 * that no handshake should choose them but present enough that a server which
 * insists on one still works.
 */
const USHORT ami_crypto_ecc_supported_groups[] =
{
    (USHORT)NX_CRYPTO_EC_SECP256R1,
    (USHORT)NX_CRYPTO_EC_SECP384R1,
    (USHORT)NX_CRYPTO_EC_SECP521R1,
};

const NX_CRYPTO_METHOD *ami_crypto_ecc_curves[] =
{
    &ami_crypto_method_ec_secp256,
    &crypto_method_ec_secp384,
    &crypto_method_ec_secp521,
};

const UINT ami_crypto_ecc_supported_groups_size =
    sizeof(ami_crypto_ecc_supported_groups) / sizeof(USHORT);
