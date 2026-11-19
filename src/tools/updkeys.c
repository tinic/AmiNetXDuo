/*
 * updkeys, the public keys UpdateAmiNetXDuo accepts a manifest signed by.
 *
 * A build with -DAMINETXDUO_UPDATE_TEST_KEY=ON (UPD_TEST_KEY) knows exactly
 * one key, test-a, whose private half is committed as
 * tests/updater/test-only-update-key-a.pem: anyone can sign for it, so such
 * a build is for the scenario tests only and never ships.
 *
 * Every other build knows the release keys below, and only those.  The
 * private half of each is the secret AMINETXDUO_UPDATE_SIGNING_KEY of the
 * GitHub environment `release` and an offline copy; release.yml signs with
 * it (tools/sign-update-manifest.sh).  Up to UPD_KEYS_MAX keys, so that a
 * rotation can ship the next key beside the current one.  With no key
 * listed, upd_key_count is 0 and the updater refuses every manifest before
 * it fetches one.
 *
 * tools/check-release-candidate.sh reads both tables below, refuses a
 * candidate whose UpdateAmiNetXDuo does not hold every release key or holds
 * the test key, and so expects this layout: the id string, then the 32
 * bytes as 0x.. literals.
 *
 * To add one:
 *   openssl pkey -in <key>.pem -pubout -outform DER | tail -c 32 | xxd -i
 *
 * SPDX-License-Identifier: MIT
 */

#include "updkeys.h"

#ifdef UPD_TEST_KEY

const UpdKey upd_keys[] = {
    { "test-a",
      { 0x1d, 0x81, 0x8a, 0xde, 0x40, 0xee, 0xe9, 0xda,
        0xac, 0x03, 0x4d, 0xd1, 0x66, 0xee, 0xbd, 0xab,
        0xc0, 0xcd, 0x61, 0x16, 0xeb, 0x46, 0x8b, 0x18,
        0x46, 0xb0, 0x44, 0xfb, 0xe1, 0x59, 0x0a, 0xd7 } },
};
const unsigned int upd_key_count = 1;

#else

/* rel-2026: made 2026-11-19 for 1.0.0-rc5. */
const UpdKey upd_keys[] = {
    { "rel-2026",
      { 0x8e, 0x52, 0x3c, 0x5d, 0x33, 0xb7, 0x91, 0x1c,
        0x9f, 0xb3, 0xcd, 0x95, 0x8e, 0xf2, 0x3f, 0x64,
        0xc6, 0xba, 0x60, 0x31, 0x1e, 0x40, 0x42, 0xe0,
        0xc1, 0x5e, 0xe9, 0x2c, 0x47, 0x63, 0xf4, 0x48 } },
};
const unsigned int upd_key_count = 1;

#endif
