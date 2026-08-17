/*
 * The tests for src/tlslib/tls_certfmt.c, which turns the parts of a
 * certificate into what a TLSA_VerifyHook puts in front of a person.
 *
 *   Everything here is decided about bytes that came off the wire from
 *   somebody who has not been authenticated yet, and every way of getting it
 *   wrong is silent.  A subject line that stops at an embedded NUL shows the
 *   half of a name the certificate chose to show.  A control character goes
 *   into a requester title.  A UTCTime read with the wrong century pivot dates
 *   a certificate a hundred years out, and the person deciding whether to trust
 *   it is looking at the wrong number.  None of that is a handshake failure, a
 *   log line or anything the library notices.
 *
 *   The fingerprint is checked against the published SHA-256 vectors rather
 *   than against itself, and against the same implementation the library links,
 *   so what this blesses is what ships.
 *
 *   cc -std=c11 -Wall -Wextra -Isrc/tlslib -Isrc/crypto68k \
 *      -Itests/crypto68k/host/shim -Ithird_party/netxduo/crypto_libraries/inc \
 *      -DNX_CRYPTO_STANDALONE_ENABLE \
 *      src/tlslib/test/test_tls_certfmt.c src/tlslib/tls_certfmt.c \
 *      src/crypto68k/c68k_sha256.c -o test_tls_certfmt
 *
 * SPDX-License-Identifier: MIT
 */

#include "tls_certfmt.h"

#include "c68k_sha256.h"

#include <stdio.h>
#include <string.h>

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

#define CHECK_STR(got, want)                                                 \
    do {                                                                     \
        checks++;                                                            \
        if (strcmp((got), (want)) != 0) {                                    \
            failures++;                                                      \
            printf("  FAIL %s:%d: \"%s\" != \"%s\"\n",                       \
                   __FILE__, __LINE__, (got), (want));                       \
        }                                                                    \
    } while (0)

/* A literal as a certificate carries it: bytes and a length, no terminator. */
#define BYTES(s)    ((const unsigned char *)(s)), (sizeof(s) - 1)

static TLSCertAttr attr(const char *label, const unsigned char *value,
                        unsigned long length)
{
    TLSCertAttr a;

    a.ca_Label  = label;
    a.ca_Value  = value;
    a.ca_Length = length;

    return a;
}

/* --------------------------------------------------- distinguished names --- */

/*
 * The order is the header's, CN then O then C, and it is the order of the
 * array rather than of the certificate.
 */
static void test_name_order(void)
{
    TLSCertAttr a[3];
    char        buf[128];

    a[0] = attr("CN", BYTES("example.com"));
    a[1] = attr("O",  BYTES("Example Ltd"));
    a[2] = attr("C",  BYTES("GB"));

    CHECK(tls_cert_name_format(buf, sizeof(buf), a, 3) == 35);
    CHECK_STR(buf, "CN=example.com, O=Example Ltd, C=GB");

    /* The common name alone is the same call over one attribute, which is how
       tc_CommonName is produced. */
    CHECK(tls_cert_name_format(buf, sizeof(buf), a, 1) == 14);
    CHECK_STR(buf, "CN=example.com");
}

/*
 * A field the certificate does not carry is skipped, and the separator goes
 * with it: no ", ," and no leading or trailing comma.
 */
static void test_name_missing_fields(void)
{
    TLSCertAttr a[3];
    char        buf[128];

    a[0] = attr("CN", BYTES("example.com"));
    a[1] = attr("O",  NULL, 0);
    a[2] = attr("C",  BYTES("GB"));

    (void)tls_cert_name_format(buf, sizeof(buf), a, 3);
    CHECK_STR(buf, "CN=example.com, C=GB");

    /* A present pointer with a zero length is the same as absent. */
    a[1] = attr("O", (const unsigned char *)"", 0);
    (void)tls_cert_name_format(buf, sizeof(buf), a, 3);
    CHECK_STR(buf, "CN=example.com, C=GB");

    /* Only the last one. */
    a[0] = attr("CN", NULL, 0);
    (void)tls_cert_name_format(buf, sizeof(buf), a, 3);
    CHECK_STR(buf, "C=GB");

    /* None at all is an empty string and a length of zero, which is what makes
       tls_cert.c leave tc_Subject NULL rather than allocate for nothing. */
    a[2] = attr("C", NULL, 0);
    CHECK(tls_cert_name_format(buf, sizeof(buf), a, 3) == 0);
    CHECK_STR(buf, "");

    CHECK(tls_cert_name_format(buf, sizeof(buf), NULL, 3) == 0);
    CHECK(tls_cert_name_format(buf, sizeof(buf), a, 0) == 0);
}

