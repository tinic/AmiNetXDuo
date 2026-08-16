/*
 * AmiNetXDuo, nx_secure crypto tables wired to src/crypto68k/.
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
#include "nx_crypto_aes.h"
#include "nx_crypto_sha2.h"
#include "nx_crypto_hmac.h"
#include "nx_crypto_hmac_sha2.h"

#include "tx_api.h"

#include "crypto68k.h"
#include "c68k_p256.h"
#include "c68k_aes.h"
#include "c68k_sha256.h"
#include "c68k_chacha20.h"

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

/* Zero unless the application opened the E-Clock, see ami_tls_timer_is_open(). */
static ULONG ami_now(VOID)
{

    return(ami_tls_timer_is_open() ? ami_tls_eclock() : 0UL);
}

static ULONG ami_since(ULONG start)
{

    return(ami_tls_timer_is_open() ?
           ami_tls_eclock_micros(ami_tls_eclock() - start) : 0UL);
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


/* ------------------------------------------------------------ P-256 curve, */

/*
 * A private, mutable copy of the vendored secp256r1 curve.  The vendored
 * object is `const` and is aliased by every ECDH and ECDSA context in the
 * process, so it must not be written to.  The curve method below hands out
 * this copy, and nothing else can reach it.
 *
 * A shallow copy is correct, and is what tests/crypto68k/c68k_ec_bench.c does
 * since the EC work landed.  Every NX_CRYPTO_HUGE_NUMBER inside NX_CRYPTO_EC
 * points at static limb arrays that no operation writes, and the fixed-point
 * table is read-only too.  The fixed-base comb needs pointer identity with
 * `&curve -> nx_crypto_ec_g`, and callers derive that from the curve they
 * were given, so it holds for the copy too.
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
    start =  ami_now();

    if (ami_arithmetic == AMI_TLS_ARITH_REFERENCE)
    {
        _nx_crypto_ec_fp_projective_multiple(curve, g, d, r, scratch);
    }
    else
    {
        c68k_p256_ec_multiple(curve, g, d, r, scratch);
    }

    bank -> ami_ec_multiple_count++;
    bank -> ami_ec_multiple_us += ami_since(start);
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
     * different curve produces points that are self-consistent and wrong.  Two
     * generic scalar multiplications settle it.  On the floor target that is
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
 * _nx_crypto_method_ec_secp256r1_operation().  The only difference is which
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
     * A caller is not expected to reach here without
     * ami_tls_crypto_initialize().  A curve struct full of zeroes is a
     * null-pointer dereference three calls deep, so build it now rather than
     * trust the caller.  Idempotent.
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

static NX_CRYPTO_METHOD ami_crypto_method_ec_secp256 =
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
 * Where a certificate's p and q live, between the moment the application
 * parses a certificate and the moment nx_secure asks for a private-key
 * exponentiation on that modulus without the primes.
 *
 * Four entries: this stack does not yet offer a local certificate chain that
 * needs more than a leaf and a client certificate.  Nothing is copied, the
 * pointers are into the caller's DER, as the CRT setup in
 * nx_secure_process_client_key_exchange.c does.
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
 * Find the primes for this modulus.  The comparison is on the bytes, not on
 * the pointer.  nx_secure hands the RSA method the modulus out of
 * nx_secure_x509_public_key for a signature and out of
 * nx_secure_x509_private_key for a decryption.  Those are two different
 * pointers to the same number in the same certificate.
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
 * is given, C68K_POWM_SCRATCH_LIMBS(m_len, w) = (20 + 2^(w-1)) * m_len + 16,
 * and 2048 limbs buys:
 *
 *     CRT half of a 2048-bit key (m_len 32)   w = 6   the case that matters
 *     full-width 2048-bit        (m_len 64)   w = 4   ~4% off w = 6
 *     full-width 4096-bit        (m_len 128)  does not fit at all
 *
 * The w = 4 line only runs when a private key's primes are unknown.  w = 6 is
 * also C68K_POWM_MAX_WINDOW, so the first line is not short of scratch.
 *
 * 8 KB per RSA context, HN_UBASE being a ULONG here, and a session pays for
 * exactly one of them: nx_secure_tls_metadata_size_calculate() came out
 * 6,144 bytes over the vendored table when the area was 1536 limbs, which is
 * one area and not two, so 2048 limbs puts the ECC table at 18,320 (see
 * tls_conn.c).
 *
 * An undersized area is safe, which is what the 4096-bit line means:
 * c68k_mont_power_modulus() answers NX_CRYPTO_SIZE_ERROR when not even w = 1
 * fits and c68k_huge_number_mont_power_modulus() then falls back to the
 * vendored routine.  A 4096-bit key still verifies, at the vendored speed.
 */
#define AMI_TLS_POWM_SCRATCH_LIMBS  2048u

typedef struct AMI_CRYPTO_RSA_STRUCT
{
    NX_CRYPTO_RSA   ami_rsa_base;
    HN_UBASE        ami_rsa_powm_scratch[AMI_TLS_POWM_SCRATCH_LIMBS];
} AMI_CRYPTO_RSA;

/*
 * Anything longer than this is a private exponent.  Real public exponents are
 * 3 bytes (0x010001) or 1 byte.  The largest one ever seen is a few bytes.
 * The test only decides whether to consult the prime table.  A wrong answer
 * here costs a table walk, not a wrong result.
 */
#define AMI_TLS_RSA_PUBLIC_EXPONENT_MAX  16u

/*
 * The vendored _nx_crypto_rsa_operation() with the two exponentiation calls
 * replaced.  The buffer carving is identical, including the
 * modulus_length << 1 on the output.  NX_CRYPTO_HUGE_NUMBER_INITIALIZE is a
 * bump allocator over the same scratch the CRT routine then allocates from,
 * so the layout is load bearing.
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
     * The vendored init records the modulus and nulls the primes, and
     * NX_CRYPTO_RSA is the first member of this context, so it operates on it
     * unchanged.  It checks the size against sizeof(NX_CRYPTO_RSA), which this
     * context exceeds.
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
         * The missing 3.6x.  nx_secure sets the primes on one path out of
         * three.  On the other two, the ECDHE_RSA ServerKeyExchange signature
         * and CertificateVerify, it hands over the full private exponent and
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
    start =  ami_now();

    status =  ami_rsa_exponentiate(key, exponent_length,
                                   base -> nx_crypto_rsa_modulus,
                                   base -> nx_crypto_rsa_modulus_length,
                                   p, p_length, q, q_length,
                                   input, (UINT)input_length_in_byte,
                                   output,
                                   base -> nx_crypto_rsa_scratch_buffer,
                                   ctx -> ami_rsa_powm_scratch,
                                   AMI_TLS_POWM_SCRATCH_LIMBS);

    elapsed =  ami_since(start);

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

static NX_CRYPTO_METHOD ami_crypto_method_rsa =
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


/* ======================================================= the record path == */
/*
 * AES-128/256-CBC and HMAC-SHA256, on src/crypto68k/.
 *
 *   Same mechanism as the RSA and P-256 methods above.  NX_CRYPTO_METHOD is
 *   nx_secure's own extension point and is what the ciphersuite table is made
 *   of, so swapping an entry redirects every path that reaches the primitive
 *   with no vendored source touched.
 *
 *   docs/RESEARCH.md 15 measured the handshake level with OpenSSL's and the
 *   bulk path equal.  The handshake is paid once per connection.  The record
 *   path is paid on every byte.  `https` moved 16,464 B/s against `http`'s
 *   114,598 on this machine, and most of that ceiling is these two functions.
 *
 *   These two are the record path for 0xC027 and 0xC023, ECDHE_RSA and
 *   ECDHE_ECDSA with AES_128_CBC_SHA256, which a server negotiates when it
 *   will not take the AEAD offered above them.  GitHub is one such server.
 *   ChaCha20-Poly1305 is preferred and is the other record path.  See the
 *   block above ami_crypto_method_chacha20_poly1305.
 */

typedef struct
{
    C68K_AES    ami_aes;
    UCHAR       ami_aes_chain[16];      /* the CBC chaining value           */
} AMI_CRYPTO_AES_CTX;

static UINT ami_crypto_method_aes_init(struct NX_CRYPTO_METHOD_STRUCT *method,
                                       UCHAR *key,
                                       NX_CRYPTO_KEY_SIZE key_size_in_bits,
                                       VOID **handle,
                                       VOID *crypto_metadata,
                                       ULONG crypto_metadata_size)
{

AMI_CRYPTO_AES_CTX *ctx;


    NX_CRYPTO_PARAMETER_NOT_USED(handle);

    if ((method == NX_CRYPTO_NULL) || (key == NX_CRYPTO_NULL) ||
        (crypto_metadata == NX_CRYPTO_NULL))
    {
        return(NX_CRYPTO_PTR_ERROR);
    }

    if (((((ULONG)crypto_metadata) & 0x3u) != 0u) ||
        (crypto_metadata_size < sizeof(AMI_CRYPTO_AES_CTX)))
    {
        return(NX_CRYPTO_PTR_ERROR);
    }

    ctx = (AMI_CRYPTO_AES_CTX *)crypto_metadata;

    if (c68k_aes_key_set(&ctx -> ami_aes, key, (UINT)key_size_in_bits) !=
        NX_CRYPTO_SUCCESS)
    {
        return(NX_CRYPTO_UNSUPPORTED_KEY_SIZE);
    }

    return(NX_CRYPTO_SUCCESS);
}

static UINT ami_crypto_method_aes_cleanup(VOID *crypto_metadata)
{

#ifdef NX_SECURE_KEY_CLEAR
    if (crypto_metadata != NX_CRYPTO_NULL)
    {
        NX_CRYPTO_MEMSET(crypto_metadata, 0, sizeof(AMI_CRYPTO_AES_CTX));
    }
#else
    NX_CRYPTO_PARAMETER_NOT_USED(crypto_metadata);
#endif

    return(NX_CRYPTO_SUCCESS);
}

static VOID ami_aes_set_iv(AMI_CRYPTO_AES_CTX *ctx, const UCHAR *iv)
{

UINT    i;


    for (i = 0; i < 16u; i++)
    {
        ctx -> ami_aes_chain[i] = iv[i];
    }
}

/*
 * The vendored operation, case for case.  A record is either one
 * ENCRYPT/DECRYPT call, or an INITIALIZE followed by UPDATEs and a CALCULATE
 * that has nothing to do.  nx_secure uses both forms, so both are here.
 *
 * A length that is not a whole number of blocks is refused rather than
 * silently truncated.  CBC has no answer for a partial block, and the caller
 * that asked has a bug.  The vendored code returns success and leaves the
 * tail unwritten.
 */
static UINT ami_crypto_method_aes_cbc_operation(UINT op,
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
                                                VOID (*nx_crypto_hw_process_callback)(VOID *packet_ptr, UINT status))
{

AMI_CRYPTO_AES_CTX *ctx;
ULONG               blocks;


    NX_CRYPTO_PARAMETER_NOT_USED(handle);
    NX_CRYPTO_PARAMETER_NOT_USED(key);
    NX_CRYPTO_PARAMETER_NOT_USED(key_size_in_bits);
    NX_CRYPTO_PARAMETER_NOT_USED(output_length_in_byte);
    NX_CRYPTO_PARAMETER_NOT_USED(packet_ptr);
    NX_CRYPTO_PARAMETER_NOT_USED(nx_crypto_hw_process_callback);

    if ((method == NX_CRYPTO_NULL) || (crypto_metadata == NX_CRYPTO_NULL) ||
        ((((ULONG)crypto_metadata) & 0x3u) != 0u))
    {
        return(NX_CRYPTO_PTR_ERROR);
    }

    if (crypto_metadata_size < sizeof(AMI_CRYPTO_AES_CTX))
    {
        return(NX_CRYPTO_PTR_ERROR);
    }

    ctx    = (AMI_CRYPTO_AES_CTX *)crypto_metadata;
    blocks = input_length_in_byte >> 4;

    switch (op)
    {
    case NX_CRYPTO_ENCRYPT_INITIALIZE:
    /* fallthrough */
    case NX_CRYPTO_DECRYPT_INITIALIZE:
        if (iv_ptr == NX_CRYPTO_NULL)
        {
            return(NX_CRYPTO_PTR_ERROR);
        }
        ami_aes_set_iv(ctx, iv_ptr);
        break;

    case NX_CRYPTO_ENCRYPT:
        if (iv_ptr == NX_CRYPTO_NULL)
        {
            return(NX_CRYPTO_PTR_ERROR);
        }
        ami_aes_set_iv(ctx, iv_ptr);
        /* fallthrough */
    case NX_CRYPTO_ENCRYPT_UPDATE:
        if ((input_length_in_byte & 15uL) != 0uL)
        {
            return(NX_CRYPTO_INVALID_BUFFER_SIZE);
        }
        c68k_aes_cbc_encrypt(&ctx -> ami_aes, ctx -> ami_aes_chain,
                             input, output, blocks);
        break;

    case NX_CRYPTO_DECRYPT:
        if (iv_ptr == NX_CRYPTO_NULL)
        {
            return(NX_CRYPTO_PTR_ERROR);
        }
        ami_aes_set_iv(ctx, iv_ptr);
        /* fallthrough */
    case NX_CRYPTO_DECRYPT_UPDATE:
        if ((input_length_in_byte & 15uL) != 0uL)
        {
            return(NX_CRYPTO_INVALID_BUFFER_SIZE);
        }
        c68k_aes_cbc_decrypt(&ctx -> ami_aes, ctx -> ami_aes_chain,
                             input, output, blocks);
        break;

    case NX_CRYPTO_ENCRYPT_CALCULATE:
    /* fallthrough */
    case NX_CRYPTO_DECRYPT_CALCULATE:
        break;

    default:
        return(NX_CRYPTO_INVALID_ALGORITHM);
    }

    return(NX_CRYPTO_SUCCESS);
}

static NX_CRYPTO_METHOD ami_crypto_method_aes_cbc_128 =
{
    NX_CRYPTO_ENCRYPTION_AES_CBC,
    NX_CRYPTO_AES_128_KEY_LEN_IN_BITS,
    NX_CRYPTO_AES_IV_LEN_IN_BITS,
    0,
    (NX_CRYPTO_AES_BLOCK_SIZE_IN_BITS >> 3),
    sizeof(AMI_CRYPTO_AES_CTX),
    ami_crypto_method_aes_init,
    ami_crypto_method_aes_cleanup,
    ami_crypto_method_aes_cbc_operation
};

static NX_CRYPTO_METHOD ami_crypto_method_aes_cbc_256 =
{
    NX_CRYPTO_ENCRYPTION_AES_CBC,
    NX_CRYPTO_AES_256_KEY_LEN_IN_BITS,
    NX_CRYPTO_AES_IV_LEN_IN_BITS,
    0,
    (NX_CRYPTO_AES_BLOCK_SIZE_IN_BITS >> 3),
    sizeof(AMI_CRYPTO_AES_CTX),
    ami_crypto_method_aes_init,
    ami_crypto_method_aes_cleanup,
    ami_crypto_method_aes_cbc_operation
};


/* --------------------------------------------- ChaCha20-Poly1305 (AEAD), */

/*
 * RFC 7905, ciphersuites 0xCCA8 and 0xCCA9.
 *
 *   Present because the two CBC suites above no longer reach the web.
 *   Google's front end answers a ClientHello that offers only 0xC027 and
 *   0xC023 with a handshake_failure in a second and a half.  GitHub still
 *   accepts them, so they stay.
 *
 *   AES-GCM also restores the reach, and is not affordable.  Its GHASH is a
 *   carry-less multiply in GF(2^128), which no 68k instruction does, so
 *   nx_crypto_gcm.c does it bit by bit and charges 344.6 ms for 1 KB against
 *   AES-CBC's 21.9 (docs/RESEARCH.md 5.5).  ChaCha20 is add, rotate and
 *   exclusive-or on 32-bit words, and Poly1305 is MULU.L.  Both come out
 *   ahead of the CBC suite, and docs/RESEARCH.md 54 has the measurement.  The
 *   0xCCA8 rows sit at the top of the table for that reason.
 *
 *   An NX_CRYPTO_METHOD is handed a nonce and a buffer.  What a record looks
 *   like on the wire is decided before it is called.  RFC 7905 differs from
 *   the GCM suites there, no nonce_explicit and a twelve-byte implicit IV,
 *   and that lives in src/tls/rfc7905/.
 */

typedef struct
{
    C68K_CHACHA20_POLY1305  ami_aead;
    UCHAR                   ami_aead_key[C68K_CHACHA20_KEY_SIZE];
} AMI_CRYPTO_CHACHA20_CTX;

static UINT ami_crypto_method_chacha20_poly1305_init(struct NX_CRYPTO_METHOD_STRUCT *method,
                                                     UCHAR *key,
                                                     NX_CRYPTO_KEY_SIZE key_size_in_bits,
                                                     VOID **handle,
                                                     VOID *crypto_metadata,
                                                     ULONG crypto_metadata_size)
{

AMI_CRYPTO_CHACHA20_CTX *ctx;
UINT                     i;


    NX_CRYPTO_PARAMETER_NOT_USED(handle);

    if ((method == NX_CRYPTO_NULL) || (key == NX_CRYPTO_NULL) ||
        (crypto_metadata == NX_CRYPTO_NULL))
    {
        return(NX_CRYPTO_PTR_ERROR);
    }

    if (((((ULONG)crypto_metadata) & 0x3u) != 0u) ||
        (crypto_metadata_size < sizeof(AMI_CRYPTO_CHACHA20_CTX)))
    {
        return(NX_CRYPTO_PTR_ERROR);
    }

    if (key_size_in_bits != (NX_CRYPTO_KEY_SIZE)(C68K_CHACHA20_KEY_SIZE << 3))
    {
        return(NX_CRYPTO_UNSUPPORTED_KEY_SIZE);
    }

    /*
     * The key is kept rather than expanded, because ChaCha20 has no key
     * schedule.  The state is rebuilt from key and nonce once per record, and
     * the nonce only arrives with the record.
     */
    ctx = (AMI_CRYPTO_CHACHA20_CTX *)crypto_metadata;

    for (i = 0u; i < C68K_CHACHA20_KEY_SIZE; i++)
    {
        ctx -> ami_aead_key[i] = key[i];
    }

    return(NX_CRYPTO_SUCCESS);
}

static UINT ami_crypto_method_chacha20_poly1305_cleanup(VOID *crypto_metadata)
{

#ifdef NX_SECURE_KEY_CLEAR
    if (crypto_metadata != NX_CRYPTO_NULL)
    {
        NX_CRYPTO_MEMSET(crypto_metadata, 0, sizeof(AMI_CRYPTO_CHACHA20_CTX));
    }
#else
    NX_CRYPTO_PARAMETER_NOT_USED(crypto_metadata);
#endif

    return(NX_CRYPTO_SUCCESS);
}

/*
 * The record, call for call.  INITIALIZE carries the nonce in `iv_ptr`,
 * length in byte 0, twelve bytes after it, nx_secure's own convention, and
 * the additional data in `input`.  UPDATE moves payload, and can run several
 * times for a chained packet.  CALCULATE produces the tag on send and checks
 * it on receive.
 *
 * A tag mismatch is NX_CRYPTO_AUTHENTICATION_FAILED and nothing else.  The
 * caller maps it to an alert, and the plaintext it has already written into
 * the packet is discarded with it.
 */
static UINT ami_crypto_method_chacha20_poly1305_operation(UINT op,
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
                                                          VOID (*nx_crypto_hw_process_callback)(VOID *packet_ptr, UINT status))
{

AMI_CRYPTO_CHACHA20_CTX *ctx;
UCHAR                    tag[C68K_POLY1305_TAG_SIZE];


    NX_CRYPTO_PARAMETER_NOT_USED(handle);
    NX_CRYPTO_PARAMETER_NOT_USED(key);
    NX_CRYPTO_PARAMETER_NOT_USED(key_size_in_bits);
    NX_CRYPTO_PARAMETER_NOT_USED(packet_ptr);
    NX_CRYPTO_PARAMETER_NOT_USED(nx_crypto_hw_process_callback);

    if ((method == NX_CRYPTO_NULL) || (crypto_metadata == NX_CRYPTO_NULL) ||
        ((((ULONG)crypto_metadata) & 0x3u) != 0u) ||
        (crypto_metadata_size < sizeof(AMI_CRYPTO_CHACHA20_CTX)))
    {
        return(NX_CRYPTO_PTR_ERROR);
    }

    ctx = (AMI_CRYPTO_CHACHA20_CTX *)crypto_metadata;

    switch (op)
    {
    case NX_CRYPTO_ENCRYPT_INITIALIZE:
    /* fallthrough */
    case NX_CRYPTO_DECRYPT_INITIALIZE:
        if ((iv_ptr == NX_CRYPTO_NULL) ||
            (iv_ptr[0] != (UCHAR)C68K_CHACHA20_NONCE_SIZE))
        {
            return(NX_CRYPTO_PTR_ERROR);
        }

        c68k_chacha20_poly1305_initialize(&ctx -> ami_aead,
                                          ctx -> ami_aead_key, &iv_ptr[1]);

        if ((input != NX_CRYPTO_NULL) && (input_length_in_byte != 0uL))
        {
            c68k_chacha20_poly1305_associate(&ctx -> ami_aead, input,
                                             input_length_in_byte);
        }
        break;

    case NX_CRYPTO_ENCRYPT_UPDATE:
        if ((input == NX_CRYPTO_NULL) || (output == NX_CRYPTO_NULL))
        {
            return(NX_CRYPTO_PTR_ERROR);
        }
        c68k_chacha20_poly1305_encrypt(&ctx -> ami_aead, input, output,
                                       input_length_in_byte);
        break;

    case NX_CRYPTO_DECRYPT_UPDATE:
        if ((input == NX_CRYPTO_NULL) || (output == NX_CRYPTO_NULL))
        {
            return(NX_CRYPTO_PTR_ERROR);
        }
        c68k_chacha20_poly1305_decrypt(&ctx -> ami_aead, input, output,
                                       input_length_in_byte);
        break;

    case NX_CRYPTO_ENCRYPT_CALCULATE:
        if ((output == NX_CRYPTO_NULL) ||
            (output_length_in_byte < (ULONG)C68K_POLY1305_TAG_SIZE))
        {
            return(NX_CRYPTO_INVALID_BUFFER_SIZE);
        }
        c68k_chacha20_poly1305_tag(&ctx -> ami_aead, output);
        break;

    case NX_CRYPTO_DECRYPT_CALCULATE:
        if ((input == NX_CRYPTO_NULL) ||
            (input_length_in_byte != (ULONG)C68K_POLY1305_TAG_SIZE))
        {
            return(NX_CRYPTO_INVALID_BUFFER_SIZE);
        }
        c68k_chacha20_poly1305_tag(&ctx -> ami_aead, tag);
        if (c68k_chacha20_poly1305_verify(tag, input) != NX_CRYPTO_TRUE)
        {
            return(NX_CRYPTO_AUTHENTICATION_FAILED);
        }
        break;

    default:
        /*
         * The one-shot ENCRYPT and DECRYPT forms are not implemented.  An
         * AEAD record is always INITIALIZE / UPDATE / CALCULATE, because the
         * tag arrives separately from the payload.  nx_secure never issues
         * the one-shot form for one.
         */
        return(NX_CRYPTO_INVALID_ALGORITHM);
    }

    return(NX_CRYPTO_SUCCESS);
}

static NX_CRYPTO_METHOD ami_crypto_method_chacha20_poly1305 =
{
    NX_CRYPTO_ENCRYPTION_CHACHA20_POLY1305,
    (C68K_CHACHA20_KEY_SIZE << 3),          /* 256-bit key                    */
    (C68K_CHACHA20_NONCE_SIZE << 3),        /* the whole 12-byte IV is        */
                                            /* implicit, RFC 7905             */
    (C68K_POLY1305_TAG_SIZE << 3),          /* 128-bit tag                    */
    1,                                      /* block size: a stream cipher,   */
                                            /* so a record can be split       */
                                            /* anywhere                       */
    sizeof(AMI_CRYPTO_CHACHA20_CTX),
    ami_crypto_method_chacha20_poly1305_init,
    ami_crypto_method_chacha20_poly1305_cleanup,
    ami_crypto_method_chacha20_poly1305_operation
};


/* -------------------------------------------------------------- SHA-256, */

static UINT ami_crypto_method_sha256_init(struct NX_CRYPTO_METHOD_STRUCT *method,
                                          UCHAR *key,
                                          NX_CRYPTO_KEY_SIZE key_size_in_bits,
                                          VOID **handle,
                                          VOID *crypto_metadata,
                                          ULONG crypto_metadata_size)
{

    NX_CRYPTO_PARAMETER_NOT_USED(key);
    NX_CRYPTO_PARAMETER_NOT_USED(key_size_in_bits);
    NX_CRYPTO_PARAMETER_NOT_USED(handle);

    if ((method == NX_CRYPTO_NULL) || (crypto_metadata == NX_CRYPTO_NULL) ||
        ((((ULONG)crypto_metadata) & 0x3u) != 0u) ||
        (crypto_metadata_size < sizeof(C68K_SHA256)))
    {
        return(NX_CRYPTO_PTR_ERROR);
    }

    return(NX_CRYPTO_SUCCESS);
}

static UINT ami_crypto_method_sha256_cleanup(VOID *crypto_metadata)
{

#ifdef NX_SECURE_KEY_CLEAR
    if (crypto_metadata != NX_CRYPTO_NULL)
    {
        NX_CRYPTO_MEMSET(crypto_metadata, 0, sizeof(C68K_SHA256));
    }
#else
    NX_CRYPTO_PARAMETER_NOT_USED(crypto_metadata);
#endif

    return(NX_CRYPTO_SUCCESS);
}

static UINT ami_crypto_method_sha256_operation(UINT op,
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
                                               VOID (*nx_crypto_hw_process_callback)(VOID *packet_ptr, UINT status))
{

C68K_SHA256    *ctx;


    NX_CRYPTO_PARAMETER_NOT_USED(handle);
    NX_CRYPTO_PARAMETER_NOT_USED(key);
    NX_CRYPTO_PARAMETER_NOT_USED(key_size_in_bits);
    NX_CRYPTO_PARAMETER_NOT_USED(iv_ptr);
    NX_CRYPTO_PARAMETER_NOT_USED(packet_ptr);
    NX_CRYPTO_PARAMETER_NOT_USED(nx_crypto_hw_process_callback);

    if ((method == NX_CRYPTO_NULL) || (crypto_metadata == NX_CRYPTO_NULL) ||
        ((((ULONG)crypto_metadata) & 0x3u) != 0u) ||
        (crypto_metadata_size < sizeof(C68K_SHA256)))
    {
        return(NX_CRYPTO_PTR_ERROR);
    }

    ctx = (C68K_SHA256 *)crypto_metadata;

    switch (op)
    {
    case NX_CRYPTO_HASH_INITIALIZE:
        return(c68k_sha256_initialize(ctx, method -> nx_crypto_algorithm));

    case NX_CRYPTO_HASH_UPDATE:
        return(c68k_sha256_update(ctx, input, (UINT)input_length_in_byte));

    case NX_CRYPTO_HASH_CALCULATE:
        if (output_length_in_byte < C68K_SHA256_DIGEST_SIZE)
        {
            return(NX_CRYPTO_INVALID_BUFFER_SIZE);
        }
        return(c68k_sha256_digest_calculate(ctx, output,
                                            method -> nx_crypto_algorithm));

    default:
        break;
    }

    if (output_length_in_byte < C68K_SHA256_DIGEST_SIZE)
    {
        return(NX_CRYPTO_INVALID_BUFFER_SIZE);
    }

    (VOID)c68k_sha256_initialize(ctx, method -> nx_crypto_algorithm);
    (VOID)c68k_sha256_update(ctx, input, (UINT)input_length_in_byte);

    return(c68k_sha256_digest_calculate(ctx, output,
                                        method -> nx_crypto_algorithm));
}

static NX_CRYPTO_METHOD ami_crypto_method_sha256 =
{
    NX_CRYPTO_HASH_SHA256,
    0,
    0,
    NX_CRYPTO_SHA256_ICV_LEN_IN_BITS,
    NX_CRYPTO_SHA2_BLOCK_SIZE_IN_BYTES,
    sizeof(C68K_SHA256),
    ami_crypto_method_sha256_init,
    ami_crypto_method_sha256_cleanup,
    ami_crypto_method_sha256_operation
};


/* --------------------------------------------------------- HMAC-SHA256, */
/*
 * nx_crypto's own HMAC framing with the hash swapped underneath it.
 * _nx_crypto_hmac_metadata_set() takes the three hash entry points as
 * function pointers, and c68k_sha256_initialize / _update /
 * _digest_calculate have exactly those signatures.  So none of the key
 * shortening, ipad/opad or padding logic is reimplemented here.  Only the
 * compression function changes.
 */

typedef struct
{
    NX_CRYPTO_HMAC  ami_hmac;
    C68K_SHA256     ami_hmac_hash;
} AMI_CRYPTO_HMAC_SHA256;

static VOID ami_hmac_bind(AMI_CRYPTO_HMAC_SHA256 *ctx, UINT algorithm)
{

    _nx_crypto_hmac_metadata_set(&ctx -> ami_hmac, &ctx -> ami_hmac_hash,
                                 algorithm,
                                 NX_CRYPTO_SHA2_BLOCK_SIZE_IN_BYTES,
                                 C68K_SHA256_DIGEST_SIZE,
                                 (UINT (*)(VOID *, UINT))
                                     c68k_sha256_initialize,
                                 (UINT (*)(VOID *, UCHAR *, UINT))
                                     c68k_sha256_update,
                                 (UINT (*)(VOID *, UCHAR *, UINT))
                                     c68k_sha256_digest_calculate);
}

static UINT ami_crypto_method_hmac_sha256_init(struct NX_CRYPTO_METHOD_STRUCT *method,
                                               UCHAR *key,
                                               NX_CRYPTO_KEY_SIZE key_size_in_bits,
                                               VOID **handle,
                                               VOID *crypto_metadata,
                                               ULONG crypto_metadata_size)
{

    NX_CRYPTO_PARAMETER_NOT_USED(key);
    NX_CRYPTO_PARAMETER_NOT_USED(key_size_in_bits);
    NX_CRYPTO_PARAMETER_NOT_USED(handle);

    if ((method == NX_CRYPTO_NULL) || (crypto_metadata == NX_CRYPTO_NULL) ||
        ((((ULONG)crypto_metadata) & 0x3u) != 0u) ||
        (crypto_metadata_size < sizeof(AMI_CRYPTO_HMAC_SHA256)))
    {
        return(NX_CRYPTO_PTR_ERROR);
    }

    return(NX_CRYPTO_SUCCESS);
}

static UINT ami_crypto_method_hmac_sha256_cleanup(VOID *crypto_metadata)
{

#ifdef NX_SECURE_KEY_CLEAR
    if (crypto_metadata != NX_CRYPTO_NULL)
    {
        NX_CRYPTO_MEMSET(crypto_metadata, 0, sizeof(AMI_CRYPTO_HMAC_SHA256));
    }
#else
    NX_CRYPTO_PARAMETER_NOT_USED(crypto_metadata);
#endif

    return(NX_CRYPTO_SUCCESS);
}

static UINT ami_crypto_method_hmac_sha256_operation(UINT op,
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
                                                    VOID (*nx_crypto_hw_process_callback)(VOID *packet_ptr, UINT status))
{

AMI_CRYPTO_HMAC_SHA256 *ctx;
ULONG                   want;


    NX_CRYPTO_PARAMETER_NOT_USED(handle);
    NX_CRYPTO_PARAMETER_NOT_USED(iv_ptr);
    NX_CRYPTO_PARAMETER_NOT_USED(packet_ptr);
    NX_CRYPTO_PARAMETER_NOT_USED(nx_crypto_hw_process_callback);

    if ((method == NX_CRYPTO_NULL) || (crypto_metadata == NX_CRYPTO_NULL) ||
        ((((ULONG)crypto_metadata) & 0x3u) != 0u) ||
        (crypto_metadata_size < sizeof(AMI_CRYPTO_HMAC_SHA256)))
    {
        return(NX_CRYPTO_PTR_ERROR);
    }

    ctx = (AMI_CRYPTO_HMAC_SHA256 *)crypto_metadata;

    /* Rebound on every call, as the vendored operation does.  The metadata is
       the caller's memory and can be moved or zeroed between calls, and the
       pointers inside it are self-referential. */
    ami_hmac_bind(ctx, method -> nx_crypto_algorithm);

    want = (ULONG)(method -> nx_crypto_ICV_size_in_bits >> 3);

    switch (op)
    {
    case NX_CRYPTO_HASH_INITIALIZE:
        if (key == NX_CRYPTO_NULL)
        {
            return(NX_CRYPTO_PTR_ERROR);
        }
        return(_nx_crypto_hmac_initialize(&ctx -> ami_hmac, key,
                                          (UINT)(key_size_in_bits >> 3)));

    case NX_CRYPTO_HASH_UPDATE:
        return(_nx_crypto_hmac_update(&ctx -> ami_hmac, input,
                                      (UINT)input_length_in_byte));

    case NX_CRYPTO_HASH_CALCULATE:
        if (output_length_in_byte == 0uL)
        {
            return(NX_CRYPTO_INVALID_BUFFER_SIZE);
        }
        if (output_length_in_byte < want)
        {
            want = output_length_in_byte;
        }
        return(_nx_crypto_hmac_digest_calculate(&ctx -> ami_hmac, output,
                                                (UINT)want));

    default:
        break;
    }

    if (key == NX_CRYPTO_NULL)
    {
        return(NX_CRYPTO_PTR_ERROR);
    }
    if (output_length_in_byte < want)
    {
        want = output_length_in_byte;
    }

    return(_nx_crypto_hmac(&ctx -> ami_hmac, input,
                           (UINT)input_length_in_byte,
                           key, (UINT)(key_size_in_bits >> 3),
                           output, (UINT)want));
}

static NX_CRYPTO_METHOD ami_crypto_method_hmac_sha256 =
{
    NX_CRYPTO_AUTHENTICATION_HMAC_SHA2_256,
    0,
    0,
    NX_CRYPTO_HMAC_SHA256_ICV_FULL_LEN_IN_BITS,
    NX_CRYPTO_SHA2_BLOCK_SIZE_IN_BYTES,
    sizeof(AMI_CRYPTO_HMAC_SHA256),
    ami_crypto_method_hmac_sha256_init,
    ami_crypto_method_hmac_sha256_cleanup,
    ami_crypto_method_hmac_sha256_operation
};


/*
 * AMINETXDUO_TLS_STOCK_BULK selects the vendored AES and SHA-256 in the tables
 * below, with everything else unchanged: RSA, P-256, the suites and their
 * order.
 *
 * For measurement only.  A number from last week does not answer whether the
 * record path got faster on the wire.  The TCP layer moved underneath it
 * (delayed ACKs, 78b4ed9) and so did the input path (SOCK_RAW's per-packet
 * filter, 026c348).  Two tls.library binaries built from the same commit,
 * different only in this define, run through tests/curl/run-curlverify.sh
 * give a before and an after that cannot contain anybody else's work.
 *
 * Not a fallback and not a supported configuration.  The fallback for a
 * suspected bug in the assembly is AMINETXDUO_CRYPTO68K_ASM=OFF, which keeps
 * these methods and swaps the primitives underneath them.
 *
 * All or nothing: one row swapped by hand answers NX_CRYPTO_PTR_ERROR.
 * _nx_secure_tls_session_create_ext() partitions the metadata area once from
 * the sizes in these tables, and the two SHA-256 contexts differ -- 108 bytes
 * against 360, because the vendored one keeps the 64-word message schedule
 * that c68k_sha256 builds on the stack.  A half-swapped table gets a region
 * sized from one and an init that checks against the other.
 */
#ifdef AMINETXDUO_TLS_STOCK_BULK
#define AMI_BULK_AES128     crypto_method_aes_cbc_128
#define AMI_BULK_AES256     crypto_method_aes_cbc_256
#define AMI_BULK_SHA256     crypto_method_sha256
#define AMI_BULK_HMAC256    crypto_method_hmac_sha256
#else
#define AMI_BULK_AES128     ami_crypto_method_aes_cbc_128
#define AMI_BULK_AES256     ami_crypto_method_aes_cbc_256
#define AMI_BULK_SHA256     ami_crypto_method_sha256
#define AMI_BULK_HMAC256    ami_crypto_method_hmac_sha256
#endif


/* ------------------------------------------------------------- the tables, */

/* Everything untouched comes straight from nx_crypto_methods.c. */
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
 * _nx_crypto_x509_cipher_lookup_table_ecc with crypto_method_rsa swapped.  The
 * ECDSA rows need no change, because crypto_method_ecdsa reaches P-256 through
 * the curve method, which is the one above.
 *
 * MD5 and SHA-1 are gone from it.  The table does two jobs: it is what a
 * certificate signature is checked with, and
 * _nx_secure_tls_send_clienthello_extensions() walks it to build the
 * signature_algorithms extension, so a row here is also a row on the wire.
 * MD5 collisions produced a working rogue CA certificate in 2008, and SHA-1
 * chosen-prefix collisions cost a few tens of thousands of dollars since 2020.
 * A signature made with either is not evidence of anything, so this stack
 * stopped both accepting one and offering to accept one.
 *
 * The cost: a chain whose leaf or intermediate is signed with SHA-1 no longer
 * passes the check.  No public CA issued one after 2016, but a device on a
 * private network can still present one, and that connection now fails where
 * it used to work.  A self-signed root in the trust store is unaffected
 * whatever it is signed with.  The walk stops when it reaches the trusted
 * store, and never checks a root's signature on itself.
 */
static NX_SECURE_X509_CRYPTO ami_x509_cipher_table[] =
{
    /* OID identifier,                       public cipher,             hash method */
    {NX_SECURE_TLS_X509_TYPE_ECDSA_SHA_256,  &crypto_method_ecdsa,      &AMI_BULK_SHA256},
    {NX_SECURE_TLS_X509_TYPE_ECDSA_SHA_384,  &crypto_method_ecdsa,      &crypto_method_sha384},
    {NX_SECURE_TLS_X509_TYPE_ECDSA_SHA_512,  &crypto_method_ecdsa,      &crypto_method_sha512},
    {NX_SECURE_TLS_X509_TYPE_RSA_SHA_256,    &ami_crypto_method_rsa,    &AMI_BULK_SHA256},
    {NX_SECURE_TLS_X509_TYPE_RSA_SHA_384,    &ami_crypto_method_rsa,    &crypto_method_sha384},
    {NX_SECURE_TLS_X509_TYPE_RSA_SHA_512,    &ami_crypto_method_rsa,    &crypto_method_sha512},
    {NX_SECURE_TLS_X509_TYPE_ECDSA_SHA_224,  &crypto_method_ecdsa,      &crypto_method_sha224},
};

/*
 * The ciphersuite table.  This is the list a ClientHello carries, in the order
 * it carries it, so it is also the client's stated preference.
 *
 * ChaCha20-Poly1305 is first.  It must be present because the CBC suites
 * underneath it no longer reach a large and growing share of the web, and
 * Google's front end refuses a ClientHello that offers only those.  It is
 * first because it is also the cheaper record path on this machine: ~120
 * cycles a byte for the cipher and ~67 for the authenticator, against
 * AES-128-CBC's measured 233 and HMAC-SHA256's 236 (docs/RESEARCH.md 18.2 and
 * 19).
 *
 * The CBC pair stays underneath it, unchanged and in its old order, because
 * many servers still speak nothing else.
 *
 * AES-GCM is absent.  It restores the same reach, at 344.6 ms for 1 KB
 * against AES-CBC's 21.9 (docs/RESEARCH.md 5.5), because nx_crypto_gcm.c's
 * GHASH is a bit-serial GF(2^128) multiply and a 68k has no carry-less
 * multiply to build it from.  An offer of it lets some servers negotiate a
 * download nobody waits for, and no server takes GCM but neither
 * ChaCha20-Poly1305 nor CBC.
 */
static NX_SECURE_TLS_CIPHERSUITE_INFO ami_ciphersuite_table[] =
{
    /* Ciphersuite,                           public cipher,            public_auth,              session cipher & mode,          iv, key, hash method,                hash size, TLS PRF */
#if (NX_SECURE_TLS_TLS_1_3_ENABLED)
    {TLS_AES_128_GCM_SHA256,                  &crypto_method_ecdhe,     &crypto_method_ecdsa,     &crypto_method_aes_128_gcm_16,  96, 16,  &AMI_BULK_SHA256,          32,        &crypto_method_hkdf},
    {TLS_AES_128_CCM_SHA256,                  &crypto_method_ecdhe,     &crypto_method_ecdsa,     &crypto_method_aes_ccm_16,      96, 16,  &AMI_BULK_SHA256,          32,        &crypto_method_hkdf},
    {TLS_AES_128_CCM_8_SHA256,                &crypto_method_ecdhe,     &crypto_method_ecdsa,     &crypto_method_aes_ccm_8,       96, 16,  &AMI_BULK_SHA256,          32,        &crypto_method_hkdf},
#endif

    {TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256, &crypto_method_ecdhe, &crypto_method_ecdsa,   &ami_crypto_method_chacha20_poly1305, 12, 32, &crypto_method_null, 0,   &crypto_method_tls_prf_sha256},
    {TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,   &crypto_method_ecdhe, &ami_crypto_method_rsa, &ami_crypto_method_chacha20_poly1305, 12, 32, &crypto_method_null, 0,   &crypto_method_tls_prf_sha256},

    {TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256, &crypto_method_ecdhe,     &crypto_method_ecdsa,     &AMI_BULK_AES128,     16, 16,  &AMI_BULK_HMAC256, 32,        &crypto_method_tls_prf_sha256},
    {TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256,   &crypto_method_ecdhe,     &ami_crypto_method_rsa,   &AMI_BULK_AES128,     16, 16,  &AMI_BULK_HMAC256, 32,        &crypto_method_tls_prf_sha256},

    {TLS_RSA_WITH_AES_256_CBC_SHA256,         &ami_crypto_method_rsa,   &ami_crypto_method_rsa,   &AMI_BULK_AES256,     16, 32,  &AMI_BULK_HMAC256, 32,        &crypto_method_tls_prf_sha256},
    {TLS_RSA_WITH_AES_128_CBC_SHA256,         &ami_crypto_method_rsa,   &ami_crypto_method_rsa,   &AMI_BULK_AES128,     16, 16,  &AMI_BULK_HMAC256, 32,        &crypto_method_tls_prf_sha256},
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
    &AMI_BULK_SHA256,
    &crypto_method_tls_prf_sha256,
#endif

#if (NX_SECURE_TLS_TLS_1_3_ENABLED)
    &crypto_method_hkdf,
    &crypto_method_hmac,
    &crypto_method_ecdhe,
#endif
};

/*
 * The curve list.  P-256 is first, and is the only entry crypto68k
 * accelerates.  P-384 and P-521 stay on the vendored arithmetic, slow enough
 * that no handshake wants them, and present so a server that insists on one
 * still works.
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

/*
 * What the ClientHello offers, which is not the same list.  TLS 1.3 generates
 * a key pair for every group it offers before the ClientHello goes out, and a
 * P-384 and a P-521 key pair together cost about four seconds here, spent
 * before the first byte of the handshake reaches the server.  A server that
 * picks either of them is slower still, so only P-256 is offered.  The table
 * above stays complete, because certificate chains carry P-384 keys that
 * still have to be checked.
 */
const USHORT ami_crypto_ecc_offered_groups[] =
{
    (USHORT)NX_CRYPTO_EC_SECP256R1,
};

const NX_CRYPTO_METHOD *ami_crypto_ecc_offered_curves[] =
{
    &ami_crypto_method_ec_secp256,
};

const UINT ami_crypto_ecc_offered_groups_size =
    sizeof(ami_crypto_ecc_offered_groups) / sizeof(USHORT);
