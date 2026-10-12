/*
 * NetWireless: PBKDF2-HMAC-SHA1 for the WPA key.  See the header.
 *
 * HMAC's two padded key blocks are hashed once; every iteration after that is
 * one compression from each saved state, a 20-byte message padded in place.
 * 8,192 iterations is then 16,384 compressions rather than 32,768, which is
 * the difference a 68000 notices.
 *
 * _NX_CRYPTO_INITIALIZE_ must be defined in exactly one translation unit of a
 * program; this is that unit for NetWirelessPrefs.
 *
 * SPDX-License-Identifier: MIT
 */

#define _NX_CRYPTO_INITIALIZE_

#include "nx_crypto_sha1.h"

#include "netwireless_key.h"
#include "aminetxduo/nxstatus.h"

typedef struct NwSha1State
{
    ULONG h[5];
} NwSha1State;

static NX_CRYPTO_SHA1 nw_ctx;
static UCHAR          nw_block[64];

/* One compression of a final block holding len < 56 message bytes, after
   one 64-byte block (the HMAC pad) already in from. */
static void nw_sha1_final(const NwSha1State *from, const UCHAR *msg, UINT len,
                          UCHAR out[20])
{
    ULONG bits = (64UL + len) * 8UL;
    UINT  i;

    for (i = 0; i < 64; i++) nw_block[i] = 0;
    for (i = 0; i < len; i++) nw_block[i] = msg[i];
    nw_block[len] = 0x80;
    nw_block[62] = (UCHAR)(bits >> 8);
    nw_block[63] = (UCHAR)bits;
    for (i = 0; i < 5; i++) nw_ctx.nx_sha1_states[i] = from->h[i];
    AMI_NX_ONLY_SUCCESS(_nx_crypto_sha1_process_buffer(&nw_ctx, nw_block));
    for (i = 0; i < 5; i++)
    {
        ULONG v = nw_ctx.nx_sha1_states[i];
        out[4 * i]     = (UCHAR)(v >> 24);
        out[4 * i + 1] = (UCHAR)(v >> 16);
        out[4 * i + 2] = (UCHAR)(v >> 8);
        out[4 * i + 3] = (UCHAR)v;
    }
}

/* The state after the key XOR pad, the first block of HMAC's inner or
   outer hash. */
static int nw_sha1_pad(const char *key, size_t len, UCHAR pad, NwSha1State *s)
{
    UINT i;

    if (_nx_crypto_sha1_initialize(&nw_ctx, NX_CRYPTO_HASH_SHA1) !=
        NX_CRYPTO_SUCCESS)
        return 0;
    for (i = 0; i < 64; i++)
        nw_block[i] = (UCHAR)((i < len ? (UCHAR)key[i] : 0) ^ pad);
    AMI_NX_ONLY_SUCCESS(_nx_crypto_sha1_process_buffer(&nw_ctx, nw_block));
    for (i = 0; i < 5; i++) s->h[i] = nw_ctx.nx_sha1_states[i];
    return 1;
}

int nw_key_kind(const char *pass, size_t len)
{
    size_t i;

    if (len == 2 * NW_KEY_LEN)
    {
        for (i = 0; i < len; i++)
        {
            char c = pass[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F')))
                break;
        }
        if (i == len) return NW_PASS_HEX;
    }
    if (len < 8 || len > 63) return NW_PASS_BAD;
    for (i = 0; i < len; i++)
        if ((unsigned char)pass[i] < 0x20 || (unsigned char)pass[i] > 0x7e)
            return NW_PASS_BAD;
    return NW_PASS_TEXT;
}

int nw_key_derive(const char *pass, size_t pass_len,
                  const unsigned char *ssid, size_t ssid_len,
                  unsigned char key[NW_KEY_LEN],
                  NwKeyProgress progress, void *ctx)
{
    NwSha1State   inner, outer;
    UCHAR         u[20], t[20], msg[36];
    unsigned long done = 0;
    UINT          block, iter, i;

    if (pass_len < 1 || pass_len > 63 || ssid_len > 32) return 0;

    if (!nw_sha1_pad(pass, pass_len, 0x36, &inner) ||
        !nw_sha1_pad(pass, pass_len, 0x5c, &outer))
        return 0;

    for (block = 1; block <= 2; block++)
    {
        for (i = 0; i < ssid_len; i++) msg[i] = ssid[i];
        msg[ssid_len]     = 0;
        msg[ssid_len + 1] = 0;
        msg[ssid_len + 2] = 0;
        msg[ssid_len + 3] = (UCHAR)block;

        for (iter = 0; iter < NW_KEY_ITERATIONS; iter++)
        {
            if (iter == 0)
                nw_sha1_final(&inner, msg, (UINT)ssid_len + 4, u);
            else
                nw_sha1_final(&inner, u, 20, u);
            nw_sha1_final(&outer, u, 20, u);
            for (i = 0; i < 20; i++) t[i] = (UCHAR)(iter == 0 ? u[i] : t[i] ^ u[i]);
            if ((++done & 255) == 0 && progress != NULL &&
                !progress(ctx, done))
                return 0;
        }

        for (i = 0; i < 20 && (block - 1) * 20 + i < NW_KEY_LEN; i++)
            key[(block - 1) * 20 + i] = t[i];
    }
    return 1;
}

void nw_key_hex(const unsigned char key[NW_KEY_LEN], char out[2 * NW_KEY_LEN + 1])
{
    static const char hex[] = "0123456789abcdef";
    int i;

    for (i = 0; i < NW_KEY_LEN; i++)
    {
        out[2 * i]     = hex[key[i] >> 4];
        out[2 * i + 1] = hex[key[i] & 15];
    }
    out[2 * NW_KEY_LEN] = '\0';
}
