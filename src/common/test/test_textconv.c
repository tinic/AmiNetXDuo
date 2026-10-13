/* Local codeset <-> UTF-8 (src/common/textconv.c). */

#include <stdio.h>
#include <string.h>

#include "aminetxduo/textconv.h"

static int failures;

#define CHECK(expr, message)                                                \
    do                                                                       \
    {                                                                        \
        if (!(expr))                                                         \
        {                                                                    \
            fprintf(stderr, "FAIL: %s\n", message);                         \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static uint16_t latin2[128];            /* only e caron, 0xEC = U+011B */

static size_t lookalike(void *ctx, uint32_t cp, uint8_t *out, size_t cap)
{
    (void)ctx;
    (void)cap;
    if (cp != 0x20ACu)
        return 0;
    memcpy(out, "EUR", 3);
    return 3;
}

static size_t decode_all(txt_dec *d, const txt_codeset *cs, const char *in,
                         size_t n, uint8_t *out)
{
    size_t i, o = 0;

    for (i = 0; i < n; i++)
        o += txt_dec_byte(d, cs, (uint8_t)in[i], out + o);
    return o;
}

int main(void)
{
    txt_codeset cs2 = { latin2, lookalike, NULL, 0 };
    uint8_t     out[64];
    size_t      n, used;
    int         r;

    latin2[0xEC - 0x80] = 0x011B;

    /* A UTF-8 system codeset (the /shell and clipboard path): identity. */
    {
        txt_codeset u8 = { NULL, NULL, NULL, 1 };
        txt_dec     d = { 0, 0, 0, 1 };

        n  = decode_all(&d, &u8, "B\xC3", 2, out);
        n += decode_all(&d, &u8, "\xBC\xE2\x82\xAC", 4, out + n);
        CHECK(n == 6 && memcmp(out, "B\xC3\xBC\xE2\x82\xAC", 6) == 0,
              "UTF-8 system: decoded byte for byte, split or not");
        n = decode_all(&d, &u8, "\xC3\x28", 2, out);
        CHECK(n == 2 && memcmp(out, "?(", 2) == 0, "UTF-8 system: malformed is '?'");
        n = txt_local_to_utf8_str(&u8, (const uint8_t *)"B\xC3\xBC", 3, out,
                                  sizeof(out), &used);
        CHECK(n == 3 && used == 3 && memcmp(out, "B\xC3\xBC", 3) == 0,
              "UTF-8 system: local text goes out as it is");
        r = txt_utf8_to_local_exact(&u8, (const uint8_t *)"B\xC3\xBC", 3, out,
                                    sizeof(out), &n);
        CHECK(r == TXT_OK && n == 3 && memcmp(out, "B\xC3\xBC", 3) == 0,
              "UTF-8 system: an exact name is identity");
        r = txt_utf8_to_local_exact(&u8, (const uint8_t *)"\xC3\x28", 2, out,
                                    sizeof(out), &n);
        CHECK(r == TXT_BAD_UTF8, "UTF-8 system: a malformed name is refused");
    }

    /* Streaming: a sequence split across calls. */
    {
        txt_dec d = { 0, 0, 0, 0 };

        n  = decode_all(&d, NULL, "B\xC3", 2, out);
        n += decode_all(&d, NULL, "\xBC" "cher", 5, out + n);
        CHECK(n == 6 && memcmp(out, "B\xFC" "cher", 6) == 0,
              "a character split across buffers");
    }

    /* C1: '?' for a console, the byte when asked for. */
    {
        txt_dec con = { 0, 0, 0, 0 }, raw = { 0, 0, 0, 1 };

        n = decode_all(&con, NULL, "\xC2\x9B", 2, out);
        CHECK(n == 1 && out[0] == '?', "U+009B is '?' on a console");
        n = decode_all(&raw, NULL, "\xC2\x9B", 2, out);
        CHECK(n == 1 && out[0] == 0x9B, "U+009B is 0x9B when asked");
    }

    /* Invalid, cut short, flushed. */
    {
        txt_dec d = { 0, 0, 0, 0 };

        n = decode_all(&d, NULL, "\x80\xC0\xAF\xED\xA0\x80\xC3x\xE2\x82", 10, out);
        n += txt_dec_flush(&d, out + n);
        CHECK(n == 7 && memcmp(out, "?????x?", 7) == 0,
              "invalid UTF-8 is '?', an open sequence flushes to '?'");
    }

    /* The codeset, look-alike text, and its absence. */
    n = txt_utf8_to_local(&cs2, (const uint8_t *)"\xC4\x9B\xE2\x82\xAC\xC3\xBC",
                          7, out, sizeof(out));
    CHECK(n == 5 && memcmp(out, "\xEC" "EUR?", 5) == 0,
          "codeset, look-alike, and '?' for what neither has");
    n = txt_utf8_to_local(&cs2, (const uint8_t *)"\xE2\x82\xAC", 3, out, 2);
    CHECK(n == 0, "a look-alike is not split by the buffer");

    /* Local to UTF-8: C1 as it is, the codeset above 0xA0. */
    n = txt_local_to_utf8_str(NULL, (const uint8_t *)"a\xFC\x9B", 3, out,
                              sizeof(out), &used);
    CHECK(n == 5 && used == 3 && memcmp(out, "a\xC3\xBC\xC2\x9B", 5) == 0,
          "ISO-8859-1 and C1 to UTF-8");
    n = txt_local_to_utf8_str(&cs2, (const uint8_t *)"\xEC\xE4", 2, out,
                              sizeof(out), &used);
    CHECK(n == 3 && memcmp(out, "\xC4\x9B?", 3) == 0,
          "the codeset to UTF-8, '?' for a byte it leaves empty");
    n = txt_local_to_utf8_str(NULL, (const uint8_t *)"a\xFC", 2, out, 2, &used);
    CHECK(n == 1 && used == 1, "never half a character");

    /* Exact: names. */
    {
        char buf[] = "caf\xC3\xA9/\xC2\x9B";
        size_t len = 0;

        r = txt_utf8_to_local_exact(NULL, (const uint8_t *)buf, strlen(buf),
                                    (uint8_t *)buf, strlen(buf), &len);
        CHECK(r == TXT_OK && len == 6 && memcmp(buf, "caf\xE9/\x9B", 6) == 0,
              "exact, in place");
    }
    r = txt_utf8_to_local_exact(NULL, (const uint8_t *)"\xFC", 1, out, 8, &n);
    CHECK(r == TXT_BAD_UTF8, "a Latin-1 byte is not UTF-8");
    r = txt_utf8_to_local_exact(NULL, (const uint8_t *)"\xC0\xAE", 2, out, 8, &n);
    CHECK(r == TXT_BAD_UTF8, "an overlong '.' is refused");
    r = txt_utf8_to_local_exact(NULL, (const uint8_t *)"a\xC3", 2, out, 8, &n);
    CHECK(r == TXT_BAD_UTF8, "cut short at the end");
    r = txt_utf8_to_local_exact(&cs2, (const uint8_t *)"\xE2\x82\xAC", 3, out, 8, &n);
    CHECK(r == TXT_UNMAPPABLE, "no look-alike for a name");
    r = txt_utf8_to_local_exact(&cs2, (const uint8_t *)"\xC4\x9B", 2, out, 8, &n);
    CHECK(r == TXT_OK && n == 1 && out[0] == 0xEC, "the codeset for a name");
    r = txt_utf8_to_local_exact(NULL, (const uint8_t *)"abc", 3, out, 2, &n);
    CHECK(r == TXT_TOO_LONG, "too long");

    if (failures == 0)
        printf("textconv=PASS\n");
    return failures == 0 ? 0 : 1;
}
