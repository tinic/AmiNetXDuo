/*
 * updsig: see updsig.h.  SHA-512 for c68k_ed25519_verify() is NetX Duo's,
 * bound the way tests/crypto68k/host/test_c68k_25519.c binds it; built with
 * NX_CRYPTO_STANDALONE_ENABLE.
 *
 * SPDX-License-Identifier: MIT
 */

#include "updsig.h"

#include "nx_crypto_sha5.h"
#include "aminetxduo/nxstatus.h"
#include "c68k_25519.h"

static void us_sha512(unsigned char out[64],
                      const unsigned char *a, unsigned long alen,
                      const unsigned char *b, unsigned long blen,
                      const unsigned char *c, unsigned long clen)
{
    static NX_CRYPTO_SHA512 ctx;

    /* Each fails only on a NULL context, and ctx is static. */
    AMI_NX_ONLY_SUCCESS(_nx_crypto_sha512_initialize(&ctx,
                                                     NX_CRYPTO_HASH_SHA512));
    if (alen)
        AMI_NX_ONLY_SUCCESS(_nx_crypto_sha512_update(&ctx, (UCHAR *)a,
                                                     (UINT)alen));
    if (blen)
        AMI_NX_ONLY_SUCCESS(_nx_crypto_sha512_update(&ctx, (UCHAR *)b,
                                                     (UINT)blen));
    if (clen)
        AMI_NX_ONLY_SUCCESS(_nx_crypto_sha512_update(&ctx, (UCHAR *)c,
                                                     (UINT)clen));
    AMI_NX_ONLY_SUCCESS(_nx_crypto_sha512_digest_calculate(&ctx, out,
                                                     NX_CRYPTO_HASH_SHA512));
}

static int us_same(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b)
    {
        a++;
        b++;
    }

    return *a == *b;
}

UpdResult upd_verify(const unsigned char *m, unsigned long mlen,
                     const char *sig, unsigned long siglen,
                     const UpdKey *keys, unsigned int nkeys)
{
    UpdSigLine   lines[UPD_SIG_LINES];
    unsigned int nlines, i, k;
    int          known = 0;
    UpdResult    r;

    if (m == 0 || keys == 0 || nkeys == 0 || nkeys > UPD_KEYS_MAX)
        return UPD_E_ARG;

    if ((r = upd_sig_parse(sig, siglen, lines, &nlines)) != UPD_OK)
        return r;

    for (i = 0; i < nlines; i++)
    {
        for (k = 0; k < nkeys; k++)
        {
            if (!us_same(lines[i].id, keys[k].id))
                continue;

            known = 1;
            if (c68k_ed25519_verify(us_sha512, m, mlen, lines[i].sig,
                                    keys[k].pk) == 0)
                return UPD_OK;
        }
    }

    return known ? UPD_E_SIG_BAD : UPD_E_SIG_UNKNOWN_KEY;
}
