/*
 * Wireless.prefs is WirelessManager's file and the user's.  An unchanged save
 * must give back the same bytes, and an edit must touch the lines it names
 * and nothing else.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netwireless_prefs.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void check(int yes, const char *what)
{
    if (!yes) { fprintf(stderr, "FAIL: %s\n", what); failures++; }
}

/* The installer's file, a hand-written network, a hex SSID, a '#' inside a
   quoted SSID, CRLF lines and a missing final newline. */
static const char sample[] =
    "# ENVARC:Sys/Wireless.prefs -- written by the AmiNetXDuo installer.\n"
    "ctrl_interface=RAM:\n"
    "\n"
    "network={\n"
    "\tssid=\"Home\"\n"
    "\tpsk=\"correct horse\"\n"
    "\t# a comment kept\n"
    "\tpriority=2\n"
    "}\n"
    "# between blocks\n"
    "network={\r\n"
    "    ssid=\"Caf#e\"   # trailing comment\r\n"
    "    key_mgmt=NONE\r\n"
    "    scan_ssid=1\r\n"
    "}\r\n"
    "network={\n"
    "  ssid=4142ff\n"
    "  psk=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n"
    "  disabled=1\n"
    "  pairwise=CCMP\n"
    "}";

static int contains(const char *hay, size_t len, const char *needle)
{
    size_t n = strlen(needle), i;
    for (i = 0; i + n <= len; i++)
        if (memcmp(hay + i, needle, n) == 0) return 1;
    return 0;
}

static void test_parse(void)
{
    NwNetwork n[8];
    int c = nw_prefs_parse(sample, sizeof(sample) - 1, n, 8);

    check(c == 3, "three blocks");
    check(n[0].ssid_len == 4 && memcmp(n[0].ssid, "Home", 4) == 0, "quoted ssid");
    check(n[0].priority == 2 && n[0].has_psk && !n[0].open, "priority and psk");
    check(n[1].ssid_len == 5 && memcmp(n[1].ssid, "Caf#e", 5) == 0,
          "'#' inside the quotes is text, after them a comment");
    check(n[1].open && n[1].scan_ssid && n[1].priority == 0, "open hidden");
    check(n[2].ssid_len == 3 && n[2].ssid[0] == 'A' && n[2].ssid[2] == 0xff,
          "hex ssid");
    check(n[2].disabled && n[2].has_psk, "disabled block");
    check(n[2].end == sizeof(sample) - 1, "last block ends at end of text");
    check(memcmp(sample + n[1].start, "network={\r\n", 11) == 0, "block start");

    {
        static const char open_block[] = "network={\n ssid=\"x\"\n";
        check(nw_prefs_parse(open_block, sizeof(open_block) - 1, n, 8) == -1,
              "unclosed block refused");
    }
    {
        static const char odd[] = "network={\nssid=\"a\nssid=abc\n}\n";
        check(nw_prefs_parse(odd, sizeof(odd) - 1, n, 8) == 1 &&
              n[0].ssid_len == -1, "unusable ssid lines read as none");
    }
    {
        static const char wep[] =
            "network={\nssid=\"w\"\nkey_mgmt=NONE\nwep_key0=\"abcde\"\n}\n";
        check(nw_prefs_parse(wep, sizeof(wep) - 1, n, 8) == 1 &&
              !n[0].open && n[0].wep, "key_mgmt=NONE with a WEP key is not open");
    }
    {
        static const char lead[] = "network={\nssid=\"p\"\npriority= +3\n}\n";
        check(nw_prefs_parse(lead, sizeof(lead) - 1, n, 8) == 1 &&
              n[0].priority == 3, "priority read as atoi() reads it");
    }
    {
        static const char none[] = "# nothing\nctrl_interface=RAM:\n";
        check(nw_prefs_parse(none, sizeof(none) - 1, n, 8) == 0, "no blocks");
        check(nw_prefs_parse("", 0, n, 8) == 0, "empty file");
    }
}

