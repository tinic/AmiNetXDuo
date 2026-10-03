/*
 * N-156: _nx_crypto_ecdh_setup() and _nx_crypto_ecdsa_sign() must return the
 * status of _nx_crypto_ec_key_pair_generation_extra(), and on a failure must
 * not extract, reduce or write anything from the key pair it did not make.
 *
 * The two files under test are compiled into this program against stubs of
 * the EC and huge-number layer below them: the key-pair generation returns
 * what the case injects, and every function after it counts its calls.  No
 * real point is ever computed, so a failed key pair is never read; the
 * question is only whether anything downstream was called, and whether the
 * caller's output buffers are untouched.  A success stub is the control: the
 * same calls must still be made and the output still written.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include "nx_crypto_ecdh.h"
#include "nx_crypto_ecdsa.h"

static int failures;
static int checks;

static void expect(int ok, const char *what)
{
    checks++;
    if (!ok)
    {
        printf("FAIL %s\n", what);
        failures++;
    }
}

/* ------------------------------------------------------------- stubs -- */

static UINT h_keygen_status;    /* what the key-pair generation returns */
static int  h_keygen_calls;
static int  h_downstream;       /* any call after the key pair */
static int  h_extract_calls;

#define H_KEY_BYTE  0xA5        /* what a key pair, made or half-made, holds */

UINT _nx_crypto_ec_key_pair_generation_extra(NX_CRYPTO_EC *curve, NX_CRYPTO_EC_POINT *g,
                                             NX_CRYPTO_HUGE_NUMBER *private_key,
                                             NX_CRYPTO_EC_POINT *public_key, HN_UBASE *scratch)
{
    UINT n = curve -> nx_crypto_ec_n.nx_crypto_huge_buffer_size;

    (void)g;
    (void)scratch;
    h_keygen_calls++;

    /* Written whether or not it succeeds: a failure can come after the
       private key is drawn. */
    memset(private_key -> nx_crypto_huge_number_data, H_KEY_BYTE, n);
    private_key -> nx_crypto_huge_number_size = n >> HN_SIZE_SHIFT;
    memset(public_key -> nx_crypto_ec_point_x.nx_crypto_huge_number_data, H_KEY_BYTE, n);
    public_key -> nx_crypto_ec_point_x.nx_crypto_huge_number_size = n >> HN_SIZE_SHIFT;

    return h_keygen_status;
}

VOID _nx_crypto_ec_point_extract_uncompressed(NX_CRYPTO_EC *curve, NX_CRYPTO_EC_POINT *point,
                                              UCHAR *byte_stream, UINT byte_stream_size,
                                              UINT *huge_number_size)
{
    UINT n = 1 + 2 * ((curve -> nx_crypto_ec_bits + 7) >> 3);

    (void)point;
    h_downstream++;
    h_extract_calls++;
    if (byte_stream_size < n)
    {
        *huge_number_size = 0;
        return;
    }
    byte_stream[0] = 0x04;
    memset(byte_stream + 1, H_KEY_BYTE, n - 1);
    *huge_number_size = n;
}

UINT _nx_crypto_huge_number_extract(NX_CRYPTO_HUGE_NUMBER *number, UCHAR *byte_stream,
                                    UINT byte_stream_size, UINT *huge_number_size)
{
    (void)number;
    h_downstream++;
    h_extract_calls++;
    if (byte_stream_size < 32)
        return NX_CRYPTO_SIZE_ERROR;
    memset(byte_stream, 0x11, 32);
    *huge_number_size = 32;
    return NX_CRYPTO_SUCCESS;
}

VOID _nx_crypto_huge_number_modulus(NX_CRYPTO_HUGE_NUMBER *dividend, NX_CRYPTO_HUGE_NUMBER *divisor)
{
    (void)dividend; (void)divisor;
    h_downstream++;
}

UINT _nx_crypto_huge_number_inverse_modulus(NX_CRYPTO_HUGE_NUMBER *a, NX_CRYPTO_HUGE_NUMBER *m,
                                            NX_CRYPTO_HUGE_NUMBER *r, HN_UBASE *scratch)
{
    (void)a; (void)m; (void)r; (void)scratch;
    h_downstream++;
    return NX_CRYPTO_SUCCESS;
}