/*
 * The one that matters.  A value comes off the wire from a peer that has not
 * been authenticated, and it goes into a requester title.  An embedded NUL
 * would end the string where the certificate decided, hiding everything after
 * it -- "example.com\0.attacker.example" reading as "example.com" is the whole
 * of the classic attack.  Control characters would move the cursor.
 */
static void test_name_sanitised(void)
{
    TLSCertAttr a[1];
    char        buf[128];

    a[0] = attr("CN", BYTES("example.com\0.attacker.test"));
    (void)tls_cert_name_format(buf, sizeof(buf), a, 1);
    CHECK_STR(buf, "CN=example.com?.attacker.test");

    a[0] = attr("CN", BYTES("a\nb\tc\033d\177e"));
    (void)tls_cert_name_format(buf, sizeof(buf), a, 1);
    CHECK_STR(buf, "CN=a?b?c?d?e");

    /* Anything from a space up to DEL is left as it is, including the eighth
       bit: a UTF-8 name goes through unchanged rather than becoming a row of
       question marks. */
    a[0] = attr("CN", BYTES("caf\303\251.example"));
    (void)tls_cert_name_format(buf, sizeof(buf), a, 1);
    CHECK_STR(buf, "CN=caf\303\251.example");
}

/*
 * The measuring pass and the writing pass run the same code, which is what
 * lets tls_cert.c size one allocation for a whole chain and then fill it.  A
 * disagreement between the two is a truncated string in the good case and an
 * overrun in the bad one.
 */
static void test_name_measure(void)
{
    TLSCertAttr   a[3];
    char          buf[128];
    unsigned long want;

    a[0] = attr("CN", BYTES("example.com"));
    a[1] = attr("O",  BYTES("Example Ltd"));
    a[2] = attr("C",  BYTES("GB"));

    want = tls_cert_name_format(NULL, 0, a, 3);
    CHECK(want == 35);
    CHECK(tls_cert_name_format(buf, want + 1, a, 3) == want);
    CHECK(strlen(buf) == want);
    CHECK_STR(buf, "CN=example.com, O=Example Ltd, C=GB");
}

/*
 * A buffer smaller than the name truncates and still terminates.  This is not
 * a case tls_cert.c can reach, because it allocates what it measured; it is
 * here because the alternative to checking it is finding out on a machine with
 * no memory protection.
 */
static void test_name_truncation(void)
{
    TLSCertAttr a[1];
    char        buf[8];

    memset(buf, 'X', sizeof(buf));

    a[0] = attr("CN", BYTES("example.com"));

    /* The wanted length is reported whatever was written. */
    CHECK(tls_cert_name_format(buf, sizeof(buf), a, 1) == 14);
    CHECK(strlen(buf) == sizeof(buf) - 1);
    CHECK_STR(buf, "CN=exam");
}

/* --------------------------------------------------- alternative names --- */

static void test_alt_names(void)
{
    char          buf[128];
    unsigned long used;

    used = 0;
    used = tls_cert_list_append(buf, sizeof(buf), used, BYTES("example.com"));
    CHECK_STR(buf, "example.com");

    used = tls_cert_list_append(buf, sizeof(buf), used, BYTES("www.example.com"));
    CHECK_STR(buf, "example.com, www.example.com");
    CHECK(used == 28);

    /* A wildcard is a name like any other and is shown as it stands. */
    used = tls_cert_list_append(buf, sizeof(buf), used, BYTES("*.example.com"));
    CHECK_STR(buf, "example.com, www.example.com, *.example.com");

    /* An empty entry adds nothing, and adds no separator either. */
    used = tls_cert_list_append(buf, sizeof(buf), used, NULL, 0);
    CHECK_STR(buf, "example.com, www.example.com, *.example.com");

    /* Sanitised on the same rule as a name. */
    used = 0;
    used = tls_cert_list_append(buf, sizeof(buf), used, BYTES("a\0b"));
    CHECK_STR(buf, "a?b");
    CHECK(used == 3);

    /* And measured the same way. */
    used = 0;
    used = tls_cert_list_append(NULL, 0, used, BYTES("example.com"));
    used = tls_cert_list_append(NULL, 0, used, BYTES("www.example.com"));
    CHECK(used == 28);
}

/* ------------------------------------------------------------ the dates --- */

/* 2026-08-16 00:00:00 UTC, checked against an independent conversion. */
#define AUG_16_2026     1786838400UL

