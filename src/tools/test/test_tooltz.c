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

    printf("tooltz: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