static void test_round_trip(void)
{
    NwNetwork n[8];
    char      out[2048];
    size_t    len = 0;
    int       c = nw_prefs_parse(sample, sizeof(sample) - 1, n, 8);

    check(nw_prefs_write(sample, sizeof(sample) - 1, n, c, NULL, 0,
                         out, sizeof(out), &len), "unchanged write fits");
    check(len == sizeof(sample) - 1 && memcmp(out, sample, len) == 0,
          "unchanged write is byte for byte");

    {
        /* An edit that sets a key to the value it already has. */
        NwField     f[1] = { { "priority", "2", 0 } };
        NwBlockEdit e[1] = { { 0, 0, f, 1 } };

        check(nw_prefs_write(sample, sizeof(sample) - 1, n, c, e, 1,
                             out, sizeof(out), &len), "same-value write fits");
        check(len == sizeof(sample) - 1 && memcmp(out, sample, len) == 0,
              "same value rewrites the same bytes");
    }
    check(!nw_prefs_write(sample, sizeof(sample) - 1, n, c, NULL, 0,
                          out, 10, &len), "short buffer refused");

    {
        /* More blocks than stored: the count says so, the blocks not stored
           are copied, and an edit of one is refused rather than dropped. */
        NwNetwork   two[2];
        NwField     f[1] = { { "priority", "9", 0 } };
        NwBlockEdit e[1] = { { 2, 0, f, 1 } };
        int         k = nw_prefs_parse(sample, sizeof(sample) - 1, two, 2);

        check(k == 3, "the count is the file's");
        check(nw_prefs_write(sample, sizeof(sample) - 1, two, 2, NULL, 0,
                             out, sizeof(out), &len) &&
              len == sizeof(sample) - 1 && memcmp(out, sample, len) == 0,
              "blocks not stored are copied");
        check(!nw_prefs_write(sample, sizeof(sample) - 1, two, 2, e, 1,
                              out, sizeof(out), &len),
              "an edit of a block not stored is refused");
    }
}

