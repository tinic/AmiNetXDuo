/*
 * The one SHA-1 call httpws.c makes, over the SHA-1 already in this tree.
 * _NX_CRYPTO_INITIALIZE_ must be defined in exactly one translation unit of a
 * program; this is that unit.
 *
 * SPDX-License-Identifier: MIT
 */

#define _NX_CRYPTO_INITIALIZE_

#include "nx_crypto_sha1.h"

#include "httpws.h"

void http_ws_sha1(const unsigned char *data, unsigned long len,
                  unsigned char out[20])
{
    NX_CRYPTO_SHA1 ctx;

    (VOID)_nx_crypto_sha1_initialize(&ctx, NX_CRYPTO_HASH_SHA1);
    (VOID)_nx_crypto_sha1_update(&ctx, (UCHAR *)data, (UINT)len);
    (VOID)_nx_crypto_sha1_digest_calculate(&ctx, (UCHAR *)out,
                                           NX_CRYPTO_HASH_SHA1);
}