VOID _nx_crypto_huge_number_multiply(NX_CRYPTO_HUGE_NUMBER *left, NX_CRYPTO_HUGE_NUMBER *right,
                                     NX_CRYPTO_HUGE_NUMBER *result)
{
    (void)left; (void)right;
    h_downstream++;
    result -> nx_crypto_huge_number_size = 1;
    result -> nx_crypto_huge_number_data[0] = 1;
}

VOID _nx_crypto_huge_number_add_unsigned(NX_CRYPTO_HUGE_NUMBER *left, NX_CRYPTO_HUGE_NUMBER *right)
{
    (void)left; (void)right;
    h_downstream++;
}

UINT _nx_crypto_huge_number_is_zero(NX_CRYPTO_HUGE_NUMBER *x)
{
    (void)x;
    h_downstream++;
    return NX_CRYPTO_FALSE;
}

/* Before the key pair in the sign: the private key and the hash. */
UINT _nx_crypto_huge_number_setup(NX_CRYPTO_HUGE_NUMBER *number, const UCHAR *byte_stream, UINT size)
{
    (void)byte_stream;
    number -> nx_crypto_huge_number_size = (size + HN_SIZE_ROUND) >> HN_SIZE_SHIFT;
    return NX_CRYPTO_SUCCESS;
}

VOID _nx_crypto_huge_number_shift_right(NX_CRYPTO_HUGE_NUMBER *x, UINT shift)
{
    (void)x; (void)shift;
}

/* The rest of both files (compute_secret, verify, the method entry points):
   not called here, defined so the program links. */
UINT _nx_crypto_ec_key_pair_stream_generate(NX_CRYPTO_EC *curve, UCHAR *output,
                                            ULONG output_length_in_byte,
                                            ULONG *actual_output_length, HN_UBASE *scratch)
{
    (void)curve; (void)output; (void)output_length_in_byte; (void)actual_output_length; (void)scratch;
    h_downstream++;
    return NX_CRYPTO_NOT_SUCCESSFUL;
}

UINT _nx_crypto_ec_point_setup(NX_CRYPTO_EC_POINT *point, UCHAR *byte_stream, UINT byte_stream_size)
{
    (void)point; (void)byte_stream; (void)byte_stream_size;
    h_downstream++;
    return NX_CRYPTO_NOT_SUCCESSFUL;
}

UINT _nx_crypto_ec_validate_public_key(NX_CRYPTO_EC_POINT *public_key, NX_CRYPTO_EC *chosen_curve,
                                       UINT partial, HN_UBASE *scratch)
{
    (void)public_key; (void)chosen_curve; (void)partial; (void)scratch;
    h_downstream++;
    return NX_CRYPTO_NOT_SUCCESSFUL;
}

UINT _nx_crypto_huge_number_compare_unsigned(NX_CRYPTO_HUGE_NUMBER *left, NX_CRYPTO_HUGE_NUMBER *right)
{
    (void)left; (void)right;
    h_downstream++;
    return NX_CRYPTO_HUGE_NUMBER_EQUAL;
}

UINT _nx_crypto_huge_number_extract_fixed_size(NX_CRYPTO_HUGE_NUMBER *number, UCHAR *byte_stream,
                                               UINT byte_stream_size)
{
    (void)number; (void)byte_stream; (void)byte_stream_size;
    h_downstream++;
    return NX_CRYPTO_NOT_SUCCESSFUL;
}

/* ------------------------------------------------------------- cases -- */

#define H_CANARY        0x5A
#define H_LEN_CANARY    0xDEADBEEFu

static NX_CRYPTO_EC curve;
static HN_UBASE     scratch[4096 / sizeof(HN_UBASE)];

static void reset(UINT keygen_status)
{
    memset(&curve, 0, sizeof(curve));
    curve.nx_crypto_ec_bits = 256;
    curve.nx_crypto_ec_n.nx_crypto_huge_buffer_size = 32;
    memset(scratch, 0, sizeof(scratch));

    h_keygen_status = keygen_status;
    h_keygen_calls  = 0;
    h_downstream    = 0;
    h_extract_calls = 0;
}

static int all_bytes(const void *p, UCHAR v, size_t n)
{
    const UCHAR *b = p;
    size_t       i;

    for (i = 0; i < n; i++)
        if (b[i] != v)
            return 0;
    return 1;
}

