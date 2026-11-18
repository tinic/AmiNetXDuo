/*
 * updkeys, the public keys UpdateAmiNetXDuo accepts a manifest signed by.
 *
 * A build with -DAMINETXDUO_UPDATE_TEST_KEY=ON (UPD_TEST_KEY) knows exactly
 * one key, test-a, whose private half is committed as
 * tests/updater/test-only-update-key-a.pem: anyone can sign for it, so such
 * a build is for the scenario tests only and never ships.
 *
 * Every other build knows the release keys below.  THERE ARE NONE YET: the
 * release signing key is made with the release environment (PR 8 of #497).
 * Until a key is listed, upd_key_count is 0 and the updater refuses every
 * manifest before it fetches one.  Up to UPD_KEYS_MAX keys, so that a
 * rotation can ship the next key beside the current one.
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

/* PLACEHOLDER: no release key exists yet.  One entry so that the array is
   not empty; upd_key_count keeps it unused. */
const UpdKey upd_keys[] = {
    { "release-none", { 0 } },
};
const unsigned int upd_key_count = 0;

#endif
