/* The ssh client's console text conversion and OSC filter (clients/dropbear/sshcs.c). */

#include <stdio.h>
#include <string.h>

#include "sshcs.h"

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

static uint8_t title[64];
static size_t  title_len;
static int     titles;

static void on_title(void *ctx, const uint8_t *utf8, size_t len)
{
    (void)ctx;
    titles++;
    title_len = len < sizeof(title) ? len : sizeof(title);
    memcpy(title, utf8, title_len);
}

/* Feed `in` in pieces of `step` bytes through a small output buffer. */
static size_t feed(sshcs_out *s, const char *in, size_t n, size_t step,
                   uint8_t *out, size_t cap)
{
    uint8_t buf[SSHCS_CHAR_MAX + 3];
    size_t  at = 0, o = 0;

    while (at < n)
    {
        size_t len = n - at < step ? n - at : step;

        while (len > 0)
        {
            size_t used, w;

            w = sshcs_out_run(s, (const uint8_t *)in + at, len,
                              buf, sizeof(buf), &used);
            CHECK(used > 0 || w > 0, "progress on every call");
            if (used == 0 && w == 0)
                return o;
            if (o + w <= cap)
                memcpy(out + o, buf, w);
            o += w;
            at += used;
            len -= used;
        }
    }
    return o;
}

static int no_c1(const uint8_t *p, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
        if (p[i] >= 0x80 && p[i] <= 0x9F)
            return 0;
    return 1;
}

static uint8_t tbuf[SSHCS_TITLE_MAX];

static void reset(sshcs_out *s, const txt_codeset *cs, int convert, int osc)
{
    memset(s, 0, sizeof(*s));
    s->cs        = cs;
    s->convert   = (uint8_t)convert;
    s->osc       = (uint8_t)osc;
    s->title_buf = tbuf;
    s->title     = on_title;
    titles       = 0;
    title_len    = 0;
}

/* Latin-2 0xEC is U+011B, e caron; nothing else mapped. */
static uint16_t latin2_upper[128];

static size_t foreign_e(void *ctx, uint32_t cp, uint8_t *out, size_t cap)
{
    (void)ctx;
    (void)cap;
    if (cp == 0x20ACu)                  /* the euro sign */
    {
        memcpy(out, "EUR", 3);
        return 3;
    }
    if (cp == 0x2026u)                  /* a hook that returns C1 */
    {
        out[0] = 0x9B;
        return 1;
    }
    return 0;
}

