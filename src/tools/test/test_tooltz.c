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
        const unsigned long isr  = 1522108800UL;   /* 2026-03-27 00:00 UTC */

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
        /* tzdata footers (2026c): Israel's /26, Greenland's /-1, Dublin's
           negative DST, Chile's /24, Lord Howe's half-hour DST. */
        CHECK(tooltz_posix_west("IST-2IDT,M3.4.4/26,M10.5.0", isr - 1UL, &w)
              && w == -7200);
        CHECK(tooltz_posix_west("IST-2IDT,M3.4.4/26,M10.5.0", isr, &w)
              && w == -10800);
        CHECK(tooltz_posix_west("<-02>2<-01>,M3.5.0/-1,M10.5.0/0", mar1 - 1UL,
                                &w) && w == 7200);
        CHECK(tooltz_posix_west("<-02>2<-01>,M3.5.0/-1,M10.5.0/0", mar1, &w)
              && w == 3600);
        CHECK(tooltz_posix_west("IST-1GMT0,M10.5.0,M3.5.0/1", jan, &w)
              && w == 0);
        CHECK(tooltz_posix_west("IST-1GMT0,M10.5.0,M3.5.0/1", jul, &w)
              && w == -3600);
        CHECK(tooltz_posix_west("<-04>4<-03>,M9.1.6/24,M4.1.6/24", jan, &w)
              && w == 3 * 3600);
        CHECK(tooltz_posix_west("<-04>4<-03>,M9.1.6/24,M4.1.6/24", jul, &w)
              && w == 4 * 3600);
        CHECK(tooltz_posix_west("<+1030>-10:30<+11>-11,M10.1.0,M4.1.0", jan,
                                &w) && w == -11 * 3600);
        CHECK(tooltz_posix_west("<+1030>-10:30<+11>-11,M10.1.0,M4.1.0", jul,
                                &w) && w == -37800);
        CHECK(tooltz_posix_west("<+1245>-12:45<+1345>,M9.5.0/2:45,M4.1.0/3:45",
                                jan, &w) && w == -49500);
        CHECK(tooltz_posix_west("NST3:30NDT,M3.2.0,M11.1.0", jul, &w)
              && w == 9000);
        CHECK(tooltz_posix_west("EST5EDT,0/0,J365/25", jan, &w)
              && w == 4 * 3600);
        CHECK(tooltz_posix_west("EST5EDT,0/0,J365/25", jul, &w)
              && w == 4 * 3600);
        /* SetDST forms: half-hour and 45-minute zones, angle names. */
        CHECK(tooltz_posix_west("CST-9:30CST", jan, &w) && w == -37800);
        CHECK(tooltz_posix_west("CST-9:30CST-10:30", jan, &w) && w == -37800);
        CHECK(tooltz_posix_west("CST-9:30", jul, &w) && w == -34200);
        CHECK(tooltz_posix_west("<+0545>-5:45", jul, &w) && w == -20700);
        CHECK(tooltz_posix_west("NPT-5:45:0", jul, &w) && w == -20700);
        CHECK(tooltz_posix_west("<+1245>-12:45<+1345>-13:45", jan, &w)
              && w == -49500);
        CHECK(tooltz_posix_west("JST-9", jul, &w) && w == -9 * 3600);
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