static void test_edits(void)
{
    NwNetwork n[8];
    char      out[2048];
    size_t    len = 0;
    int       c = nw_prefs_parse(sample, sizeof(sample) - 1, n, 8);
    char      hex[] = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";

    {
        /* Block 0 gets a derived key and a lower priority; block 1 goes;
           block 2 is untouched; a new block is added. */
        NwField f0[3] = { { "psk", hex, 0 }, { "key_mgmt", "WPA-PSK", 0 },
                          { "priority", "1", 0 } };
        NwField fn[4] = { { "ssid", "\"New\"", 0 }, { "psk", hex, 0 },
                          { "key_mgmt", "WPA-PSK", 0 }, { "priority", "3", 0 } };
        NwBlockEdit e[3] = { { 0, 0, f0, 3 }, { 1, 1, NULL, 0 },
                             { -1, 0, fn, 4 } };
        static const char want[] =
            "# ENVARC:Sys/Wireless.prefs -- written by the AmiNetXDuo installer.\n"
            "ctrl_interface=RAM:\n"
            "\n"
            "network={\n"
            "\tssid=\"Home\"\n"
            "\tpsk=fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210\n"
            "\t# a comment kept\n"
            "\tpriority=1\n"
            "\tkey_mgmt=WPA-PSK\n"
            "}\n"
            "# between blocks\n"
            "network={\n"
            "  ssid=4142ff\n"
            "  psk=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n"
            "  disabled=1\n"
            "  pairwise=CCMP\n"
            "}\n"
            "network={\n"
            "\tssid=\"New\"\n"
            "\tpsk=fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210\n"
            "\tkey_mgmt=WPA-PSK\n"
            "\tpriority=3\n"
            "}\n";

        check(nw_prefs_write(sample, sizeof(sample) - 1, n, c, e, 3,
                             out, sizeof(out), &len), "edit fits");
        check(len == sizeof(want) - 1 && memcmp(out, want, len) == 0,
              "edit touches only the named lines");
        if (len != sizeof(want) - 1 || memcmp(out, want, len) != 0)
            fprintf(stderr, "got:\n%.*s\n", (int)len, out);
    }
    {
        /* CRLF block: the line ending and indentation of the line replaced
           are kept; an open network turned WPA loses key_mgmt=NONE's line
           and keeps the order of everything else. */
        NwField f[3] = { { "key_mgmt", NULL, 0 }, { "psk", hex, 0 },
                         { "scan_ssid", NULL, 0 } };
        NwBlockEdit e[1] = { { 1, 0, f, 3 } };

        check(nw_prefs_write(sample, sizeof(sample) - 1, n, c, e, 1,
                             out, sizeof(out), &len), "crlf edit fits");
        check(contains(out, len,
                       "network={\r\n"
                       "    ssid=\"Caf#e\"   # trailing comment\r\n"
                       "    psk=fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210\r\n"
                       "}\r\n"), "crlf block patched in its own indentation and line ending");
        check(!contains(out, len, "scan_ssid") && !contains(out, len, "NONE"),
              "removed keys gone");
    }
    {
        /* Duplicate keys: the first is replaced, the later ones dropped
           (wpa_supplicant would otherwise take the last). */
        static const char dup[] =
            "network={\n ssid=\"d\"\n priority=1\n priority=7\n}\n";
        static const char want[] =
            "network={\n ssid=\"d\"\n priority=4\n}\n";
        NwNetwork   m[2];
        NwField     f[1] = { { "priority", "4", 0 } };
        NwBlockEdit e[1] = { { 0, 0, f, 1 } };
        int         k = nw_prefs_parse(dup, sizeof(dup) - 1, m, 2);

        check(k == 1 && m[0].priority == 7, "the last duplicate is the one read");
        check(nw_prefs_write(dup, sizeof(dup) - 1, m, k, e, 1,
                             out, sizeof(out), &len) &&
              len == sizeof(want) - 1 && memcmp(out, want, len) == 0,
              "duplicates collapse to one line");
    }
    {
        /* A new block into an empty file and after text with no newline. */
        NwField     f[2] = { { "ssid", "\"a\"", 0 }, { "key_mgmt", "NONE", 0 } };
        NwBlockEdit e[1] = { { -1, 0, f, 2 } };
        static const char tail[] = "ctrl_interface=RAM:";

        static const char fresh[] = "network={\n\tssid=\"a\"\n\tkey_mgmt=NONE\n}\n";

        check(nw_prefs_write("", 0, NULL, 0, e, 1, out, sizeof(out), &len) &&
              len == sizeof(fresh) - 1 && memcmp(out, fresh, len) == 0,
              "new block into an empty file");
        check(nw_prefs_write(tail, sizeof(tail) - 1, NULL, 0, e, 1,
                             out, sizeof(out), &len) &&
              len == sizeof(tail) + sizeof(fresh) - 1 &&
              memcmp(out, "ctrl_interface=RAM:\n", 20) == 0 &&
              memcmp(out + 20, fresh, sizeof(fresh) - 1) == 0,
              "a newline goes before a new block when the text lacks one");
    }
    {
        /* Removing every block leaves the rest of the file. */
        NwBlockEdit e[3] = { { 0, 1, NULL, 0 }, { 1, 1, NULL, 0 },
                             { 2, 1, NULL, 0 } };
        static const char want[] =
            "# ENVARC:Sys/Wireless.prefs -- written by the AmiNetXDuo installer.\n"
            "ctrl_interface=RAM:\n"
            "\n"
            "# between blocks\n";

        check(nw_prefs_write(sample, sizeof(sample) - 1, n, c, e, 3,
                             out, sizeof(out), &len) &&
              len == sizeof(want) - 1 && memcmp(out, want, len) == 0,
              "all blocks removed, comments kept");
    }
}

