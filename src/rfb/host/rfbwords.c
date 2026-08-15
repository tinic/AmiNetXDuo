/*
 * The control words, checked against the spellings the browser reads.
 *
 * The receiver is src/tools/web/client/console/tiles.ts and .../input.ts, and
 * neither of them is compiled by anything here, so what this asserts is the
 * literal text those two files parse: `geom` takes six numbers, `pal` is two
 * hex characters a byte and 3 << depth bytes of them, and the five words a
 * viewer sends are read exactly as it sends them.  A server and a page that
 * disagree about one space produce a blank canvas and no error anywhere.
 *
 *   cc -std=c99 -Iinclude src/rfb/host/rfbwords.c src/rfb/rfb_words.c -o rfbwords
 *
 * Output is one line per failure and the exit code is the verdict.
 *
 * SPDX-License-Identifier: MIT
 */

#include "aminetxduo/rfb_words.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

static void eq(const char *got, const char *want, const char *what)
{
    checks++;
    if (strcmp(got, want) != 0) {
        failures++;
        printf("FAIL %s\n  got  \"%s\"\n  want \"%s\"\n", what, got, want);
    }
}

static void yes(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL %s\n", what);
    }
}

static void word_geom(void)
{
    rfb_geom g;
    char out[RFB_WORD_GEOM_MAX + 1];

    memset(&g, 0, sizeof(g));
    g.width = 640;
    g.height = 256;
    g.depth = 2;
    g.bytes_per_row = 80;
    g.tile_w = 16;
    g.tile_h = 16;

    yes(rfb_word_geom(out, sizeof(out), &g) == 23, "geom length");
    eq(out, "geom 640 256 2 80 16 16", "geom, the 640x256x2 Workbench");

    /* A screen whose width is not a whole number of bytes: the grid is over
       bytes_per_row and not over the width, and the word carries both. */
    g.width = 644;
    g.bytes_per_row = 82;
    (void)rfb_word_geom(out, sizeof(out), &g);
    eq(out, "geom 644 256 2 82 16 16", "geom, a width with padding after it");

    /* The biggest one there is still fits the buffer the header promises. */
    g.width = 65535;
    g.height = 65535;
    g.depth = 8;
    g.bytes_per_row = 65535;
    g.tile_w = 64;
    g.tile_h = 64;
    yes(rfb_word_geom(out, sizeof(out), &g) > 0, "geom, the largest there is");

    yes(rfb_word_geom(out, 8, &g) == 0, "geom refuses a buffer it overruns");
}

static void word_pal(void)
{
    static const rfb_u8 four[12] = {
        0x00, 0x55, 0xaa, 0xff, 0x88, 0x88, 0x00, 0x00, 0x00, 0x12, 0x34, 0x56
    };
    char out[RFB_WORD_PAL_MAX + 1];

    yes(rfb_word_pal(out, sizeof(out), four, 4) == 4 + 24, "pal length");
    eq(out, "pal 0055aaff8888000000123456", "pal, four colours");

    /* The viewer computes 3 * (1 << depth) and refuses anything else, so the
       length is the contract and not a convenience. */
    yes(rfb_word_pal(out, sizeof(out), four, 4) == 4u + 6u * 4u,
        "pal is six characters a colour");

    yes(rfb_word_pal(out, 10, four, 4) == 0, "pal refuses a buffer it overruns");
}

/* The word's leading fields, which is what the numbers are checked through:
   the tail is hex and is checked by length. */
static int starts(const char *s, const char *want)
{
    return strncmp(s, want, strlen(want)) == 0;
}

