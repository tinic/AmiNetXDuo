/*
 * The tests for src/tools/toolbudget.c, how one connect timeout is divided
 * over the addresses a name resolved to.
 *
 * THE DEFECT THIS EXISTS TO END.  Every mistake this schedule can make is
 * invisible.  It produces no wrong answer, no error, nothing in a capture and
 * nothing in a log: the only symptom is a user waiting, and how long a
 * connect "should" take is not something anyone checks by eye.  So a change
 * that meant to stop giving up early on a slow address, and did, also turned
 * `telnet host` against a name with a blackholed AAAA and a refusing A from
 * ten seconds into roughly two hundred -- the stack's whole SYN schedule,
 * spent on a retry -- and every build was green.
 *
 * The cases below are that one, and the rest of toolbudget.h's six rules.
 * They are stated in seconds, because seconds are what the user experiences.
 *
 *   cc -std=c11 -Wall -Wextra -Isrc/tools \
 *      src/tools/test/test_toolbudget.c src/tools/toolbudget.c \
 *      -o test_toolbudget
 *
 * SPDX-License-Identifier: MIT
 */

#include "toolbudget.h"

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

/* ---------------------------------------------------------- the machine ---
 *
 * A walk of the whole list, driven exactly as tool_sock_connect_host() drives
 * it, against a table of what each address does.  Nothing is a socket: the
 * question is only how many seconds go by and which addresses are tried.
 */

#define MAX_ADDR    4

enum { A_REFUSE = 0, A_BLACKHOLE, A_ANSWERS_AT };

typedef struct Addr
{
    int             kind;
    unsigned long   after;      /* A_ANSWERS_AT: seconds to the handshake */
} Addr;

typedef struct Walk
{
    unsigned long   elapsed;    /* seconds the whole call took            */
    int             tries[MAX_ADDR * 2];
    int             ntries;
    int             connected;  /* -1, or the address that answered       */
} Walk;

/* One attempt.  Returns 1 when it connected, 0 when it failed fast, -1 when
   it ran out its ceiling; *cost is the seconds it took. */
static int attempt(const Addr *a, unsigned long secs, unsigned long *cost)
{
    /* A ceiling of 0 is the stack's own, documented at 191 s. */
    unsigned long ceiling = (secs == 0UL) ? 191UL : secs;

    if (a->kind == A_REFUSE)
    {
        *cost = 0UL;            /* an RST comes back in the first RTT */
        return 0;
    }

    if (a->kind == A_ANSWERS_AT && a->after <= ceiling)
    {
        *cost = a->after;
        return 1;
    }

    *cost = ceiling;
    return -1;
}

static void walk(Walk *w, const Addr *addr, unsigned long count,
                 unsigned long timeout)
{
    ToolBudget    b;
    unsigned long i;

    w->elapsed   = 0UL;
    w->ntries    = 0;
    w->connected = -1;

    tool_budget_init(&b, timeout, count);

    for (i = 0; i < count; i++)
    {
        unsigned long secs;
        unsigned long cost;
        int           cut;
        int           result;

        if (!tool_budget_first(&b, i, w->elapsed, &secs, &cut))
            break;

        w->tries[w->ntries++] = (int)i;

        result = attempt(&addr[i], secs, &cost);
        w->elapsed += cost;

        if (result == 1)
        {
            w->connected = (int)i;
            return;
        }

        tool_budget_done(&b, i, secs, (result < 0) ? 1 : 0, cut);
    }

    for (i = 0; i < count; i++)
    {
        unsigned long secs = tool_budget_again(&b, i, w->elapsed);
        unsigned long cost;
        int           result;

        if (secs == 0UL)
            continue;

        w->tries[w->ntries++] = (int)i;

        result = attempt(&addr[i], secs, &cost);
        w->elapsed += cost;

        if (result == 1)
        {
            w->connected = (int)i;
            return;
        }

        tool_budget_done(&b, i, secs, (result < 0) ? 1 : 0, 0);
    }
}

/* ------------------------------------------------------------ the cases --- */