static void t_ecdh(UINT injected)
{
    NX_CRYPTO_ECDH ecdh;
    UCHAR          pub[1 + 2 * 32 + 16];
    ULONG          pub_len = H_LEN_CANARY;
    UINT           status;
    char           what[160];

    reset(injected);
    memset(&ecdh, 0, sizeof(ecdh));
    memset(pub, H_CANARY, sizeof(pub));

    status = _nx_crypto_ecdh_setup(&ecdh, pub, sizeof(pub), &pub_len, &curve, scratch);

    snprintf(what, sizeof(what), "ECDH setup, key pair 0x%x: status 0x%x returned", injected, status);
    expect(status == injected, what);
    expect(h_keygen_calls == 1, "ECDH setup: one key pair asked for");

    if (injected == NX_CRYPTO_SUCCESS)
    {
        expect(h_extract_calls == 1, "ECDH setup (control): the public key is extracted");
        expect(pub_len == 65 && pub[0] == 0x04, "ECDH setup (control): a 65-byte uncompressed key");
        expect(all_bytes(ecdh.nx_crypto_ecdh_private_key_buffer, H_KEY_BYTE, 32),
               "ECDH setup (control): the private key is kept");
        return;
    }

    snprintf(what, sizeof(what), "ECDH setup, key pair 0x%x: %d calls after it, want 0",
             injected, h_downstream);
    expect(h_downstream == 0, what);
    expect(all_bytes(pub, H_CANARY, sizeof(pub)), "ECDH setup failed: the public key buffer is untouched");
    expect(pub_len == H_LEN_CANARY, "ECDH setup failed: no public key length is written");
    expect(all_bytes(ecdh.nx_crypto_ecdh_private_key_buffer, 0,
                     sizeof(ecdh.nx_crypto_ecdh_private_key_buffer)),
           "ECDH setup failed: no half-made private key is left");
}

static void t_ecdsa(UINT injected)
{
    UCHAR hash[32];
    UCHAR key[32];
    UCHAR sig[2 * 32 + 9 + 16];
    ULONG sig_len = H_LEN_CANARY;
    UINT  status;
    char  what[160];

    reset(injected);
    memset(hash, 0x33, sizeof(hash));
    memset(key, 0x44, sizeof(key));
    memset(sig, H_CANARY, sizeof(sig));

    status = _nx_crypto_ecdsa_sign(&curve, hash, sizeof(hash), key, sizeof(key),
                                   sig, sizeof(sig), &sig_len, scratch);

    snprintf(what, sizeof(what), "ECDSA sign, key pair 0x%x: status 0x%x returned", injected, status);
    expect(status == injected, what);
    expect(h_keygen_calls == 1, "ECDSA sign: one key pair asked for");

    if (injected == NX_CRYPTO_SUCCESS)
    {
        expect(h_extract_calls == 2, "ECDSA sign (control): r and s are extracted");
        expect(sig[0] == 0x30 && sig_len != H_LEN_CANARY && sig_len <= sizeof(sig),
               "ECDSA sign (control): a DER SEQUENCE is written");
        return;
    }

    snprintf(what, sizeof(what), "ECDSA sign, key pair 0x%x: %d calls after it, want 0",
             injected, h_downstream);
    expect(h_downstream == 0, what);
    expect(all_bytes(sig, H_CANARY, sizeof(sig)), "ECDSA sign failed: the signature buffer is untouched");
    expect(sig_len == H_LEN_CANARY, "ECDSA sign failed: no signature length is written");
}

int main(void)
{
    static const UINT injected[] =
    {
        NX_CRYPTO_NOT_SUCCESSFUL,   /* the RBG */
        NX_CRYPTO_SIZE_ERROR,       /* a number that does not fit */
    };
    unsigned i;

    t_ecdh(NX_CRYPTO_SUCCESS);
    t_ecdsa(NX_CRYPTO_SUCCESS);

    for (i = 0; i < sizeof(injected) / sizeof(injected[0]); i++)
    {
        t_ecdh(injected[i]);
        t_ecdsa(injected[i]);
    }

    printf("ec_keygen_status: %d checks, %d failures\n", checks, failures);

    return failures == 0 ? 0 : 1;
}
