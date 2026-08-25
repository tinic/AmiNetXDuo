/*
 * The tests for src/tlslib/tls_expiry.c.  Two of its four rules exist only
 * because tls_time_monotonic() returns an uptime on a machine with no clock,
 * and TLS_CLOCK_FLOOR is the only thing telling the two kinds of stamp apart.
 *
 *   cc -std=c11 -Wall -Wextra -Isrc/tlslib \
 *      src/tlslib/test/test_tls_expiry.c src/tlslib/tls_expiry.c \
 *      -o test_tls_expiry
 *
 * SPDX-License-Identifier: MIT
 */

#include "tls_expiry.h"

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

/* A plausible wall-clock instant, comfortably above the floor. */
#define WALL    (TLS_CLOCK_FLOOR + 1000000UL)

/* ------------------------------------------------------ the ordinary case --- */

/* The comparison is `age > limit`, so an entry exactly at its lifetime is
   still good and one second past it is not. */
static void test_lifetime_boundary(void)
{
    CHECK(tls_expired(WALL, 300UL, WALL) == 0);          /* no age at all */
    CHECK(tls_expired(WALL, 300UL, WALL + 299UL) == 0);
    CHECK(tls_expired(WALL, 300UL, WALL + 300UL) == 0);  /* exactly at it */
    CHECK(tls_expired(WALL, 300UL, WALL + 301UL) == 1);
}

/* No hint means the ceiling applies, and so does a hint above it. */
static void test_ceiling(void)
{
    CHECK(tls_expired(WALL, 0UL, WALL + TLS_RESUME_MAX_AGE) == 0);
    CHECK(tls_expired(WALL, 0UL, WALL + TLS_RESUME_MAX_AGE + 1UL) == 1);

    /* Cloudflare's 64800 is under the ceiling and is used as given. */
    CHECK(tls_expired(WALL, 64800UL, WALL + 64800UL) == 0);
    CHECK(tls_expired(WALL, 64800UL, WALL + 64801UL) == 1);

    /* A year: capped, not believed. */
    CHECK(tls_expired(WALL, 31536000UL, WALL + TLS_RESUME_MAX_AGE) == 0);
    CHECK(tls_expired(WALL, 31536000UL, WALL + TLS_RESUME_MAX_AGE + 1UL) == 1);

    /* Exactly the ceiling as a hint is the ceiling either way. */
    CHECK(tls_expired(WALL, TLS_RESUME_MAX_AGE,
                      WALL + TLS_RESUME_MAX_AGE) == 0);
    CHECK(tls_expired(WALL, TLS_RESUME_MAX_AGE,
                      WALL + TLS_RESUME_MAX_AGE + 1UL) == 1);
}

/* ------------------------------------------------- machines with no clock --- */

/* Uptime stamps: a machine with no battery-backed clock counts from boot, so
   both numbers are small and the ageing rule is the same at those values. */
static void test_uptime_stamps(void)
{
    CHECK(tls_expired(100UL, 300UL, 200UL) == 0);
    CHECK(tls_expired(100UL, 300UL, 400UL) == 0);
    CHECK(tls_expired(100UL, 300UL, 401UL) == 1);

    /* Including from the first second of that boot. */
    CHECK(tls_expired(0UL, 300UL, 300UL) == 0);
    CHECK(tls_expired(0UL, 300UL, 301UL) == 1);
}

/* A clock stamp and an uptime are different kinds of number, and subtracting
   one from the other gives an age that means nothing.  Either way is expired. */
static void test_clock_kind_mismatch(void)
{
    /* Stored under a clock, asked about with an uptime. */
    CHECK(tls_expired(WALL, 300UL, 500UL) == 1);
    CHECK(tls_expired(WALL, 0UL, 500UL) == 1);

    /* Stored as an uptime, asked about with a clock.  This one otherwise
       produces an enormous age and reads as "expired" by accident rather than
       by rule, so it is checked from both sides. */
    CHECK(tls_expired(500UL, 300UL, WALL) == 1);
    CHECK(tls_expired(500UL, 0UL, WALL) == 1);

    /* The floor itself is a clock stamp, the second below it is not. */
    CHECK(tls_expired(TLS_CLOCK_FLOOR, 300UL, TLS_CLOCK_FLOOR) == 0);
    CHECK(tls_expired(TLS_CLOCK_FLOOR - 1UL, 300UL, TLS_CLOCK_FLOOR) == 1);
    CHECK(tls_expired(TLS_CLOCK_FLOOR, 300UL, TLS_CLOCK_FLOOR - 1UL) == 1);
}

/*
 * An uptime from a previous boot.  These assert the verdict, not the branch:
 * deleting `now < stamp` from tls_expired() passes every case below, because
 * the subtraction then wraps to a number larger than any limit.
 */
static void test_previous_boot(void)
{
    CHECK(tls_expired(5000UL, 300UL, 100UL) == 1);
    CHECK(tls_expired(5000UL, 0UL, 100UL) == 1);

    /* One second before the stamp is still a previous boot. */
    CHECK(tls_expired(5000UL, 300UL, 4999UL) == 1);

    /* And the same instant is not. */
    CHECK(tls_expired(5000UL, 300UL, 5000UL) == 0);

    /* The wall-clock side has the same rule: a clock set backwards. */
    CHECK(tls_expired(WALL, 300UL, WALL - 1UL) == 1);
}

/*
 * Far-apart stamps stay a plain subtraction.  Nothing here must depend on the
 * width of `unsigned long`: it is 32 bits on m68k and 64 on the host that runs
 * this, so the values below are far from either end.
 */
static void test_far_apart(void)
{
    /* A very old clock stamp against a recent one: expired, by age. */
    CHECK(tls_expired(TLS_CLOCK_FLOOR, 0UL, WALL) == 1);

    /* A huge lifetime hint does not overflow the comparison, it is capped. */
    CHECK(tls_expired(WALL, 0xFFFFFFFFUL, WALL + 10UL) == 0);
    CHECK(tls_expired(WALL, 0xFFFFFFFFUL,
                      WALL + TLS_RESUME_MAX_AGE + 1UL) == 1);

    /* A long uptime, still an uptime: below the floor and aged normally. */
    CHECK(tls_expired(1000000UL, 300UL, 1000200UL) == 0);
    CHECK(tls_expired(1000000UL, 300UL, 1000400UL) == 1);
}

int main(void)
{
    test_lifetime_boundary();
    test_ceiling();
    test_uptime_stamps();
    test_clock_kind_mismatch();
    test_previous_boot();
    test_far_apart();

    printf("\n%d checks, %d failure(s)\n", checks, failures);

    return failures == 0 ? 0 : 1;
}
