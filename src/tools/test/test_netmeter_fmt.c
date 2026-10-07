/*
 * NetMeter's arithmetic (netmeter_fmt.c): the rate and total text, the delta
 * of a count kept in two 32-bit halves across its carry and a reset, and the
 * bar scale.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netmeter_fmt.h"

#include <stdio.h>
#include <string.h>

static unsigned long checks;
static unsigned long failures;

static void expect_text(const char *got, const char *want, const char *what)
{
    checks++;
    if (strcmp(got, want) != 0)
    {
        failures++;
        printf("  FAIL %s: \"%s\", want \"%s\"\n", what, got, want);
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

static void t_rates(void)
{
    char s[24];

    nm_format_rate(0, s, sizeof(s));            expect_text(s, "0 B/s", "zero");
    nm_format_rate(512, s, sizeof(s));          expect_text(s, "512 B/s", "bytes");
    nm_format_rate(1023, s, sizeof(s));         expect_text(s, "1023 B/s", "under a KB");
    nm_format_rate(1536, s, sizeof(s));         expect_text(s, "1.50 KB/s", "two decimals");
    nm_format_rate(12595, s, sizeof(s));        expect_text(s, "12.2 KB/s", "one decimal");
    nm_format_rate(262144, s, sizeof(s));       expect_text(s, "256 KB/s", "no decimal");
    nm_format_rate(2841641, s, sizeof(s));      expect_text(s, "2.70 MB/s", "a Zorro III card");
    nm_format_rate(0xFFFFFFFFu, s, sizeof(s));  expect_text(s, "3.99 GB/s", "the largest");
    nm_format_rate(2841641, s, 5);              expect_text(s, "2.70", "cut to the buffer");
}

static void t_totals(void)
{
    char s[24];

    nm_format_total(0, 1, s, sizeof(s));        expect_text(s, "1 byte", "one byte");
    nm_format_total(0, 312, s, sizeof(s));      expect_text(s, "312 bytes", "bytes");
    nm_format_total(0, 1536, s, sizeof(s));     expect_text(s, "1.50 KB", "KB");
    nm_format_total(0, 12898509, s, sizeof(s)); expect_text(s, "12.3 MB", "MB");
    /* 0x1_0400_0000 = 4.0625 GB: the high half has to count. */
    nm_format_total(1, 0x04000000u, s, sizeof(s));
    expect_text(s, "4.06 GB", "across 4 GB");
    nm_format_total(0x400, 0, s, sizeof(s));    expect_text(s, "4.00 TB", "TB");
}

static void t_delta(void)
{
    expect(nm_delta(0, 5000, 0, 1000) == 4000, "a plain delta");
    expect(nm_delta(1, 100, 0, 0xFFFFFF00u) == 356, "across the carry");
    expect(nm_delta(0, 10, 0, 5000) == 0, "a reset reads zero");
    expect(nm_delta(0, 10, 3, 0) == 0, "a reset of the high half too");
    expect(nm_delta(5, 0, 0, 0) == 0xFFFFFFFFu, "too far saturates");
}

static void t_rate(void)
{
    expect(nm_rate(2700000, 1000) == 2700000, "a second");
    expect(nm_rate(1350000, 500) == 2700000, "half a second");
    expect(nm_rate(1000, 0) == 0, "no time");
    expect(nm_rate(0xFFFFFFF0u, 1000) == 4294967000u, "large without overflow");
}

static void t_scale(void)
{
    uint32_t s = NM_SCALE_FLOOR;

    expect(nm_scale_next(s, 0, 0) == NM_SCALE_FLOOR, "the floor holds");
    s = nm_scale_next(s, 1000000, 0);
    expect(s == 1250000, "a peak raises it with a quarter headroom");
    s = nm_scale_next(s, 0, 0);
    expect(s == 1250000 - 1250000 / 16, "and it falls by a sixteenth");
    expect(nm_scale_next(0xF0000000u, 0xF0000000u, 0) == 0xFFFFFFFFu,
           "the headroom saturates");
}

static void t_bar(void)
{
    expect(nm_bar_fill(0, 1000, 100) == 0, "nothing");
    expect(nm_bar_fill(500, 1000, 100) == 50, "half");
    expect(nm_bar_fill(2000, 1000, 100) == 100, "over the scale is full");
    /* Large rates lose low bits to stay in 32-bit arithmetic: a pixel. */
    expect(nm_bar_fill(2700000, 5400000, 120) >= 59 &&
           nm_bar_fill(2700000, 5400000, 120) <= 60, "large rates");
    expect(nm_bar_fill(5, 0, 100) == 0, "no scale");
}

int main(void)
{
    printf("NetMeter arithmetic\n");
    t_rates();
    t_totals();
    t_delta();
    t_rate();
    t_scale();
    t_bar();
    printf("netmeter_fmt checks=%lu failures=%lu\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
