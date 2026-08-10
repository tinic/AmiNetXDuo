/*
 * The one SHA-1 call httpws.c makes, over the SHA-1 that is already in this
 * tree.
 *
 * WHY IT IS A FILE OF ITS OWN
 *
 *   third_party/netxduo/crypto_libraries/src/nx_crypto_sha1.c is the tree's
 *   SHA-1 and there is no reason for a second one.  It is compiled here in the
 *   crypto library's standalone mode -- NX_CRYPTO_STANDALONE_ENABLE, its own
 *   escape hatch -- which swaps nx_api.h for src/tools/nx_crypto_port.h, so
 *   httpd gets the hash without linking any part of NetX Duo.  httpd is a
 *   bsdsocket.library application, the same as nc and telnet, and it must stay
 *   one.
 *
 *   Kept out of httpws.c because that file must compile on a host with
 *   nothing but a C compiler, which is what makes the frame codec testable.
 *   The types the crypto headers declare -- UINT, ULONG, VOID -- are also the
 *   ones exec/types.h declares, and only one of the two may be in a
 *   translation unit at a time.  This one has neither httpd.c's headers nor
 *   httpws.h's callers in it, so the question does not arise.
 *
 *   _NX_CRYPTO_INITIALIZE_ has to be defined in exactly one translation unit
 *   of a program: it is what turns the two memcpy/memset function pointers
 *   nx_crypto.h declares from extern into definitions.  This is that unit.
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
