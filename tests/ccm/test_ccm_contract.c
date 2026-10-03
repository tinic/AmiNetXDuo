/*
 * N-147/N-148: the vendored AES-CCM against independent known answers.
 *
 * Every vector is Python `cryptography`'s AESCCM (gen_ccm_vectors.py), so a
 * NetX build that only agrees with itself cannot pass.  Payloads straddle the
 * counter: 4080 bytes is 255 blocks, 4096 the 256th (the first carry out of
 * the low counter octet), 16384 the TLS record cap.  AAD 5 and 13 are the TLS
 * 1.3 and 1.2 headers; 0, 65279, 65280 and 70000 are RFC 3610 2.2's empty,
 * 2-octet and 6-octet l(a) forms.  Tags 16 and 8 are the two CCM methods.
 *
 * Each case runs the record layer's three-step path (INITIALIZE, one UPDATE
 * for the whole record, CALCULATE) both ways, the one-shot ENCRYPT/DECRYPT,
 * and a decrypt with one tag bit flipped, which must be refused.
 *
 * Output is key=value; the exit status is the verdict.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nx_crypto_aes.h"
#include "ccm_vectors.h"

extern NX_CRYPTO_METHOD crypto_method_aes_ccm_8;
extern NX_CRYPTO_METHOD crypto_method_aes_ccm_16;

#define CCM_VEC_MAXAAD 70000

static NX_CRYPTO_AES ctx __attribute__((aligned(8)));
static UCHAR plain[CCM_VEC_MAXLEN];
static UCHAR aad[CCM_VEC_MAXAAD];
static UCHAR out[CCM_VEC_MAXLEN + 16];
static UCHAR back[CCM_VEC_MAXLEN + 16];
static UCHAR iv[13];

static int failures;
static int checks;

static NX_CRYPTO_METHOD *method_for(unsigned tag_len)
{
    return (tag_len == 8) ? &crypto_method_aes_ccm_8 : &crypto_method_aes_ccm_16;
}

static UINT keyed(NX_CRYPTO_METHOD *m)
{
VOID *handle = NX_CRYPTO_NULL;

    memset(&ctx, 0, sizeof(ctx));
    return m -> nx_crypto_init(m, (UCHAR *)ccm_vec_key, 128, &handle, &ctx, sizeof(ctx));
}

static UINT op(NX_CRYPTO_METHOD *m, UINT o, UCHAR *in, ULONG in_len, UCHAR *o_ptr, ULONG o_len)
{
    return m -> nx_crypto_operation(o, NX_CRYPTO_NULL, m, (UCHAR *)ccm_vec_key, 128,
                                    in, in_len, iv, o_ptr, o_len, &ctx, sizeof(ctx),
                                    NX_CRYPTO_NULL, NX_CRYPTO_NULL);
}

/* First 16-byte block where got differs from the reference ciphertext, or -1. */
static long first_bad_block(const UCHAR *got, unsigned len)
{
unsigned i;

    for (i = 0; i < len; i++)
    {
        if (got[i] != ccm_vec_ciphertext[i])
        {
            return (long)(i / 16);
        }
    }
    return -1;
}

static void expect(int ok, unsigned idx, const char *path, const char *what, long block)
{
    checks++;
    if (!ok)
    {
        failures++;
        printf("fail case=%u payload=%u aad=%u tag=%u path=%s what=%s block=%ld\n",
               idx, ccm_vec_cases[idx].payload, ccm_vec_cases[idx].aad,
               ccm_vec_cases[idx].tag_len, path, what, block);
    }
}

