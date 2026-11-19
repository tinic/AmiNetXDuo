/*
 * The tests for src/tools/updcheck.c: the settings file, the flavour from
 * the IdString, the mirror URLs, the date and the clock floor, the CHECK
 * verdict over signed manifests, the log's whole lines and the stack by
 * the word.  The signatures are openssl's, made with the
 * TEST key tests/updater/test-only-update-key-a.pem (see test_updsig.c):
 *
 *   openssl pkeyutl -sign -rawin \
 *       -inkey tests/updater/test-only-update-key-a.pem -in m -out s
 *
 * SPDX-License-Identifier: MIT
 */

#include "updcheck.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static int failures;
static int checks;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                    \
    } while (0)

/* ------------------------------------------------------------ settings --- */

static UpdSettings parse(const char *text)
{
    UpdSettings s;

    memset(&s, 0x5a, sizeof(s));
    upd_settings_parse(text, (unsigned long)strlen(text), &s);
    return s;
}

static void test_settings(void)
{
    UpdSettings s;

    s = parse("");
    CHECK(s.auto_check == 1 && s.channel == -1 && s.mirror[0] == '\0' &&
          s.bad == 0);

    upd_settings_parse(0, 0, &s);
    CHECK(s.auto_check == 1 && s.channel == -1 && s.mirror[0] == '\0');

    s = parse("AUTO=OFF\nCHANNEL=RELEASE\nMIRROR=http://h:8480/t/x/good/\n");
    CHECK(s.auto_check == 0);
    CHECK(s.channel == UPD_CHANNEL_RELEASE);
    CHECK(strcmp(s.mirror, "http://h:8480/t/x/good/") == 0);
    CHECK(s.bad == 0);

    /* Case, CR LF, blanks around = and comments. */
    s = parse("; prefs\r\n# more\r\n auto = on \r\nchannel=PreRelease\r\n");
    CHECK(s.auto_check == 1 && s.channel == UPD_CHANNEL_PRERELEASE &&
          s.bad == 0);

    /* No final LF on the last line. */
    s = parse("AUTO=OFF");
    CHECK(s.auto_check == 0);

    /* The "/" a MIRROR needs. */
    s = parse("MIRROR=http://h/upd");
    CHECK(strcmp(s.mirror, "http://h/upd/") == 0);

    /* Unknown keys are ignored; bad words count and keep the default. */
    s = parse("COLOUR=red\nAUTO=maybe\nCHANNEL=beta\nnoequals\n");
    CHECK(s.auto_check == 1 && s.channel == -1 && s.bad == 3);

    /* A MIRROR with a blank or a control byte, or too long, is refused. */
    s = parse("MIRROR=http://h/a b/\n");
    CHECK(s.mirror[0] == '\0' && s.bad == 1);
    s = parse("MIRROR=\n");
    CHECK(s.mirror[0] == '\0' && s.bad == 1);
    {
        char long_text[UPD_MIRROR_MAX + 32];

        strcpy(long_text, "MIRROR=http://");
        memset(long_text + 14, 'a', UPD_MIRROR_MAX);
        long_text[14 + UPD_MIRROR_MAX] = '\0';
        s = parse(long_text);
        CHECK(s.mirror[0] == '\0' && s.bad == 1);

        /* The longest that fits, with the "/" added. */
        strcpy(long_text, "MIRROR=");
        memset(long_text + 7, 'a', UPD_MIRROR_MAX - 2);
        long_text[7 + UPD_MIRROR_MAX - 2] = '\0';
        s = parse(long_text);
        CHECK(strlen(s.mirror) == UPD_MIRROR_MAX - 1 &&
              s.mirror[UPD_MIRROR_MAX - 2] == '/');
    }

    /* The last of a repeated key wins. */
    s = parse("AUTO=OFF\nAUTO=ON\n");
    CHECK(s.auto_check == 1);
}

/* ------------------------------------------------------------- flavour --- */

