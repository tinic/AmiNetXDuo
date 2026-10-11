/*
 * The WPA key against IEEE 802.11i-2004 Annex H.4's three test cases.  A wrong
 * key is 64 hex digits that look exactly like a right one until the access
 * point refuses the handshake.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netwireless_key.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void check(int yes, const char *what)
{
    if (!yes) { fprintf(stderr, "FAIL: %s\n", what); failures++; }
}

static void vector(const char *pass, const char *ssid, const char *want)
{
    unsigned char key[NW_KEY_LEN];
    char          hex[2 * NW_KEY_LEN + 1];

    check(nw_key_derive(pass, strlen(pass), (const unsigned char *)ssid,
                        strlen(ssid), key, NULL, NULL), "derive runs");
    nw_key_hex(key, hex);
    check(strcmp(hex, want) == 0, want);
    if (strcmp(hex, want) != 0) fprintf(stderr, "  got %s\n", hex);
}

static unsigned long calls, last;

static int count_progress(void *ctx, unsigned long done)
{
    (void)ctx;
    calls++;
    last = done;
    return 1;
}

static int stop_progress(void *ctx, unsigned long done)
{
    (void)ctx;
    return done < 1024;
}

int main(void)
{
    unsigned char key[NW_KEY_LEN];

    /* H.4.1 test cases 1-3. */
    vector("password", "IEEE",
           "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e");
    vector("ThisIsAPassword", "ThisIsASSID",
           "0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af");
    vector("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ",
           "becb93866bb8c3832cb777c2f559807c8c59afcb6eae734885001300a981cc62");

    /* The longest passphrase and SSID wpa_supplicant accepts. */
    vector("123456789012345678901234567890123456789012345678901234567890123",
           "12345678901234567890123456789012",
           "aa00f993c2eb6ee2ef3b8db3a1c7668631eead09e2187d0575e969d9045aab45");

    check(nw_key_derive("password", 8, (const unsigned char *)"IEEE", 4, key,
                        count_progress, NULL), "progress run");
    check(calls == NW_KEY_STEPS / 256 && last == NW_KEY_STEPS,
          "progress every 256 steps");
    check(!nw_key_derive("password", 8, (const unsigned char *)"IEEE", 4, key,
                         stop_progress, NULL), "progress cancels");
    check(!nw_key_derive("", 0, (const unsigned char *)"IEEE", 4, key,
                         NULL, NULL), "empty passphrase refused");
    check(!nw_key_derive("password", 8,
                         (const unsigned char *)"123456789012345678901234567890123",
                         33, key, NULL, NULL), "33-byte SSID refused");

    check(nw_key_kind("1234567", 7) == NW_PASS_BAD, "7 characters");
    check(nw_key_kind("12345678", 8) == NW_PASS_TEXT, "8 characters");
    check(nw_key_kind("123456789012345678901234567890123456789012345678901234567890123",
                      63) == NW_PASS_TEXT, "63 characters");
    check(nw_key_kind("0123456789abcdefABCDEF0123456789abcdef0123456789abcdef0123456789",
                      64) == NW_PASS_HEX, "64 hex digits are the key");
    check(nw_key_kind("0123456789abcdefABCDEF0123456789abcdef0123456789abcdef012345678g",
                      64) == NW_PASS_BAD, "64 characters that are not hex");
    check(nw_key_kind("caf\xe9 au lait", 12) == NW_PASS_BAD, "non-ASCII refused");
    check(nw_key_kind("tab\there12", 10) == NW_PASS_BAD, "control character refused");

    if (failures == 0) printf("netwireless key: all passed\n");
    return failures == 0 ? 0 : 1;
}
