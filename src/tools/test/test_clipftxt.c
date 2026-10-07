/*
 * The console's clipboard text (clipftxt.c): UTF-8 to ISO-8859-1 and back,
 * line endings, and the FORM FTXT it writes and reads.
 *
 * SPDX-License-Identifier: MIT
 */

#include "clipftxt.h"

#include <stdio.h>
#include <string.h>

static unsigned long checks;
static unsigned long failures;

static void expect_bytes(const uint8_t *got, uint32_t n, const char *want,
                         const char *what)
{
    checks++;
    if (n != strlen(want) || memcmp(got, want, n) != 0)
    {
        failures++;
        printf("  FAIL %s: got %u bytes\n", what, (unsigned)n);
    }
}

static void expect(int cond, const char *what)
{
    checks++;
    if (!cond)
    {
        failures++;
        printf("  FAIL %s\n", what);
    }
}

static uint32_t to_latin(const char *s, uint8_t *out, uint32_t cap)
{
    return clip_utf8_to_latin1((const uint8_t *)s, (uint32_t)strlen(s), out,
                               cap);
}

static void t_to_latin1(void)
{
    uint8_t  o[64];
    uint32_t n;

    n = to_latin("192.168.1.148", o, sizeof(o));
    expect_bytes(o, n, "192.168.1.148", "ASCII as it is");
    n = to_latin("a\r\nb\rc\n", o, sizeof(o));
    expect_bytes(o, n, "a\nb\nc\n", "CRLF and CR become LF");
    n = to_latin("Gr\xC3\xBC\xC3\x9F" "e", o, sizeof(o));
    expect_bytes(o, n, "Gr\xFC\xDF" "e", "Latin-1 letters arrive");
    n = to_latin("\xE2\x82\xAC 5", o, sizeof(o));
    expect_bytes(o, n, "? 5", "a euro sign has no Latin-1 byte");
    n = to_latin("\xF0\x9F\x98\x80!", o, sizeof(o));
    expect_bytes(o, n, "?!", "nor has an emoji");
    n = to_latin("x\xC3", o, sizeof(o));
    expect_bytes(o, n, "x?", "a sequence cut short");
    n = to_latin("\x80y", o, sizeof(o));
    expect_bytes(o, n, "?y", "a stray continuation byte");
    n = to_latin("\xC1\x81", o, sizeof(o));
    expect_bytes(o, n, "?", "an overlong A");
    n = to_latin("abcdef", o, 3);
    expect_bytes(o, n, "abc", "cut to the buffer");

    {
        char buf[] = "line\r\n\xC3\xA9t\xC3\xA9";
        n = clip_utf8_to_latin1((uint8_t *)buf, (uint32_t)strlen(buf),
                                (uint8_t *)buf, sizeof(buf));
        expect_bytes((uint8_t *)buf, n, "line\n\xE9t\xE9", "in place");
    }
}

static void t_to_utf8(void)
{
    uint8_t  o[16];
    uint32_t n;

    n = clip_latin1_to_utf8((const uint8_t *)"a\xFC" "b", 3, o, sizeof(o));
    expect_bytes(o, n, "a\xC3\xBC" "b", "a Latin-1 letter widens to two bytes");
    n = clip_latin1_to_utf8((const uint8_t *)"a\xFC", 2, o, 2);
    expect_bytes(o, n, "a", "never half a character");
}

static void t_ftxt(void)
{
    uint8_t  iff[64];
    uint8_t  o[64];
    uint32_t n;

    clip_ftxt_header(3, iff);
    memcpy(iff + CLIP_FTXT_HEADER, "abc", 3);
    iff[CLIP_FTXT_HEADER + 3] = 0;
    expect(memcmp(iff, "FORM\0\0\0\x10" "FTXTCHRS\0\0\0\x03", 20) == 0,
           "the header for three bytes, padded to four");
    n = clip_ftxt_text(iff, CLIP_FTXT_HEADER + 4, o, sizeof(o));
    expect_bytes(o, n, "abc", "and it reads back");

    /* Two CHRS chunks with another chunk between, as an editor may write. */
    {
        static const uint8_t two[] =
            "FORM\0\0\0\x26" "FTXT"
            "CHRS\0\0\0\x03" "one\0"
            "FONS\0\0\0\x02" "xx"
            "CHRS\0\0\0\x04" "two!";
        n = clip_ftxt_text(two, sizeof(two) - 1, o, sizeof(o));
        expect_bytes(o, n, "onetwo!", "every CHRS, and nothing else");
    }

    n = clip_ftxt_text((const uint8_t *)"FORM\0\0\0\x04ILBM", 12, o, sizeof(o));
    expect(n == 0, "a picture is not text");

    /* A CHRS that claims more than arrived reads as far as it goes. */
    n = clip_ftxt_text((const uint8_t *)"FORM\0\0\0\x30" "FTXTCHRS\0\0\0\x20" "short",
                       25, o, sizeof(o));
    expect_bytes(o, n, "short", "a chunk cut short");

    clip_ftxt_header(5, iff);
    memcpy(iff + CLIP_FTXT_HEADER, "hello", 5);
    iff[CLIP_FTXT_HEADER + 5] = 0;
    n = clip_ftxt_text(iff, CLIP_FTXT_HEADER + 6, o, 4);
    expect_bytes(o, n, "hell", "cut to the buffer");
}

int main(void)
{
    printf("Console clipboard text\n");
    t_to_latin1();
    t_to_utf8();
    t_ftxt();
    printf("clipftxt checks=%lu failures=%lu\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