static void run_case(unsigned idx)
{
unsigned          p = ccm_vec_cases[idx].payload;
unsigned          a = ccm_vec_cases[idx].aad;
unsigned          t = ccm_vec_cases[idx].tag_len;
const UCHAR      *tag = ccm_vec_cases[idx].tag;
NX_CRYPTO_METHOD *m = method_for(t);
UCHAR            *a_ptr = a ? aad : NX_CRYPTO_NULL;
UCHAR             icv[16];
UINT              st;

    /* Record path, encrypt: as nx_secure_tls_record_payload_encrypt.c.  */
    memset(out, 0xA5, sizeof(out));
    st = keyed(m);
    st |= op(m, NX_CRYPTO_ENCRYPT_INITIALIZE, a_ptr, a, NX_CRYPTO_NULL, p);
    st |= op(m, NX_CRYPTO_ENCRYPT_UPDATE, plain, p, out, p);
    st |= op(m, NX_CRYPTO_ENCRYPT_CALCULATE, NX_CRYPTO_NULL, 0, icv, t);
    expect(st == NX_CRYPTO_SUCCESS, idx, "record", "encrypt_status", -1);
    expect(memcmp(out, ccm_vec_ciphertext, p) == 0, idx, "record", "ciphertext",
           first_bad_block(out, p));
    expect(memcmp(icv, tag, t) == 0, idx, "record", "tag", -1);

    /* Record path, decrypt.  */
    memset(back, 0xA5, sizeof(back));
    memcpy(icv, tag, t);
    st = keyed(m);
    st |= op(m, NX_CRYPTO_DECRYPT_INITIALIZE, a_ptr, a, NX_CRYPTO_NULL, p);
    st |= op(m, NX_CRYPTO_DECRYPT_UPDATE, (UCHAR *)ccm_vec_ciphertext, p, back, p);
    st |= op(m, NX_CRYPTO_DECRYPT_CALCULATE, icv, t, NX_CRYPTO_NULL, 0);
    expect(st == NX_CRYPTO_SUCCESS, idx, "record", "decrypt_status", -1);
    expect(memcmp(back, plain, p) == 0, idx, "record", "plaintext", -1);

    /* One shot, encrypt: tag appended to the ciphertext.  */
    memset(out, 0xA5, sizeof(out));
    st = keyed(m);
    st |= op(m, NX_CRYPTO_SET_ADDITIONAL_DATA, a_ptr, a, NX_CRYPTO_NULL, 0);
    st |= op(m, NX_CRYPTO_ENCRYPT, plain, p, out, p + t);
    expect(st == NX_CRYPTO_SUCCESS, idx, "oneshot", "encrypt_status", -1);
    expect(memcmp(out, ccm_vec_ciphertext, p) == 0, idx, "oneshot", "ciphertext",
           first_bad_block(out, p));
    expect(memcmp(out + p, tag, t) == 0, idx, "oneshot", "tag", -1);

    /* One shot, decrypt the reference.  */
    memcpy(out, ccm_vec_ciphertext, p);
    memcpy(out + p, tag, t);
    memset(back, 0xA5, sizeof(back));
    st = keyed(m);
    st |= op(m, NX_CRYPTO_SET_ADDITIONAL_DATA, a_ptr, a, NX_CRYPTO_NULL, 0);
    st |= op(m, NX_CRYPTO_DECRYPT, out, p + t, back, p);
    expect(st == NX_CRYPTO_SUCCESS, idx, "oneshot", "decrypt_status", -1);
    expect(memcmp(back, plain, p) == 0, idx, "oneshot", "plaintext", -1);

    /* A flipped tag bit is refused.  */
    out[p + t - 1] ^= 0x01;
    st = keyed(m);
    st |= op(m, NX_CRYPTO_SET_ADDITIONAL_DATA, a_ptr, a, NX_CRYPTO_NULL, 0);
    st = op(m, NX_CRYPTO_DECRYPT, out, p + t, back, p);
    expect(st == NX_CRYPTO_AUTHENTICATION_FAILED, idx, "oneshot", "tamper_refused", -1);
}

int main(void)
{
unsigned i;
unsigned n = (unsigned)(sizeof(ccm_vec_cases) / sizeof(ccm_vec_cases[0]));
int      before;

    for (i = 0; i < CCM_VEC_MAXLEN; i++)
    {
        plain[i] = (UCHAR)((i * 31) + 7);
    }
    for (i = 0; i < CCM_VEC_MAXAAD; i++)
    {
        aad[i] = (UCHAR)((i * 13) + 101);
    }
    iv[0] = 12;
    memcpy(iv + 1, ccm_vec_nonce, 12);

    for (i = 0; i < n; i++)
    {
        before = failures;
        run_case(i);
        printf("case=%u payload=%u aad=%u tag=%u result=%s\n", i, ccm_vec_cases[i].payload,
               ccm_vec_cases[i].aad, ccm_vec_cases[i].tag_len,
               (failures == before) ? "pass" : "FAIL");
    }

    printf("ccm_contract cases=%u checks=%d failures=%d\n", n, checks, failures);
    return failures ? 1 : 0;
}