/* The one from the backlog, and the reason this file exists. */
static void test_telnet_no_timeout(void)
{
    /* A dual-stack name: the AAAA goes nowhere, the A answers with a reset.
       `telnet host` sets no timeout. */
    Addr addr[2] = { { A_BLACKHOLE, 0UL }, { A_REFUSE, 0UL } };
    Walk w;

    printf("no timeout, blackholed AAAA then a refusing A\n");

    walk(&w, addr, 2UL, 0UL);

    /* Ten seconds on the AAAA, an immediate reset from the A, done.  This
       used to be about 200: the AAAA was retried with the stack's whole SYN
       schedule because "no timeout" was read as "unlimited budget left". */
    CHECK(w.elapsed == 10UL);
    CHECK(w.connected == -1);
    CHECK(w.ntries == 2);
    CHECK(w.tries[0] == 0 && w.tries[1] == 1);
}

/* The same shape with a timeout, where the retry is what the caller asked
   for and is still inside the number. */
static void test_retry_inside_the_timeout(void)
{
    /* The AAAA is slow rather than dead: it completes at 18 s, past the 10 s
       first trial.  The A refuses at once. */
    Addr addr[2] = { { A_ANSWERS_AT, 18UL }, { A_REFUSE, 0UL } };
    Walk w;

    printf("TIMEOUT=30, a slow AAAA and a refusing A\n");

    walk(&w, addr, 2UL, 30UL);

    /* 10 s cut short, 0 on the reset, then 18 of the remaining 20. */
    CHECK(w.connected == 0);
    CHECK(w.elapsed == 28UL);
    CHECK(w.ntries == 3);
    CHECK(w.tries[2] == 0);         /* the retry is the address cut short */
}

/* Rule 1: the number bounds the call, not each address in it. */
static void test_total_is_the_bound(void)
{
    Addr addr[3] = { { A_BLACKHOLE, 0UL },
                     { A_BLACKHOLE, 0UL },
                     { A_BLACKHOLE, 0UL } };
    Walk w;

    printf("TIMEOUT bounds the call\n");

    walk(&w, addr, 3UL, 30UL);
    CHECK(w.connected == -1);
    CHECK(w.elapsed == 30UL);       /* 10 + 10 + 10, and no second round */

    walk(&w, addr, 3UL, 25UL);
    CHECK(w.elapsed == 25UL);       /* 10 + 10 + the 5 that were left */

    walk(&w, addr, 3UL, 7UL);
    CHECK(w.elapsed == 7UL);        /* shorter than one trial: still 7 */
    CHECK(w.ntries == 1);

    walk(&w, addr, 2UL, 30UL);
    CHECK(w.elapsed == 30UL);       /* 10 cut short, then the other 20 */
    CHECK(w.ntries == 2);           /* nothing is left to retry with   */
}

/* Rule 5: a second attempt is never offered a ceiling it already had. */
static void test_retry_must_be_longer(void)
{
    ToolBudget    b;
    unsigned long secs;
    int           cut;

    printf("a retry is only worth making when it is longer\n");

    /* 15 s total: 10 goes on the first address, so 5 is left.  A fresh
       socket repeating the same SYN schedule for 5 s can only fail the way
       the 10 s attempt already did. */
    tool_budget_init(&b, 15UL, 2UL);
    CHECK(tool_budget_first(&b, 0UL, 0UL, &secs, &cut) == 1);
    CHECK(secs == 10UL && cut == 1);
    tool_budget_done(&b, 0UL, secs, 1, cut);
    CHECK(tool_budget_again(&b, 0UL, 10UL) == 0UL);

    /* 30 s total leaves 20, which is longer, so it is worth making. */
    tool_budget_init(&b, 30UL, 2UL);
    CHECK(tool_budget_first(&b, 0UL, 0UL, &secs, &cut) == 1);
    tool_budget_done(&b, 0UL, secs, 1, cut);
    CHECK(tool_budget_again(&b, 0UL, 10UL) == 20UL);

    /* And only for an address that was cut short.  The last address in the
       list got the whole remainder, so there is nothing more to give it. */
    CHECK(tool_budget_again(&b, 1UL, 10UL) == 0UL);
}