#define ID(p) "bsdsocket.library 1.0.0-rc5 (AmiNetXDuo, ABI 4.12, build " \
              "abc1234, profile " p ")\r\n"

static void test_flavour(void)
{
    CHECK(upd_flavour_from_id(ID("default")) == UPD_FLAVOUR_FULL);
    CHECK(upd_flavour_from_id(ID("minimal")) == UPD_FLAVOUR_MINIMAL);
    CHECK(upd_flavour_from_id(ID("micro")) == UPD_FLAVOUR_MICRO);
    CHECK(upd_flavour_from_id(ID("fat")) == UPD_FLAVOUR_FAT);
    CHECK(upd_flavour_from_id(ID("full")) == -1);
    CHECK(upd_flavour_from_id(ID("microx")) == -1);
    CHECK(upd_flavour_from_id(ID("")) == -1);
    CHECK(upd_flavour_from_id("bsdsocket.library 4.364 (Roadshow)\r\n") == -1);
    CHECK(upd_flavour_from_id("") == -1);
    CHECK(upd_flavour_from_id(0) == -1);
    /* "profile" inside another word is not the field. */
    CHECK(upd_flavour_from_id("(xprofile fat)") == -1);
}

/* ---------------------------------------------------------------- urls --- */

static void test_url(void)
{
    char out[64];

    CHECK(upd_url(out, sizeof(out), "http://h/m/", UPD_CHANNEL_PRERELEASE,
                  "update.manifest") == 1);
    CHECK(strcmp(out, "http://h/m/prerelease/update.manifest") == 0);
    CHECK(upd_url(out, sizeof(out), "http://h/", UPD_CHANNEL_RELEASE,
                  "update.manifest.sig") == 1);
    CHECK(strcmp(out, "http://h/release/update.manifest.sig") == 0);

    /* Exactly full, and one byte short. */
    CHECK(upd_url(out, 22, "http://h/", UPD_CHANNEL_RELEASE, "abcd") == 1);
    CHECK(strlen(out) == 21);
    CHECK(upd_url(out, 21, "http://h/", UPD_CHANNEL_RELEASE, "abcd") == 0);

    CHECK(upd_url(out, sizeof(out), "", UPD_CHANNEL_RELEASE, "x") == 0);
    CHECK(upd_url(out, sizeof(out), "http://h/", UPD_CHANNEL_COUNT, "x") == 0);
    CHECK(upd_url(out, 0, "http://h/", UPD_CHANNEL_RELEASE, "x") == 0);
}

/* ---------------------------------------------------------------- date --- */

static void test_date(void)
{
    char          out[11];
    unsigned long d;

    upd_date(0, out);
    CHECK(strcmp(out, "1978-01-01") == 0);
    upd_date(UPD_CLOCK_FLOOR_DAYS, out);
    CHECK(strcmp(out, "2026-01-01") == 0);
    upd_date(UPD_CLOCK_FLOOR_DAYS - 1, out);
    CHECK(strcmp(out, "2025-12-31") == 0);

    /* TLS_CLOCK_FLOOR, 1767225600, in DateStamp days. */
    CHECK((1767225600UL - 252460800UL) / 86400UL == UPD_CLOCK_FLOOR_DAYS);

    /* Every day to 2100 against the C library. */
    for (d = 0; d < 44000UL; d++)
    {
        time_t    t = (time_t)(252460800L + (long)d * 86400L);
        struct tm tm;
        char      want[40];

        gmtime_r(&t, &tm);
        snprintf(want, sizeof(want), "%04d-%02d-%02d", tm.tm_year + 1900,
                 tm.tm_mon + 1, tm.tm_mday);
        upd_date(d, out);
        if (strcmp(out, want) != 0)
        {
            CHECK(strcmp(out, want) == 0);
            break;
        }
    }
    checks++;
}

/* ------------------------------------------------------------- verdict --- */