static void test_utc_time(void)
{
    /* YYMMDDhhmmssZ, which is what every public certificate carries. */
    CHECK(tls_cert_asn1_time(BYTES("260816000000Z"), 0) == AUG_16_2026);

    /* Seconds are optional in the encoding. */
    CHECK(tls_cert_asn1_time(BYTES("2608160000Z"), 0) == AUG_16_2026);

    /* One second after the epoch.  The epoch itself is not asserted, because
       0 is also what an unparseable date reports, so that case would pass on
       either meaning. */
    CHECK(tls_cert_asn1_time(BYTES("700101000001Z"), 0) == 1UL);

    /* Hours, minutes and seconds all land where they should. */
    CHECK(tls_cert_asn1_time(BYTES("260816010203Z"), 0)
          == AUG_16_2026 + 3600UL + 120UL + 3UL);
}

/*
 * RFC 5280 4.1.2.5.1: a UTCTime year below 50 is 20YY and 50 or above is 19YY.
 * Getting the pivot wrong dates a certificate a hundred years out, in a field
 * somebody is being asked to decide on.
 */
static void test_utc_century_pivot(void)
{
    /* 49 -> 2049, the last year on the near side. */
    CHECK(tls_cert_asn1_time(BYTES("490101000000Z"), 0) == 2493072000UL);

    /* 50 -> 1950, which is before the epoch and therefore not reportable. */
    CHECK(tls_cert_asn1_time(BYTES("500101000000Z"), 0) == 0);

    /* 99 -> 1999, and that one is after the epoch. */
    CHECK(tls_cert_asn1_time(BYTES("990101000000Z"), 0) == 915148800UL);
}

static void test_generalized_time(void)
{
    CHECK(tls_cert_asn1_time(BYTES("20260816000000Z"), 1) == AUG_16_2026);
    CHECK(tls_cert_asn1_time(BYTES("202608160000Z"), 1) == AUG_16_2026);

    /* A root that expires in 2050 is a GeneralizedTime precisely because
       UTCTime cannot say it. */
    CHECK(tls_cert_asn1_time(BYTES("20500101000000Z"), 1) == 2524608000UL);
}

/*
 * Leap years, in both directions.  2000 is a leap year (divisible by 400) and
 * 1900 and 2100 are not (divisible by 100), which is the rule a naive
 * every-fourth-year count gets wrong once per century.
 */
static void test_leap_years(void)
{
    /* 2024-02-29 exists. */
    CHECK(tls_cert_asn1_time(BYTES("240229000000Z"), 0) == 1709164800UL);

    /* 2023-02-29 does not. */
    CHECK(tls_cert_asn1_time(BYTES("230229000000Z"), 0) == 0);

    /* 2000-02-29 does: the four-hundred-year rule. */
    CHECK(tls_cert_asn1_time(BYTES("000229000000Z"), 0) == 951782400UL);

    /* 2100-02-29 does not: the hundred-year rule. */
    CHECK(tls_cert_asn1_time(BYTES("21000229000000Z"), 1) == 0);

    /* And the day after 2100-02-28 is 2100-03-01, so a century that is not a
       leap year has to shift everything after it by a day. */
    CHECK(tls_cert_asn1_time(BYTES("21000301000000Z"), 1)
          == tls_cert_asn1_time(BYTES("21000228000000Z"), 1) + 86400UL);
}

/*
 * What will not parse reports 0, which is what struct TLSCertificate documents
 * for a date it could not read.  A wrong number here is worse than no number.
 */
static void test_bad_dates(void)
{
    CHECK(tls_cert_asn1_time(NULL, 13, 0) == 0);

    /* Too short for the format. */
    CHECK(tls_cert_asn1_time(BYTES("2608160000"), 0) == 0);
    CHECK(tls_cert_asn1_time(BYTES("202608160000"), 1) == 0);

    /* Not digits. */
    CHECK(tls_cert_asn1_time(BYTES("26o816000000Z"), 0) == 0);
    CHECK(tls_cert_asn1_time(BYTES("2608160000xxZ"), 0) == 0);

    /* No terminator where one is required. */
    CHECK(tls_cert_asn1_time(BYTES("260816000000"), 0) == 0);

    /*
     * A local offset instead of Zulu.  RFC 5280 requires Z, so this is
     * malformed, and nx_secure's own converter reads the offset and then never
     * applies it -- a time silently out by hours.  Refusing it says so.
     */
    CHECK(tls_cert_asn1_time(BYTES("260816000000+0100"), 0) == 0);

    /* Out-of-range components. */
    CHECK(tls_cert_asn1_time(BYTES("261316000000Z"), 0) == 0);  /* month 13 */
    CHECK(tls_cert_asn1_time(BYTES("260016000000Z"), 0) == 0);  /* month 0  */
    CHECK(tls_cert_asn1_time(BYTES("260832000000Z"), 0) == 0);  /* day 32   */
    CHECK(tls_cert_asn1_time(BYTES("260800000000Z"), 0) == 0);  /* day 0    */
    CHECK(tls_cert_asn1_time(BYTES("260816240000Z"), 0) == 0);  /* hour 24  */
    CHECK(tls_cert_asn1_time(BYTES("260816006000Z"), 0) == 0);  /* min 60   */
    CHECK(tls_cert_asn1_time(BYTES("260816000061Z"), 0) == 0);  /* sec 61   */
    CHECK(tls_cert_asn1_time(BYTES("260431000000Z"), 0) == 0);  /* 31 April */

    /* Before the epoch and after the range this reports on. */
    CHECK(tls_cert_asn1_time(BYTES("19690101000000Z"), 1) == 0);
    CHECK(tls_cert_asn1_time(BYTES("22000101000000Z"), 1) == 0);

    /* A leap second is clamped rather than refused: a certificate that carries
       one is asking for an ordering, not for a 61st second. */
    CHECK(tls_cert_asn1_time(BYTES("260816000060Z"), 0)
          == tls_cert_asn1_time(BYTES("260816000059Z"), 0));
}