/* Rule 2 and rule 3: the cap applies only while another address is untried,
   and the last address gets what the caps left behind. */
static void test_last_address_gets_the_rest(void)
{
    ToolBudget    b;
    unsigned long secs;
    int           cut;

    printf("the cap, and who gets the remainder\n");

    tool_budget_init(&b, 30UL, 2UL);

    CHECK(tool_budget_first(&b, 0UL, 0UL, &secs, &cut) == 1);
    CHECK(secs == 10UL && cut == 1);

    CHECK(tool_budget_first(&b, 1UL, 10UL, &secs, &cut) == 1);
    CHECK(secs == 20UL && cut == 0);

    /* One address is the last address: no cap at all. */
    tool_budget_init(&b, 30UL, 1UL);
    CHECK(tool_budget_first(&b, 0UL, 0UL, &secs, &cut) == 1);
    CHECK(secs == 30UL && cut == 0);

    /* No timeout: the caps still apply, and the last gets the stack's own
       ceiling, which is what a plain connect() would have cost. */
    tool_budget_init(&b, 0UL, 2UL);
    CHECK(tool_budget_first(&b, 0UL, 0UL, &secs, &cut) == 1);
    CHECK(secs == 10UL && cut == 1);
    CHECK(tool_budget_first(&b, 1UL, 10UL, &secs, &cut) == 1);
    CHECK(secs == 0UL && cut == 0);
}

/* The budget is gone: the walk stops rather than starting an attempt that
   the caller's number has no room for. */
static void test_exhausted(void)
{
    ToolBudget    b;
    unsigned long secs;
    int           cut;

    printf("an exhausted budget stops the walk\n");

    tool_budget_init(&b, 10UL, 3UL);
    CHECK(tool_budget_first(&b, 0UL, 0UL, &secs, &cut) == 1);
    CHECK(secs == 10UL && cut == 0);     /* not capped: 10 is all there is */
    tool_budget_done(&b, 0UL, secs, 1, cut);

    CHECK(tool_budget_first(&b, 1UL, 10UL, &secs, &cut) == 0);

    /* Past the end of the list is also a stop. */
    tool_budget_init(&b, 30UL, 2UL);
    CHECK(tool_budget_first(&b, 2UL, 0UL, &secs, &cut) == 0);
}

/*
 * The floor under the clock.  ami_millis() answers 0 for the whole run on a
 * machine where timer.device did not open, and a schedule that believed it
 * would hand the full timeout out on every address.
 */
static void test_without_a_clock(void)
{
    ToolBudget    b;
    unsigned long secs;
    int           cut;

    printf("a machine with no clock\n");

    tool_budget_init(&b, 30UL, 2UL);

    /* Every elapsed reading below is 0, as it would be with no timer. */
    CHECK(tool_budget_first(&b, 0UL, 0UL, &secs, &cut) == 1);
    CHECK(secs == 10UL);
    tool_budget_done(&b, 0UL, secs, 1, cut);

    /* The timed-out attempt is still known to have cost its ceiling. */
    CHECK(tool_budget_left(&b, 0UL) == 20UL);
    CHECK(tool_budget_first(&b, 1UL, 0UL, &secs, &cut) == 1);
    CHECK(secs == 20UL);
    tool_budget_done(&b, 1UL, secs, 0, cut);      /* refused, cost nothing */

    /* And the retry draws from what the floor says is left, not from 30. */
    CHECK(tool_budget_again(&b, 0UL, 0UL) == 20UL);
}

int main(void)
{
    test_telnet_no_timeout();
    test_retry_inside_the_timeout();
    test_total_is_the_bound();
    test_retry_must_be_longer();
    test_last_address_gets_the_rest();
    test_exhausted();
    test_without_a_clock();

    printf("%d checks, %d failures\n", checks, failures);

    return (failures == 0) ? 0 : 1;
}