#define MANIFEST                                                             \
    "format=1\n"                                                             \
    "product=AmiNetXDuo\n"                                                   \
    "version=1.0.0-rc5\n"                                                    \
    "channel=prerelease\n"                                                   \
    "container=full 5 "                                                      \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef "      \
    "AmiNetXDuo-1.0.0-rc5-full.update.lha\n"                                 \
    "end=5\n"

/* MANIFEST without end=, as the truncated mirror tree serves it. */
#define TRUNCATED                                                            \
    "format=1\n"                                                             \
    "product=AmiNetXDuo\n"                                                   \
    "version=1.0.0-rc5\n"                                                    \
    "channel=prerelease\n"

#define PK_A "1d818ade40eee9daac034dd166eebdabc0cd6116eb468b1846b044fbe1590ad7"
#define PK_B "1a9e8c5db8aebe8a014ff615e1168cb415aef029707317235dd5546b3ddd2fff"

#define SIG_A                                                                \
    "c3b729d6f8a76e80a044ad02b14cfa4a5646abba19a1e11e3d1bb01daa6d6c08"       \
    "cf187bd6c9ffa6692078144b7f52fd64c92c028a4eb59963e147e43ad49cb601"
#define SIG_A_BAD                                                            \
    "c2b729d6f8a76e80a044ad02b14cfa4a5646abba19a1e11e3d1bb01daa6d6c08"       \
    "cf187bd6c9ffa6692078144b7f52fd64c92c028a4eb59963e147e43ad49cb601"
#define SIG_A_TRUNCATED                                                      \
    "0fcea2d2c95c5d2cbd11dbfa0f01bb36a00e4ee2098b9311b7471fba40ae8e6d"       \
    "bd8d773e7fb0ea2279daf37cf9d78a21e124c0373110d548e3e13e522eb1fb05"

static void unhex(unsigned char *out, const char *s)
{
    size_t i;

    for (i = 0; i < strlen(s) / 2; i++)
    {
        unsigned v = 0;

        sscanf(s + 2 * i, "%2x", &v);
        out[i] = (unsigned char)v;
    }
}

static UpdKey keys[2];

static UpdVerdict verdict(const char *m, const char *sig, unsigned int k0,
                          unsigned int nkeys, const char *installed,
                          unsigned int channel, unsigned int flavour,
                          UpdResult *why, UpdManifest *out)
{
    UpdWant     want;
    UpdManifest m_;
    UpdFile     files[4];

    memset(&want, 0, sizeof(want));
    upd_ver_parse(installed, (unsigned long)strlen(installed), &want.installed);
    want.channel = (unsigned char)channel;
    want.flavour = (unsigned char)flavour;

    return upd_check_verdict((const unsigned char *)m,
                             (unsigned long)strlen(m), sig,
                             (unsigned long)strlen(sig), keys + k0, nkeys,
                             &want, out ? out : &m_, files, 4, why);
}

