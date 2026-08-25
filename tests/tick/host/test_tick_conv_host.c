/*
 * AmiNetXDuo, the tick health counters' unit conversion, as arithmetic.
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

    expect("40 ms gap", tx_amiga_eclock_ms(PAL_PER_MS * 40UL, PAL_PER_MS), 40UL);

    expect("60 ms gap", tx_amiga_eclock_ms(PAL_PER_MS * 60UL, PAL_PER_MS), 60UL);
    expect("60 ms in ticks",
           tx_amiga_eclock_ms(PAL_PER_MS * 60UL, PAL_PER_MS) / TICK_MS, 3UL);

    expect("2 ms service", tx_amiga_eclock_us(PAL_PER_MS * 2UL, PAL_PER_MS),
           2000UL);

    expect("sub-ms gap in ms", tx_amiga_eclock_ms(PAL_PER_MS - 1UL, PAL_PER_MS),
           0UL);

    expect("5 s in us", tx_amiga_eclock_us(PAL_PER_MS * 5000UL, PAL_PER_MS),
           5000000UL);
    expect("10 s in us", tx_amiga_eclock_us(PAL_PER_MS * 10000UL, PAL_PER_MS),
           10000000UL);
    expect("10 s in ms", tx_amiga_eclock_ms(PAL_PER_MS * 10000UL, PAL_PER_MS),
           10000UL);

    /* A rate the port never managed to read, so nothing divides by zero. */
    expect("no rate, ms", tx_amiga_eclock_ms(PAL_PER_MS, 0UL), 0UL);
    expect("no rate, us", tx_amiga_eclock_us(PAL_PER_MS, 0UL), 0UL);

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
