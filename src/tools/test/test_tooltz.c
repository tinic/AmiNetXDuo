/*
 * The tests for src/tools/tooltz.c: the zone the Locale editor shows, and
 * the local time sntp sets from it.  The loc_GMTOffset values are the ones
 * locale.library 40 and 47 reported in Amiberry (#362).
 *
 * SPDX-License-Identifier: MIT
 */

#include "tooltz.h"

#include <stdio.h>

static int failures;
static int checks;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                    \
    } while (0)

/* 2026-10-30 07:00:00 UTC, in seconds since 1978-01-01. */
#define UTC_0700 1540969200UL

int main(void)
{
    /* Never saved: library -300, editor shows United States, UTC-5. */
    CHECK(tooltz_minutes_west(-300, 0) == 300);
    CHECK(tooltz_local_secs(UTC_0700, tooltz_minutes_west(-300, 0))
          == UTC_0700 - 5UL * 3600UL);

    /* Saved east: Central Europe is -60, UTC+1. */
    CHECK(tooltz_minutes_west(-60, 1) == -60);
    CHECK(tooltz_local_secs(UTC_0700, tooltz_minutes_west(-60, 1))
          == UTC_0700 + 3600UL);

    /* Saved west: the editor's default saved unchanged is 300, UTC-5. */
    CHECK(tooltz_minutes_west(300, 1) == 300);
    CHECK(tooltz_local_secs(UTC_0700, 300) == UTC_0700 - 5UL * 3600UL);

    /* Saved Greenwich, and half-hour zones. */
    CHECK(tooltz_local_secs(UTC_0700, tooltz_minutes_west(0, 1)) == UTC_0700);
    CHECK(tooltz_local_secs(UTC_0700, -330) == UTC_0700 + 330UL * 60UL);

    /* Outside 32 bits: the UTC value stays. */
    CHECK(tooltz_local_secs(60UL, 300) == 60UL);
    CHECK(tooltz_local_secs(0xffffff00UL, -60) == 0xffffff00UL);

    /* ENV:TZONE.  Amiga-epoch instants: 2026-07-01 12:00 UTC (summer),
       2026-01-15 12:00 UTC (winter), 2026-11-07 12:00 UTC (#475), and both
       sides of the EU changeovers of 2026 (29 March and 25 October, 01:00
       UTC). */
    {
        long w = 12345;
        const unsigned long jul  = 1530446400UL;
        const unsigned long jan  = 1516017600UL;
        const unsigned long mar1 = 1522285200UL;   /* 2026-03-29 01:00 UTC */
        const unsigned long oct1 = 1540429200UL;   /* 2026-10-25 01:00 UTC */
        const unsigned long nov  = 1541592000UL;   /* 2026-11-07 12:00 UTC */

        /* No rules: SetDST writes the DST name only while it is in
           effect, so the DST offset holds in any month. */
        CHECK(tooltz_posix_west("GMT0BST", jul, &w) && w == -3600);
        CHECK(tooltz_posix_west("GMT0BST", jan, &w) && w == -3600);
        CHECK(tooltz_posix_west("CET-1CEST", jul, &w) && w == -7200);
        CHECK(tooltz_posix_west("CET-1CEST", jan, &w) && w == -7200);
        CHECK(tooltz_posix_west("EST-10EST", nov, &w) && w == -11 * 3600);
        CHECK(tooltz_posix_west("EST-10", nov, &w) && w == -10 * 3600);
        CHECK(tooltz_posix_west("CET-1CEST,M3.5.0/2,M10.5.0/3", jul, &w)
              && w == -7200);
        CHECK(tooltz_posix_west("CET-1CEST,M3.5.0/2,M10.5.0/3", mar1 - 1UL,
                                &w) && w == -3600);
        CHECK(tooltz_posix_west("CET-1CEST,M3.5.0/2,M10.5.0/3", mar1, &w)
              && w == -7200);
        CHECK(tooltz_posix_west("CET-1CEST,M3.5.0/2,M10.5.0/3", oct1, &w)
              && w == -3600);
        CHECK(tooltz_posix_west("CET-1CEST,M3.5.0/2,M10.5.0/3", oct1 - 1UL,
                                &w) && w == -7200);
        CHECK(tooltz_posix_west("EST5EDT", jul, &w) && w == 4 * 3600);
        CHECK(tooltz_posix_west("EST5EDT", jan, &w) && w == 4 * 3600);
        CHECK(tooltz_posix_west("EST4EDT3", jan, &w) && w == 3 * 3600);
        /* Southern hemisphere: summer spans New Year. */
        CHECK(tooltz_posix_west("AEST-10AEDT,M10.1.0,M4.1.0/3", jan, &w)
              && w == -11 * 3600);
        CHECK(tooltz_posix_west("AEST-10AEDT,M10.1.0,M4.1.0/3", jul, &w)
              && w == -10 * 3600);
        /* No DST, half hours, quoted names, Julian rules. */
        CHECK(tooltz_posix_west("UTC0", jul, &w) && w == 0);
        CHECK(tooltz_posix_west("IST-5:30", jul, &w) && w == -19800);
        CHECK(tooltz_posix_west("<+04>-4", jul, &w) && w == -14400);
        CHECK(tooltz_posix_west("EST5EDT,J60,J300", jul, &w) && w == 4 * 3600);
        /* Not a time zone: refused, *seconds_west untouched. */
        w = 777;
        CHECK(!tooltz_posix_west("", jul, &w));
        CHECK(!tooltz_posix_west("GMT", jul, &w));
        CHECK(!tooltz_posix_west("Europe/London", jul, &w));
        CHECK(!tooltz_posix_west(":Europe/London", jul, &w));
        CHECK(!tooltz_posix_west("GMT0BST,M13.5.0,M10.5.0", jul, &w));
        CHECK(!tooltz_posix_west("GMT99", jul, &w));
        CHECK(!tooltz_posix_west("XXX1193047", jul, &w));
        CHECK(w == 777);
    }

    printf("tooltz: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