static void words_pointer(void)
{
    /* The classic pointer: 16 wide, 16 rows, two planes, three colours, and
       on a hires screen one sprite pixel covers two across and one down. */
    rfb_pointer p;
    rfb_u8 rgb[9];
    rfb_u8 bits[2 * 2 * 16];
    static char out[RFB_WORD_PTR_MAX];
    rfb_u32 n;
    rfb_u32 i;

    for (i = 0; i < sizeof(rgb); i++)  rgb[i]  = (rfb_u8)(i * 16u + 1u);
    for (i = 0; i < sizeof(bits); i++) bits[i] = (rfb_u8)i;

    p.width = 16; p.height = 16; p.depth = 2;
    p.x_scale = 2; p.y_scale = 1;
    p.hot_x = 0; p.hot_y = 0;

    n = rfb_word_ptr(out, sizeof(out), &p, rgb, bits);
    yes(n > 0u, "ptr builds");
    yes(n > 0u && out[0] == 'p' && out[1] == 't' && out[2] == 'r' &&
        out[3] == ' ', "ptr starts with its keyword");
    yes(n > 0u && starts(out, "ptr 16 16 2 2 1 0 0 "),
        "ptr carries the shape, the scale and the hotspot");
    /* 4 + "16 16 2 2 1 0 0 " is 16, then 18 hex of colour, a space, 128 hex. */
    yes(n == 4u + 16u + 18u + 1u + 128u, "ptr is exactly as long as it says");

    /* A hotspot away from the corner, and negative, which a crosshair has. */
    p.hot_x = -3; p.hot_y = 7;
    n = rfb_word_ptr(out, sizeof(out), &p, rgb, bits);
    yes(n > 0u && starts(out, "ptr 16 16 2 2 1 -3 7 "),
        "a negative hotspot keeps its sign");

    /* Refusals: every one of these would have a viewer draw a wrong pointer
       with no way to know it. */
    p.hot_x = 0; p.hot_y = 0;
    p.width = 0;
    yes(rfb_word_ptr(out, sizeof(out), &p, rgb, bits) == 0u,
        "a pointer no pixels wide is refused");
    p.width = RFB_PTR_MAX_W + 1u;
    yes(rfb_word_ptr(out, sizeof(out), &p, rgb, bits) == 0u,
        "a pointer wider than the vocabulary carries is refused");
    p.width = 16; p.depth = RFB_PTR_MAX_DEPTH + 1u;
    yes(rfb_word_ptr(out, sizeof(out), &p, rgb, bits) == 0u,
        "a pointer deeper than the vocabulary carries is refused");
    p.depth = 2; p.x_scale = 0;
    yes(rfb_word_ptr(out, sizeof(out), &p, rgb, bits) == 0u,
        "a zero scale is refused");
    p.x_scale = 2;
    yes(rfb_word_ptr(out, 40, &p, rgb, bits) == 0u,
        "a buffer too small is refused rather than half filled");
}

static void words_in(void)
{
    rfb_input ev;

    yes(rfb_word_parse("refresh", 7, &ev) && ev.kind == RFB_IN_REFRESH,
        "refresh");

    yes(rfb_word_parse("reset", 5, &ev) && ev.kind == RFB_IN_RESET,
        "reset");

    yes(rfb_word_parse("m 320 128 1", 11, &ev) && ev.kind == RFB_IN_POINTER &&
        ev.a == 320 && ev.b == 128 && ev.c == 1, "m X Y BUTTONS");

    yes(rfb_word_parse("m 0 0 0", 7, &ev) && ev.kind == RFB_IN_POINTER &&
        ev.a == 0 && ev.b == 0 && ev.c == 0, "m with every field zero");

    yes(rfb_word_parse("m 639 255 6", 11, &ev) && ev.c == 6,
        "m carries the middle and right buttons together");

    yes(rfb_word_parse("w 0 -1", 6, &ev) && ev.kind == RFB_IN_WHEEL &&
        ev.a == 0 && ev.b == -1, "w DX DY, negative");

    yes(rfb_word_parse("w -1 1", 6, &ev) && ev.a == -1 && ev.b == 1,
        "w DX DY, both signs");

    yes(rfb_word_parse("kd 64 0", 7, &ev) && ev.kind == RFB_IN_KEYDOWN &&
        ev.a == 64 && ev.b == 0, "kd RAW QUAL");

    yes(rfb_word_parse("ku 64 3", 7, &ev) && ev.kind == RFB_IN_KEYUP &&
        ev.a == 64 && ev.b == 3, "ku RAW QUAL");

    /* Not terminated: a text frame carries a length and nothing else, and the
       parser reads exactly that many bytes. */
    yes(rfb_word_parse("m 1 2 3 and then some rubbish", 7, &ev) &&
        ev.a == 1 && ev.b == 2 && ev.c == 3,
        "the length is what bounds the word, not a terminator");
}

static void words_refused(void)
{
    rfb_input ev;

    yes(!rfb_word_parse("", 0, &ev), "an empty frame is not a word");
    yes(!rfb_word_parse("mm 1 2 3", 8, &ev), "a keyword that only starts ours");
    yes(!rfb_word_parse("m 1 2", 5, &ev), "m with a field missing");
    yes(!rfb_word_parse("m 1 2 3 4", 9, &ev), "m with a field too many");
    yes(!rfb_word_parse("m a b c", 7, &ev), "m with something that is not a number");
    yes(!rfb_word_parse("refreshx", 8, &ev), "refresh with something after it");
    yes(!rfb_word_parse("size 80 25", 10, &ev),
        "the terminal's vocabulary is not this one");
    yes(!rfb_word_parse("m 1234567 0 0", 13, &ev),
        "a number longer than any coordinate is refused whole");

    /* Ignored and never an error is the compatibility rule, and the caller
       reads that off a 0 return rather than off a kind it has to know. */
    ev.kind = RFB_IN_POINTER;
    yes(!rfb_word_parse("hello", 5, &ev) && ev.kind == RFB_IN_NONE,
        "an unknown word leaves nothing behind");
}

int main(void)
{
    word_geom();
    word_pal();
    words_in();
    words_pointer();
    words_refused();

    printf("%d checks, %d failure%s\n", checks, failures,
           failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