int main(void)
{
    static const txt_codeset latin1 = { NULL, NULL, NULL };
    txt_codeset latin2 = { latin2_upper, foreign_e, NULL };
    sshcs_out s;
    uint8_t   out[512];
    size_t    n, step;

    latin2_upper[0xEC - 0x80] = 0x011B;

    /* Split at every point: the same result. */
    for (step = 1; step <= 6; step++)
    {
        const char in[] = "a\xC3\xA4" "b\xE2\x82\xAC" "c";  /* a a-umlaut b euro c */

        reset(&s, &latin1, 1, 1);
        n = feed(&s, in, sizeof(in) - 1, step, out, sizeof(out));
        CHECK(n == 5 && memcmp(out, "a\xE4" "b?c", 5) == 0,
              "UTF-8 split across buffers to ISO-8859-1");
    }

    /* e caron is C4 9B: 0x9B must never reach the console. */
    reset(&s, &latin1, 1, 1);
    n = feed(&s, "\xC4\x9B", 2, 1, out, sizeof(out));
    CHECK(n == 1 && out[0] == '?' && no_c1(out, n),
          "e caron is '?' in ISO-8859-1, no CSI");
    reset(&s, &latin2, 1, 1);
    n = feed(&s, "\xC4\x9B\xE2\x82\xAC\xE2\x80\xA6", 8, 1, out, sizeof(out));
    CHECK(n == 5 && memcmp(out, "\xEC" "EUR?", 5) == 0 && no_c1(out, n),
          "the local codeset, foreign text, and a C1 from the hook");

    /* Invalid: a stray continuation, C0/C1 leads, overlong, a surrogate,
       a cut-short sequence, U+0080-U+009F. */
    reset(&s, &latin1, 1, 1);
    {
        const char in[] = "\x80" "\xC0\xAF" "\xE0\x80\xAF" "\xED\xA0\x80"
                          "\xC3" "x" "\xC2\x9B" "\xF5" "\xFF";

        n = feed(&s, in, sizeof(in) - 1, 3, out, sizeof(out));
        CHECK(no_c1(out, n), "invalid UTF-8 never yields 0x80-0x9F");
        CHECK(n == 10 && memcmp(out, "??????x???", 10) == 0,
              "invalid UTF-8 bytes are '?'");
    }

    /* Off: byte-exact. */
    reset(&s, &latin1, 0, 0);
    n = feed(&s, "\xC4\x9B\x1B]0;t\x07", 8, 2, out, sizeof(out));
    CHECK(n == 8 && memcmp(out, "\xC4\x9B\x1B]0;t\x07", 8) == 0,
          "conversion and filter off pass bytes unchanged");

    /* OSC 0 with BEL, OSC 2 with ESC \, split at every point. */
    for (step = 1; step <= 7; step++)
    {
        const char in[] = "x\x1B]0;h\xC3\xA4st\x07y\x1B]2;two\x1B\\z\x1B[1mw";

        reset(&s, &latin1, 1, 1);
        n = feed(&s, in, sizeof(in) - 1, step, out, sizeof(out));
        CHECK(n == 8 && memcmp(out, "xyz\x1B[1mw", 8) == 0,
              "OSC stripped, other ESC sequences kept");
        CHECK(titles == 2 && title_len == 3 && memcmp(title, "two", 3) == 0,
              "OSC 0 and OSC 2 set the title, BEL and ESC \\");
    }

    /* Other OSC numbers: stripped, no title. */
    reset(&s, &latin1, 1, 1);
    {
        const char in[] = "a\x1B]7;file://h/p\x07" "b\x1B]133;A\x1B\\c";

        n = feed(&s, in, sizeof(in) - 1, 4, out, sizeof(out));
    }
    CHECK(n == 3 && memcmp(out, "abc", 3) == 0 && titles == 0,
          "OSC 7 and 133 stripped without a title");

    /* Overflow: the title is dropped, the sequence still stripped. */
    reset(&s, &latin1, 1, 1);
    {
        static char in[SSHCS_TITLE_MAX + 16];
        size_t      k = 0;

        in[k++] = 0x1B; in[k++] = ']'; in[k++] = '0'; in[k++] = ';';
        memset(in + k, 'A', SSHCS_TITLE_MAX + 1);
        k += SSHCS_TITLE_MAX + 1;
        in[k++] = 0x07;
        in[k++] = 'k';
        n = feed(&s, in, k, 5, out, sizeof(out));
        CHECK(n == 1 && out[0] == 'k' && titles == 0,
              "an over-long title is dropped and the stream resumes");
    }

    /* ESC without '\' ends the OSC; the ESC starts the next sequence. */
    reset(&s, &latin1, 1, 1);
    n = feed(&s, "\x1B]0;t\x1B[2Jq", 10, 1, out, sizeof(out));
    CHECK(n == 5 && memcmp(out, "\x1B[2Jq", 5) == 0 && titles == 0,
          "an unterminated OSC gives way to the next ESC");

    /* ESC cuts a UTF-8 sequence short; ESC ESC; a trailing ESC is held. */
    reset(&s, &latin1, 1, 1);
    n = feed(&s, "\xC3\x1B\x1B" "A\x1B", 5, 1, out, sizeof(out));
    CHECK(n == 4 && memcmp(out, "?\x1B\x1B" "A", 4) == 0,
          "ESC handling around a held byte");

    /* The title itself to the local codeset, controls replaced. */
    n = sshcs_utf8_to_local(&latin1, (const uint8_t *)"h\xC3\xA4\x09\xC4\x9B\xC3",
                            7, out, sizeof(out));
    CHECK(n == 5 && memcmp(out, "h\xE4???", 5) == 0,
          "a title to ISO-8859-1, controls and C1 replaced");
    n = sshcs_utf8_to_local(&latin2, (const uint8_t *)"\xE2\x82\xAC", 3, out, 2);
    CHECK(n == 0, "a character that does not fit is not split");

    /* Keystrokes. */
    n = txt_local_to_utf8(&latin1, 0xE4, out);
    CHECK(n == 2 && out[0] == 0xC3 && out[1] == 0xA4, "a-umlaut to UTF-8");
    n = txt_local_to_utf8(&latin2, 0xEC, out);
    CHECK(n == 2 && out[0] == 0xC4 && out[1] == 0x9B, "e caron to UTF-8");
    n = txt_local_to_utf8(&latin2, 0xE4, out);
    CHECK(n == 1 && out[0] == '?', "an unmapped byte is '?'");
    n = txt_local_to_utf8(&latin2, 'q', out);
    CHECK(n == 1 && out[0] == 'q', "ASCII unchanged");

    if (failures == 0)
        printf("sshcs=PASS\n");
    return failures == 0 ? 0 : 1;
}