static void test_verdict(void)
{
    UpdResult   why;
    UpdManifest m;
    char        v[UPD_VER_MAX];

    keys[0].id = "test-a";
    unhex(keys[0].pk, PK_A);
    keys[1].id = "test-b";
    unhex(keys[1].pk, PK_B);

    /* S31 / the good tree: newer, signed, the installed flavour. */
    CHECK(verdict(MANIFEST, "key=test-a sig=" SIG_A "\n", 0, 1, "1.0.0-rc4",
                  UPD_CHANNEL_PRERELEASE, UPD_FLAVOUR_FULL, &why, &m) ==
          UPD_V_AVAILABLE);
    CHECK(why == UPD_OK);
    upd_ver_format(&m.version, v);
    CHECK(strcmp(v, "1.0.0-rc5") == 0);
    CHECK(m.container_size == 5);

    /* Two keys held, the second one signs. */
    CHECK(verdict(MANIFEST, "key=test-b sig=" SIG_A_BAD "\n"
                            "key=test-a sig=" SIG_A "\n", 0, 2, "1.0.0-rc4",
                  UPD_CHANNEL_PRERELEASE, UPD_FLAVOUR_FULL, &why, 0) ==
          UPD_V_AVAILABLE);

    /* S03: the installed version, and an older one. */
    CHECK(verdict(MANIFEST, "key=test-a sig=" SIG_A "\n", 0, 1, "1.0.0-rc5",
                  UPD_CHANNEL_PRERELEASE, UPD_FLAVOUR_FULL, &why, 0) ==
          UPD_V_CURRENT);
    CHECK(why == UPD_E_NOT_NEWER);
    CHECK(verdict(MANIFEST, "key=test-a sig=" SIG_A "\n", 0, 1, "1.0.0",
                  UPD_CHANNEL_PRERELEASE, UPD_FLAVOUR_FULL, &why, 0) ==
          UPD_V_CURRENT);

    /* S30-badsig: a known key that does not verify. */
    CHECK(verdict(MANIFEST, "key=test-a sig=" SIG_A_BAD "\n", 0, 1,
                  "1.0.0-rc4", UPD_CHANNEL_PRERELEASE, UPD_FLAVOUR_FULL,
                  &why, 0) == UPD_V_SIGNATURE);
    CHECK(why == UPD_E_SIG_BAD);

    /* One byte of the manifest changed. */
    {
        char other[sizeof(MANIFEST)];

        strcpy(other, MANIFEST);
        other[strlen(other) - 3] = '6';
        CHECK(verdict(other, "key=test-a sig=" SIG_A "\n", 0, 1, "1.0.0-rc4",
                      UPD_CHANNEL_PRERELEASE, UPD_FLAVOUR_FULL, &why, 0) ==
              UPD_V_SIGNATURE);
    }

    /* S30-unknownkey: signed by a key this build does not hold. */
    CHECK(verdict(MANIFEST, "key=test-x sig=" SIG_A "\n", 0, 1, "1.0.0-rc4",
                  UPD_CHANNEL_PRERELEASE, UPD_FLAVOUR_FULL, &why, 0) ==
          UPD_V_UNKNOWN_KEY);
    CHECK(why == UPD_E_SIG_UNKNOWN_KEY);
    /* The right signature under a name the build holds for another key. */
    CHECK(verdict(MANIFEST, "key=test-b sig=" SIG_A "\n", 1, 1, "1.0.0-rc4",
                  UPD_CHANNEL_PRERELEASE, UPD_FLAVOUR_FULL, &why, 0) ==
          UPD_V_SIGNATURE);

    /* A signature file that does not parse, or is empty. */
    CHECK(verdict(MANIFEST, "key=test-a sig=12\n", 0, 1, "1.0.0-rc4",
                  UPD_CHANNEL_PRERELEASE, UPD_FLAVOUR_FULL, &why, 0) ==
          UPD_V_SIGNATURE);
    CHECK(verdict(MANIFEST, "", 0, 1, "1.0.0-rc4",
                  UPD_CHANNEL_PRERELEASE, UPD_FLAVOUR_FULL, &why, 0) ==
          UPD_V_SIGNATURE);

    /* No key at all: never accepted. */
    CHECK(verdict(MANIFEST, "key=test-a sig=" SIG_A "\n", 0, 0, "1.0.0-rc4",
                  UPD_CHANNEL_PRERELEASE, UPD_FLAVOUR_FULL, &why, 0) ==
          UPD_V_SIGNATURE);

    /* S30-truncated: signed as it is, and still refused. */
    CHECK(verdict(TRUNCATED, "key=test-a sig=" SIG_A_TRUNCATED "\n", 0, 1,
                  "1.0.0-rc4", UPD_CHANNEL_PRERELEASE, UPD_FLAVOUR_FULL,
                  &why, 0) == UPD_V_MANIFEST);
    CHECK(why == UPD_E_TRUNCATED);

    /* Another channel's manifest, and a flavour it does not offer. */
    CHECK(verdict(MANIFEST, "key=test-a sig=" SIG_A "\n", 0, 1, "1.0.0-rc4",
                  UPD_CHANNEL_RELEASE, UPD_FLAVOUR_FULL, &why, 0) ==
          UPD_V_MANIFEST);
    CHECK(why == UPD_E_CHANNEL);
    CHECK(verdict(MANIFEST, "key=test-a sig=" SIG_A "\n", 0, 1, "1.0.0-rc4",
                  UPD_CHANNEL_PRERELEASE, UPD_FLAVOUR_MICRO, &why, 0) ==
          UPD_V_MANIFEST);
    CHECK(why == UPD_E_NO_FLAVOUR);
}

