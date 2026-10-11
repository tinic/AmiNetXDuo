/*
 * NetWireless: the WPA pre-shared key, PBKDF2-HMAC-SHA1(passphrase, SSID,
 * 4096, 32) as IEEE 802.11i-2004 Annex H.4 defines it, over the SHA-1 already
 * in this tree.  Written to Wireless.prefs as psk=<64 hex>, so the passphrase
 * is never stored and WirelessManager does not derive it at every start.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETWIRELESS_KEY_H
#define AMINETXDUO_NETWIRELESS_KEY_H

#include <stddef.h>

#define NW_KEY_LEN          32
#define NW_KEY_ITERATIONS   4096
#define NW_KEY_STEPS        (2 * NW_KEY_ITERATIONS)

enum { NW_PASS_BAD, NW_PASS_TEXT, NW_PASS_HEX };

/* NW_PASS_TEXT for 8 to 63 printable ASCII characters, NW_PASS_HEX for 64
   hex digits (the key itself), NW_PASS_BAD otherwise. */
int nw_key_kind(const char *pass, size_t len);

/* Called every 256 steps of NW_KEY_STEPS; returning 0 cancels. */
typedef int (*NwKeyProgress)(void *ctx, unsigned long done);

/* The key for a passphrase of NW_PASS_TEXT.  Returns 1, or 0 when the
   progress hook cancelled or an argument is out of range. */
int nw_key_derive(const char *pass, size_t pass_len,
                  const unsigned char *ssid, size_t ssid_len,
                  unsigned char key[NW_KEY_LEN],
                  NwKeyProgress progress, void *ctx);

/* 64 lower-case hex digits and a NUL. */
void nw_key_hex(const unsigned char key[NW_KEY_LEN], char out[2 * NW_KEY_LEN + 1]);

#endif