static void test_ssid_value(void)
{
    char v[2 * NW_SSID_MAX + 3];
    static const unsigned char odd[3] = { 'A', 0x00, 0xe9 };
    NwNetwork n[1];
    char      text[128];

    nw_prefs_ssid_value((const unsigned char *)"My \"Net\"", 8, v);
    check(strcmp(v, "\"My \"Net\"\"") == 0, "printable ssid quoted as is");
    nw_prefs_ssid_value(odd, 3, v);
    check(strcmp(v, "4100e9") == 0, "other bytes as hex");

    snprintf(text, sizeof(text), "network={\nssid=\"My \"Net\"\"\n}\n");
    check(nw_prefs_parse(text, strlen(text), n, 1) == 1 &&
          n[0].ssid_len == 8 && memcmp(n[0].ssid, "My \"Net\"", 8) == 0,
          "quoted value reads back with its inner quotes");
    snprintf(text, sizeof(text), "network={\nssid=4100e9\n}\n");
    check(nw_prefs_parse(text, strlen(text), n, 1) == 1 &&
          n[0].ssid_len == 3 && memcmp(n[0].ssid, odd, 3) == 0,
          "hex value reads back");
}

static void test_iface(void)
{
    char          dev[64];
    unsigned long unit = 9;
    static const char a[] =
        "# Pi Wi-Fi\n"
        "; device=wrong.device\n"
        "DEVICE=DEVS:Networks/anxwifipi.device\n"
        "Unit = 1 ; second\n"
        "configure=dhcp\n";
    static const char b[] =
        "  device \"Work:My Devs/anxwifipi.device\"\r\n"
        "unit\t0\r\n";
    static const char c[] = "devices=x\nunit=3\n";

    check(nw_iface_device(a, sizeof(a) - 1, dev, sizeof(dev), &unit) &&
          strcmp(dev, "DEVS:Networks/anxwifipi.device") == 0 && unit == 1,
          "keyword=value, comments, spaces");
    unit = 9;
    check(nw_iface_device(b, sizeof(b) - 1, dev, sizeof(dev), &unit) &&
          strcmp(dev, "Work:My Devs/anxwifipi.device") == 0 && unit == 0,
          "keyword value, quotes, CRLF");
    unit = 9;
    check(!nw_iface_device(c, sizeof(c) - 1, dev, sizeof(dev), &unit) &&
          unit == 3, "a longer keyword is not DEVICE");
    check(!nw_iface_device(a, sizeof(a) - 1, dev, 8, &unit),
          "a DEVICE too long for the buffer is not taken");
    {
        /* Two DEVICE lines that each fit: the last wins, written from the
           start, never after the first (which ran past the buffer). */
        static const char two[] =
            "DEVICE=abcdefghijklmn.device\n"
            "DEVICE=short.device\n";
        struct { char buf[24]; char canary[16]; } guard;

        memset(&guard, 'x', sizeof(guard));
        check(nw_iface_device(two, sizeof(two) - 1, guard.buf, sizeof(guard.buf), &unit) &&
              strcmp(guard.buf, "short.device") == 0, "the last DEVICE line wins");
        check(guard.canary[0] == 'x' && guard.canary[15] == 'x',
              "nothing written past the buffer");
    }
}

int main(void)
{
    test_parse();
    test_round_trip();
    test_edits();
    test_ssid_value();
    test_iface();
    check(nw_wireless_driver("DEVS:Networks/anxwifipi.device") == NW_DRIVER_PI &&
          nw_wireless_driver("wifipi.device") == NW_DRIVER_PI &&
          nw_wireless_driver("prism2.device") == NW_DRIVER_PRISM2 &&
          nw_wireless_driver("Work:Drivers/PRISM2.DEVICE") == NW_DRIVER_PRISM2 &&
          !nw_wireless_driver("anxnet.device") &&
          !nw_wireless_driver("prism2.device.old") &&
          !nw_wireless_driver(""), "Wi-Fi drivers known by name");
    if (failures == 0) printf("netwireless prefs: all passed\n");
    return failures == 0 ? 0 : 1;
}