/*
 * The log keeps whole key=value lines: one that does not fit ends it with
 * truncated=yes, and nothing after (#497 review: a line was cut in the
 * middle when the 2048 bytes filled).
 */
static void test_said(void)
{
    static char buf[64];
    unsigned long len = 0, n;
    const unsigned long t = sizeof(UPD_SAID_TRUNCATED) - 1;

    len = upd_said_add(buf, sizeof(buf), len, "update=available version=", "1.0.1", 0);
    CHECK(len == 31 && memcmp(buf, "update=available version=1.0.1\n", 31) == 0);
    len = upd_said_add(buf, sizeof(buf), len, "staging=removed", 0, 0);
    CHECK(len == 47);
    /* 17 bytes of 64 left: this one of 16 and the marker do not fit. */
    n = upd_said_add(buf, sizeof(buf), len, "refused=", "network", 0);
    CHECK(n == len + t && memcmp(buf + len, UPD_SAID_TRUNCATED, t) == 0);
    len = n;
    /* Ended: a short line after it is not added. */
    CHECK(upd_said_add(buf, sizeof(buf), len, "a=b", 0, 0) == len);
    /* Every line in it is whole. */
    CHECK(buf[len - 1] == '\n');
    {
        unsigned long i, start = 0;

        for (i = 0; i < len; i++)
            if (buf[i] == '\n')
            {
                CHECK(memchr(buf + start, '=', i - start) != 0);
                start = i + 1;
            }
    }
    /* Exactly full: a line with the marker's room after it fits; the
       next one gets the marker in the last 14 bytes. */
    len = upd_said_add(buf, sizeof(buf), 0, "abcdefghijklmnopqrstuvwxyz0123456789=abcdefghij", 0, 0);
    CHECK(len == 48);
    len = upd_said_add(buf, sizeof(buf), len, "x", 0, 0);
    CHECK(len == 50);
    CHECK(upd_said_add(buf, sizeof(buf), len, "y", 0, 0) == 50 + t &&
          memcmp(buf + 50, UPD_SAID_TRUNCATED, t) == 0);
    /* No room even for the marker: nothing written. */
    CHECK(upd_said_add(buf, 10, 0, "abcdefghijklmnop=x", 0, 0) == 0);
}

/* 64 KB for every word that fetches: an http: mirror can redirect to
   https:, and tls.library wants fetch's stack (#497 review). */
static void test_stack(void)
{
    CHECK(upd_stack_size("CHECK") == 65536);
    CHECK(upd_stack_size("INSTALL") == 65536);
    CHECK(upd_stack_size("BOOT") == 16384);
    CHECK(upd_stack_size("CONFIRM") == 16384);
}

/* The status first, for the manifest and its .sig as for the container:
   a 404 whose body overran the buffer is a 404 (#497 review). */
static void test_fetch_result(void)
{
    CHECK(upd_fetch_result(404, 0) == 404);
    CHECK(upd_fetch_result(404, 1) == 404);
    CHECK(upd_fetch_result(500, 0) == 500);
    CHECK(upd_fetch_result(200, 1) == 0);
    CHECK(upd_fetch_result(200, 0) == -1);
    CHECK(upd_fetch_result(0, 0) == -1);
    CHECK(upd_fetch_result(0, 1) == -1);
}

int main(void)
{
    test_settings();
    test_flavour();
    test_url();
    test_date();
    test_verdict();
    test_said();
    test_stack();
    test_fetch_result();

    printf("test_updcheck: %d checks, %d failures\n", checks, failures);

    return failures != 0;
}
