/*
 * updsig, the Ed25519 check of update.manifest against the keys the tool
 * holds.  The signature file format is in updmanifest.h.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_UPDSIG_H
#define AMINETXDUO_UPDSIG_H

#include "updmanifest.h"

#define UPD_KEYS_MAX    3

typedef struct UpdKey
{
    const char    *id;                  /* as in the signature file        */
    unsigned char  pk[32];
} UpdKey;

/*
 * UPD_OK if some line of `sig` names a key in `keys` and verifies over all
 * `mlen` bytes of `m`.  Lines naming other keys are skipped.  A malformed
 * signature file is refused whole, with upd_sig_parse()'s result.
 */
UpdResult upd_verify(const unsigned char *m, unsigned long mlen,
                     const char *sig, unsigned long siglen,
                     const UpdKey *keys, unsigned int nkeys);

#endif /* AMINETXDUO_UPDSIG_H */
