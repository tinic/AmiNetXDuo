/*
 * The transmit watchdog's definition of progress, on the host.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>

#include "netdev_watchdog.h"

static int failures;

static void expect(const char *what, ULONG got, ULONG want)
{
    if (got == want)
    {
        printf("ok   %s = %lu\n", what, (unsigned long)got);
        return;
    }

    printf("FAIL %s: got %lu, want %lu\n", what,
           (unsigned long)got, (unsigned long)want);
    failures++;
}

int main(void)
{
    UWORD stall = 0;
    ULONG seen = 0;
    ULONG completed = 0;
    ULONG expired = 0;
    UWORD i;

    /* A saturated ring can stay nonempty indefinitely without being stuck. */
    for (i = 0; i < NETDEV_TX_STALL_BLANKS * 2; i++)
    {
        completed++;
        if (netdev_tx_watchdog_tick(&stall, &seen, TRUE, 2, completed))
            expired++;
    }
    expect("a moving full ring never expires", expired, 0);
    expect("progress leaves no accumulated stall", stall, 0);

    for (i = 1; i < NETDEV_TX_STALL_BLANKS; i++)
    {
        if (netdev_tx_watchdog_tick(&stall, &seen, TRUE, 2, completed))
            expired++;
    }
    expect("a quiet busy ring does not expire early", expired, 0);
    expect("the last quiet blank expires",
           netdev_tx_watchdog_tick(&stall, &seen, TRUE, 2, completed), TRUE);
    expect("expiry rearms the interval", stall, 0);

    stall = 17;
    expect("an empty ring is idle",
           netdev_tx_watchdog_tick(&stall, &seen, TRUE, 0, completed), FALSE);
    expect("idle clears the interval", stall, 0);

    stall = 17;
    expect("an offline unit is idle",
           netdev_tx_watchdog_tick(&stall, &seen, FALSE, 2, completed), FALSE);
    expect("offline clears the interval", stall, 0);

    /* Inequality rather than ordering makes ULONG wrap ordinary progress. */
    stall = 17;
    seen = (ULONG)~0UL;
    expect("completion-counter wrap is progress",
           netdev_tx_watchdog_tick(&stall, &seen, TRUE, 2, 0), FALSE);
    expect("wrap clears the interval", stall, 0);

    if (failures != 0)
    {
        printf("netdev_watchdog: %d failures\n", failures);
        return 1;
    }

    printf("netdev_watchdog: all ok\n");
    return 0;
}
