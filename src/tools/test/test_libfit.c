/*
 * libfit_need() on built hunk headers and libfit_short() on the figures that
 * decide AddNetInterface's "bytes are free" line (#55).
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include "libfit.h"

#define HEADROOM (200UL * 1024UL)

static int failures;

static void check(const char *what, int got, int want)
{
    if (got != want)
    {
        printf("FAIL %s: got %d, want %d\n", what, got, want);
        failures++;
    }
    else
        printf("ok   %s\n", what);
}

static size_t put_long(unsigned char *b, size_t at, unsigned long v)
{
    b[at]     = (unsigned char)(v >> 24);
    b[at + 1] = (unsigned char)(v >> 16);
    b[at + 2] = (unsigned char)(v >> 8);
    b[at + 3] = (unsigned char)v;
    return at + 4;
}

/* A HUNK_HEADER naming `n` hunks of the given byte sizes. */
static size_t header(unsigned char *b, const unsigned long *bytes,
                     const unsigned long *flags, unsigned n)
{
    size_t   at = 0;
    unsigned i;

    at = put_long(b, at, 0x3F3);
    at = put_long(b, at, 0);
    at = put_long(b, at, n);
    at = put_long(b, at, 0);
    at = put_long(b, at, n - 1);
    for (i = 0; i < n; i++)
    {
        at = put_long(b, at, (bytes[i] / 4) | flags[i]);
        if ((flags[i] & 0xC0000000UL) == 0xC0000000UL)
            at = put_long(b, at, 0x00010000UL);
    }
    return at;
}

/* The shape measured in #55: bsdsocket.library at 6de3417c, three hunks. */
static const unsigned long lib_bytes[3] = { 334524, 224, 9512 };
static const unsigned long no_flags[3]  = { 0, 0, 0 };

static void test_need(void)
{
    unsigned char       b[LIBFIT_HEAD_MAX];
    LibFitNeed          need;
    size_t              len = header(b, lib_bytes, no_flags, 3);
    const unsigned long ext[2]   = { 1000, 64 };
    const unsigned long extfl[2] = { 0xC0000000UL, 0x40000000UL };

    check("a library header is read",
          libfit_need(b, (ULONG)len, &need), 1);
    check("total is every hunk plus 8 bytes each",
          need.total == 344260UL + 24UL, 1);
    check("largest is the code hunk plus 8",
          need.largest == 334532UL, 1);

    check("a header cut short is not a size",
          libfit_need(b, (ULONG)len - 4, &need), 0);
    check("and leaves no need behind", need.total == 0 && need.largest == 0, 1);

    b[3] = 0xF3 ^ 1;
    check("not HUNK_HEADER", libfit_need(b, (ULONG)len, &need), 0);

    len = header(b, ext, extfl, 2);
    check("an attribute long is skipped", libfit_need(b, (ULONG)len, &need), 1);
    check("and both hunks still count", need.total == 1000UL + 64UL + 16UL, 1);

    put_long(b, 20, 0x3FFFFFFFUL);
    check("a hunk too large to be real is refused",
          libfit_need(b, (ULONG)len, &need), 0);

    check("no bytes", libfit_need(b, 0, &need), 0);
}

static void test_short(void)
{
    LibFitNeed lib  = { 344284UL, 334532UL };
    LibFitNeed none = { 0, 0 };
    LibFitNeed wide = { 300000UL, 250000UL };

    /* #55: 344,000 free and 343,392 in one piece; the library never loads. */
    check("512 KB A2000: the load file does not fit",
          libfit_short(344000UL, 343392UL, &lib, HEADROOM), 1);

    /* It loads, but leaves the stack no room: the 73,248 of 585d7126. */
    check("it fits, with less than the headroom over",
          libfit_short(344284UL + 73248UL, 400000UL, &lib, HEADROOM), 1);

    check("room for the file and the headroom: not memory",
          libfit_short(344284UL + HEADROOM, 400000UL, &lib, HEADROOM), 0);
    check("an 8 MB machine: not memory",
          libfit_short(8000000UL, 7000000UL, &lib, HEADROOM), 0);

    check("the total fits but no block holds the largest hunk",
          libfit_short(2000000UL, 200000UL, &wide, HEADROOM), 1);

    /* Resident, or the header unreadable: the headroom alone decides. */
    check("nothing to load, little free",
          libfit_short(100000UL, 100000UL, &none, HEADROOM), 1);
    check("nothing to load, plenty free",
          libfit_short(500000UL, 400000UL, &none, HEADROOM), 0);
    check("no need given at all",
          libfit_short(500000UL, 400000UL, NULL, HEADROOM), 0);

    check("a need over everything does not wrap",
          libfit_short(0xFFFFFFF0UL, 0xFFFFFFF0UL,
                       &(LibFitNeed){ 0xFFFFFFFFUL, 16UL }, HEADROOM), 1);
}

int main(void)
{
    test_need();
    test_short();

    if (failures != 0)
    {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("libfit: all passed\n");
    return 0;
}
