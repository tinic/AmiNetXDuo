/*
 * TCP_USER_TIMEOUT's milliseconds-to-ticks conversion, without an Amiga.
 *
 * SPDX-License-Identifier: MIT
 */

#include "opt_time.h"

#include <stdio.h>

#define AMIGA_TICKS_PER_SECOND 50UL

static int failures;

static void expect(const char *what, ULONG ms, ULONG want)
{
    ULONG got = bsd_ms_ticks(ms, AMIGA_TICKS_PER_SECOND);

    if (got != want)
    {
        printf("FAIL %s: got %lu, want %lu\n", what,
               (unsigned long)got, (unsigned long)want);
        failures++;
    }
    else
    {
        printf("ok   %s = %lu\n", what, (unsigned long)got);
    }
}

int main(void)
{
    expect("zero", 0UL, 0UL);
    expect("one millisecond", 1UL, 1UL);
    expect("one tick", 20UL, 1UL);
    expect("one tick plus a millisecond", 21UL, 2UL);
    expect("one second", 1000UL, 50UL);

    /* The old (ms * 50 + 999) expression first overflowed here. */
    expect("first overflowing numerator", 85899326UL, 4294967UL);

    /* setsockopt() accepts this value, so it must remain a day internally. */
    expect("one-day option ceiling", 86400000UL, 4320000UL);

    if (failures != 0)
    {
        printf("sockopt_time: %d failures\n", failures);
        return 1;
    }

    puts("sockopt_time: all ok");
    return 0;
}
