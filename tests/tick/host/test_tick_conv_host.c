/*
 * AmiNetXDuo, the tick health counters' unit conversion, as arithmetic.
 *
 * netstat -s prints "timer wheel N ticks late, worst M" on one line and
 * "worst stall X ms, service Y us at the time" on the next, and the second
 * line is what says whether the first is one late wakeup or lateness that
 * piled up across several.  That only works if X is the gap in the units the
 * reader is dividing by, so the conversions are asserted here rather than
 * inferred from a guest's print.
 *
 * Real, from port/threadx-amiga/inc/tx_amiga.h: tx_amiga_eclock_ms(),
 * tx_amiga_eclock_us() and tx_amiga_uptime_ms(), the three places an E-Clock
 * reading becomes a number a human sees.  The tick task holds raw E-Clock
 * ticks and calls these, because a divide on the wakeup path costs a
 * 32-iteration shift-subtract on a 68000 fifty times a second.
 *
 * Every figure below is an exact multiple of the divisor.  The tick task's
 * eclock_per_ms is the rate divided by a thousand and truncates -- 709379
 * becomes 709, 0.05% fast -- and a test written in round PAL milliseconds
 * would be asserting that truncation rather than the conversion.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tx_api.h"
#include "tx_amiga.h"

#include <stdio.h>
#include <string.h>


/* PAL, as the tick task holds it: 709379 / 1000. */
#define PAL_PER_MS      709UL

/* TX_TIMER_TICKS_PER_SECOND is 50 in this port: a tick is 20 ms. */
#define TICK_MS         20UL


static int failures;

static void expect(const char *what, ULONG got, ULONG want)
{
    if (got != want)
    {
        printf("FAIL %s: got %lu, want %lu\n", what,
               (unsigned long) got, (unsigned long) want);
        failures++;
    }
    else
    {
        printf("ok   %s = %lu\n", what, (unsigned long) got);
    }
}


int main(void)
{
    TX_AMIGA_TICK_STATS t;

    /* A wakeup that arrived on time on the VBlank source: two frames, 40 ms. */
    expect("40 ms gap", tx_amiga_eclock_ms(PAL_PER_MS * 40UL, PAL_PER_MS), 40UL);

    /* The bring-up gap the lab guests report, three tick periods.  This is the
       pair netstat prints: a skew peak of 3 and a stall of 60 ms are the same
       event counted twice, and 60 / 20 == 3 is the check a reader makes. */
    expect("60 ms gap", tx_amiga_eclock_ms(PAL_PER_MS * 60UL, PAL_PER_MS), 60UL);
    expect("60 ms in ticks",
           tx_amiga_eclock_ms(PAL_PER_MS * 60UL, PAL_PER_MS) / TICK_MS, 3UL);

    /* Service costs, which are read in microseconds because the ordinary ones
       are three digits of them. */
    expect("2 ms service", tx_amiga_eclock_us(PAL_PER_MS * 2UL, PAL_PER_MS),
           2000UL);

    /* A gap shorter than a millisecond floors to 0 ms rather than rounding up:
       a wakeup that costs less than a millisecond is not a stall. */
    expect("sub-ms gap in ms", tx_amiga_eclock_ms(PAL_PER_MS - 1UL, PAL_PER_MS),
           0UL);

    /* ec * 1000 wraps a ULONG above about six seconds, so the microsecond
       conversion divides first past 4000000 E-Clock ticks.  Either side of
       that switch the answer is on the same scale; what is lost above it is
       the last three digits, and both figures here are exact. */
    expect("5 s in us", tx_amiga_eclock_us(PAL_PER_MS * 5000UL, PAL_PER_MS),
           5000000UL);
    expect("10 s in us", tx_amiga_eclock_us(PAL_PER_MS * 10000UL, PAL_PER_MS),
           10000000UL);
    expect("10 s in ms", tx_amiga_eclock_ms(PAL_PER_MS * 10000UL, PAL_PER_MS),
           10000UL);

    /* A rate the port never managed to read, so nothing divides by zero. */
    expect("no rate, ms", tx_amiga_eclock_ms(PAL_PER_MS, 0UL), 0UL);
    expect("no rate, us", tx_amiga_eclock_us(PAL_PER_MS, 0UL), 0UL);

    /* Uptime is one reading split two ways, and the part second is what
       tx_amiga_uptime_ms() converts.  It divides by the whole rate rather than
       by a per-millisecond figure, which is why this one is exact at a rate
       that is not a multiple of a thousand. */
    memset(&t, 0, sizeof(t));
    t.tx_amiga_tick_eclock_hz  =  700000UL;
    t.tx_amiga_tick_uptime_ms  =  4000UL;
    t.tx_amiga_tick_uptime_rem =  350000UL;
    expect("uptime 4.5 s", tx_amiga_uptime_ms(&t), 4500UL);

    t.tx_amiga_tick_eclock_hz  =  0UL;
    expect("uptime, no rate", tx_amiga_uptime_ms(&t), 4000UL);

    if (failures != 0)
    {
        printf("tick_conv: %d failures\n", failures);
        return 1;
    }

    printf("tick_conv: all ok\n");
    return 0;
}