/* ----------------------------------------------------- the fingerprint --- */

static void hex(const unsigned char *digest, char *out)
{
    static const char digits[] = "0123456789abcdef";
    int               i;

    for (i = 0; i < 32; i++)
    {
        out[i * 2]     = digits[(digest[i] >> 4) & 0xF];
        out[i * 2 + 1] = digits[digest[i] & 0xF];
    }
    out[64] = '\0';
}

static void digest_of(const char *data, unsigned int length, char *out)
{
    C68K_SHA256   ctx;
    unsigned char digest[32];
    unsigned char copy[64];

    memset(digest, 0, sizeof(digest));
    memcpy(copy, data, length);

    CHECK(c68k_sha256_initialize(&ctx, NX_CRYPTO_HASH_SHA256) == NX_CRYPTO_SUCCESS);
    CHECK(c68k_sha256_update(&ctx, copy, length) == NX_CRYPTO_SUCCESS);
    CHECK(c68k_sha256_digest_calculate(&ctx, digest, NX_CRYPTO_HASH_SHA256)
          == NX_CRYPTO_SUCCESS);

    hex(digest, out);
}

/*
 * The fingerprint is the one field a caller compares against a value it was
 * given by another route, so it has to be the SHA-256 everybody else computes
 * and not merely a repeatable number.  These are the published vectors.
 *
 * The initialize/update/calculate sequence is the one src/tlslib/tls_cert.c
 * uses over the certificate DER; what is checked here is that sequence, not
 * the block function underneath it.
 */
static void test_fingerprint(void)
{
    static const char *const abc =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    static const char *const empty =
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    static const char *const two_block =
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1";

    char out[65];

    digest_of("abc", 3, out);
    CHECK_STR(out, abc);

    digest_of("", 0, out);
    CHECK_STR(out, empty);

    digest_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
              out);
    CHECK_STR(out, two_block);
}

/* A certificate DER runs to a couple of kilobytes, which is thirty-odd blocks
   and a partial one, so the length the library actually hashes is covered as
   well as the short vectors. */
static void test_fingerprint_long(void)
{
    static const char *const million_a_prefix =
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";

    C68K_SHA256   ctx;
    unsigned char digest[32];
    unsigned char block[1000];
    char          out[65];
    int           i;

    memset(block, 'a', sizeof(block));

    CHECK(c68k_sha256_initialize(&ctx, NX_CRYPTO_HASH_SHA256) == NX_CRYPTO_SUCCESS);
    for (i = 0; i < 1000; i++)
        CHECK(c68k_sha256_update(&ctx, block, sizeof(block)) == NX_CRYPTO_SUCCESS);
    CHECK(c68k_sha256_digest_calculate(&ctx, digest, NX_CRYPTO_HASH_SHA256)
          == NX_CRYPTO_SUCCESS);

    hex(digest, out);
    CHECK_STR(out, million_a_prefix);
}

int main(void)
{
    test_name_order();
    test_name_missing_fields();
    test_name_sanitised();
    test_name_measure();
    test_name_truncation();
    test_alt_names();
    test_utc_time();
    test_utc_century_pivot();
    test_generalized_time();
    test_leap_years();
    test_bad_dates();
    test_fingerprint();
    test_fingerprint_long();

    printf("\n%d checks, %d failure(s)\n", checks, failures);

    return failures == 0 ? 0 : 1;
}
