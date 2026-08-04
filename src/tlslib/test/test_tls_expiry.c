/*
 * The tests for src/tlslib/tls_expiry.c, which decides whether a cached TLS
 * session may still be offered.
 *
 * WHY THIS IS A TEST AND NOT A REVIEW
 *
 *   The failure is silent and it is a key on disk.  A cached master secret
 *   lives in DEVS:Internet/tlssessions, and an expiry check that never fires
 *   leaves it offerable for as long as the file exists, which is what an
 *   attacker who takes the disk uses against captured traffic.  That is not a
 *   handshake failure, a log line or anything a user sees: it looks exactly
 *   like resumption working well.  The other direction is invisible too, an
 *   entry expired too eagerly costs one round trip and nothing else.
 *
 *   Two of the four rules exist only because the Amiga has machines with no
 *   battery-backed clock.  On those, tls_time_monotonic() counts seconds since
 *   boot instead of seconds since 1970, so a stamp is one kind of number or
 *   the other, and TLS_CLOCK_FLOOR is the only thing that tells them apart.
 *   Subtracting one kind from the other produces an age that means nothing,
 *   which is how "never expires" happened in the first place.
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

/*
 * A server hint below the ceiling is the limit, and the comparison is `age >
 * limit`, so an entry exactly at its lifetime is still good.  One second past
 * it is not.
 */
static void test_lifetime_boundary(void)
{
    CHECK(tls_expired(WALL, 300UL, WALL) == 0);          /* no age at all */
    CHECK(tls_expired(WALL, 300UL, WALL + 299UL) == 0);
    CHECK(tls_expired(WALL, 300UL, WALL + 300UL) == 0);  /* exactly at it */
    CHECK(tls_expired(WALL, 300UL, WALL + 301UL) == 1);
}

/*
 * No hint means the ceiling applies, and so does a hint above it: a server
 * that says "a year" is believed only as far as TLS_RESUME_MAX_AGE.
 */
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

/*
 * Uptime stamps.  A machine with no battery-backed clock counts from boot, so
 * both numbers are small, and ageing works exactly the same way down there.
 */
static void test_uptime_stamps(void)
{
    CHECK(tls_expired(100UL, 300UL, 200UL) == 0);
    CHECK(tls_expired(100UL, 300UL, 400UL) == 0);
    CHECK(tls_expired(100UL, 300UL, 401UL) == 1);

    /* Including from the first second of that boot. */
    CHECK(tls_expired(0UL, 300UL, 300UL) == 0);
    CHECK(tls_expired(0UL, 300UL, 301UL) == 1);
}

/*
 * The rule that matters most.  A stamp written under a real clock and an
 * uptime are different kinds of number, and subtracting one from the other
 * gives an age that means nothing.  Either direction is expired.
 */
static void test_clock_kind_mismatch(void)
{
    /* Stored under a clock, asked about with an uptime. */
    CHECK(tls_expired(WALL, 300UL, 500UL) == 1);
    CHECK(tls_expired(WALL, 0UL, 500UL) == 1);

    /* Stored as an uptime, asked about with a clock.  This is the one that
       would otherwise produce an enormous age and read as "expired" by
       accident rather than by rule, so it is checked from both sides. */
    CHECK(tls_expired(500UL, 300UL, WALL) == 1);
    CHECK(tls_expired(500UL, 0UL, WALL) == 1);

    /* The floor itself is a clock stamp, the second below it is not. */
    CHECK(tls_expired(TLS_CLOCK_FLOOR, 300UL, TLS_CLOCK_FLOOR) == 0);
    CHECK(tls_expired(TLS_CLOCK_FLOOR - 1UL, 300UL, TLS_CLOCK_FLOOR) == 1);
    CHECK(tls_expired(TLS_CLOCK_FLOOR, 300UL, TLS_CLOCK_FLOOR - 1UL) == 1);
}

/*
 * An uptime from a previous boot.  The machine rebooted, this boot's uptime is
 * lower than the stored one, and the entry is from a session that no longer
 * exists.
 *
 * These assert the verdict rather than the branch that produces it, and no
 * test can do better: deleting `now < stamp` from tls_expired() passes every
 * case below, because the subtraction then wraps to a number far larger than
 * any limit and the answer comes out the same.  The check is there to say why,
 * not to change what.  Do not read a green run here as covering it.
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
 * Far-apart stamps stay a plain subtraction.
 *
 * Nothing here may depend on the width of `unsigned long`: it is 32 bits on
 * m68k and 64 on the host running this, so a case built to wrap would be
 * asserting two different things in the two builds.  The values below are far
 * from either end.
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
