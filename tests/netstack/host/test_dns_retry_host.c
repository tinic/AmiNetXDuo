/*
 * AmiNetXDuo -- whether a name lookup stops when it is told to.
 *
 * A gethostbyname() against a name server that answers nothing used to hold the
 * calling task for minutes with no way out: NetX Duo's DNS client took the
 * thirty-second timeout as a PER-QUERY wait and spent it NX_DNS_MAX_RETRIES
 * times over every configured server, doubling between rounds, with its mutex
 * held for the whole of it.  Nothing sampled the break signal, so Ctrl-C did
 * not arrive, and the second of two concurrent lookups queued behind the first.
 *
 * The ladder is ours now (src/netstack/netstack_retry.c) and this drives it
 * with the DNS client replaced by a table of scripted answers, because the
 * three cases that matter cannot be produced on demand against a real server:
 *
 *   1. a black hole      -- every attempt uses its whole wait and nothing comes
 *                           back.  The lookup must end inside its budget rather
 *                           than at a multiple of it.
 *   2. Ctrl-C            -- the break signal arrives mid-lookup.  It must be
 *                           acted on within one rung, not at the end.
 *   3. a fast refusal    -- a server answers immediately without an address.
 *                           Re-asking cannot change that, so the ladder must
 *                           stop rather than spend the rest of the budget.
 *
 * Real, compiled into this binary: src/netstack/netstack_retry.c, the whole of
 * it.  Stubbed: the clock, which is a variable the scripted attempts advance,
 * so a thirty-second lookup costs no wall time and the intervals are exact
 * rather than approximately right.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_retry.h"

#include <stdio.h>
#include <string.h>


/* ---------------------------------------------------------- the clock ----- */

static ULONG h_now;

ULONG _tx_time_get(VOID)
{
    return h_now;
}


/* ------------------------------------------------------------- harness ---- */

static unsigned long h_checks;
static unsigned long h_failures;

static void h_check(int ok, const char *what)
{
    h_checks++;

    if (!ok)
    {
        h_failures++;
        printf("FAIL %s\n", what);
    }
}


/* ------------------------------------------------------- scripted queries -- */

/*
 * One name server that may be black-holed, refusing, or answering, and that
 * charges the clock for the time it takes.  `answer_after` is when the address
 * turns up; until then it behaves as `silent` says.
 */
typedef struct
{
    ULONG servers;          /* how many the attempt walks                    */
    ULONG reply_delay;      /* ticks before each server answers; 0 = at once */
    BOOL  refuses;          /* answers, but with no address                  */
    ULONG answer_at;        /* clock reading from which the lookup succeeds  */

    ULONG attempts;         /* counted                                       */
    ULONG waits[64];        /* the wait each attempt was given               */
} HostServer;

static AmiNetAskResult h_ask(VOID *arg, ULONG wait)
{
    HostServer *s = (HostServer *)arg;
    ULONG       i;

    if (s->attempts < (ULONG)(sizeof(s->waits) / sizeof(s->waits[0])))
        s->waits[s->attempts] = wait;
    s->attempts++;

    for (i = 0; i < s->servers; i++)
    {
        /* A server that never replies burns the whole wait; one that does
           charges only the round trip. */
        h_now += (s->reply_delay != 0UL && s->reply_delay < wait)
                     ? s->reply_delay : wait;

        if (h_now >= s->answer_at && s->answer_at != 0UL)
            return AMI_NET_ASK_ANSWERED;
    }

    if (s->refuses)
        return AMI_NET_ASK_REFUSED;

    return AMI_NET_ASK_SILENT;
}

/* Ctrl-C at a fixed moment on the simulated clock. */
static ULONG h_break_at;

static BOOL h_break(VOID *arg)
{
    (VOID)arg;

    return (BOOL)(h_break_at != 0UL && h_now >= h_break_at);
}


/* ---------------------------------------------------------------- cases --- */

#define H_SECOND    ((ULONG)NX_IP_PERIODIC_RATE)

/*
 * A black hole.  Before this change the DNS client spent 5 * (30 + 60 + 64)
 * seconds on the thirty the caller asked for; the requirement is simply that
 * the budget is the budget.
 */
static void h_case_blackhole(void)
{
    HostServer         s;
    AmiNetLadderResult done;
    ULONG              budget = 30UL * H_SECOND;
    ULONG              i;

    memset(&s, 0, sizeof(s));
    s.servers = 5;

    h_now      = 1000;
    h_break_at = 0;

    done = ami_net_ask_until(h_ask, &s, budget, h_break, NULL);

    h_check(done == AMI_NET_LADDER_SILENT,
            "a black hole ends as silence, not as no-such-host");

    /*
     * One attempt may overshoot, because an attempt walks every server and is
     * only charged what it took afterwards.  Five servers at the two-second
     * ceiling is the worst overshoot there is.
     */
    h_check(h_now - 1000 <= budget + 5UL * AMI_NET_ASK_CEILING,
            "a black-holed lookup ends inside its budget");

    h_check(s.attempts > 1, "a black hole is retried at all");

    /* The ladder doubles and then stops doubling. */
    h_check(s.waits[0] == AMI_NET_ASK_FIRST, "the first query waits one rung");

    for (i = 0; i < s.attempts && i < 64; i++)
        h_check(s.waits[i] <= AMI_NET_ASK_CEILING,
                "no query waits longer than the ceiling");
}

/* Ctrl-C.  The break must be acted on within one rung of arriving. */
static void h_case_break(void)
{
    HostServer         s;
    AmiNetLadderResult done;

    memset(&s, 0, sizeof(s));
    s.servers = 2;

    h_now      = 0;
    h_break_at = 5UL * H_SECOND;

    done = ami_net_ask_until(h_ask, &s, 30UL * H_SECOND, h_break, NULL);

    h_check(done == AMI_NET_LADDER_ABORTED, "Ctrl-C ends the lookup");

    /*
     * The break is sampled between queries and never during one, so the worst
     * case is one attempt -- a query to each configured server at the ceiling.
     * With two servers that is four seconds, against the minutes it was.
     */
    h_check(h_now <= h_break_at + s.servers * AMI_NET_ASK_CEILING,
            "Ctrl-C is acted on within one attempt of arriving");
}

/* A break that is already set when the call is made must not start a query. */
static void h_case_break_first(void)
{
    HostServer         s;
    AmiNetLadderResult done;

    memset(&s, 0, sizeof(s));
    s.servers = 1;

    /* Set before the call is made. */
    h_now      = 1;
    h_break_at = 1;

    done = ami_net_ask_until(h_ask, &s, 30UL * H_SECOND, h_break, NULL);

    h_check(done == AMI_NET_LADDER_ABORTED, "a pending break ends it at once");
    h_check(s.attempts == 0, "a pending break sends no query");
}

/*
 * A name server that answers immediately without an address -- what NXDOMAIN
 * looks like from up here, since a blocking query in addons/dns folds every
 * per-server failure into one status before returning.  Re-asking cannot
 * change the answer, so a mistyped host name must not cost the whole budget.
 */
static void h_case_fast_refusal(void)
{
    HostServer         s;
    AmiNetLadderResult done;

    memset(&s, 0, sizeof(s));
    s.servers     = 1;
    s.reply_delay = 1;      /* a tick, well inside the first rung */

    h_now      = 0;
    h_break_at = 0;

    done = ami_net_ask_until(h_ask, &s, 30UL * H_SECOND, h_break, NULL);

    h_check(done == AMI_NET_LADDER_REFUSED,
            "a server that answers is not asked again");
    h_check(s.attempts == 1, "a mistyped name costs one query");
    h_check(h_now < H_SECOND, "a mistyped name does not cost the budget");
}

/* An address that turns up on the second query. */
static void h_case_answer(void)
{
    HostServer         s;
    AmiNetLadderResult done;

    memset(&s, 0, sizeof(s));
    s.servers   = 1;
    s.answer_at = 2UL * H_SECOND;

    h_now      = 0;
    h_break_at = 0;

    done = ami_net_ask_until(h_ask, &s, 30UL * H_SECOND, h_break, NULL);

    h_check(done == AMI_NET_LADDER_ANSWERED, "a late answer is still an answer");
    h_check(s.attempts >= 2, "the retry is what found it");
}

/*
 * The loser of a race for the DNS client's mutex.  addons/dns waits the
 * caller's timeout for it and then fails, which reads here as an attempt that
 * used its whole wait and came back with nothing -- silence.  It must stay
 * silence all the way out, because "somebody else is resolving" mapped onto
 * "no such host" is what told the second of two programs that the name does
 * not exist.
 */
static void h_case_contention(void)
{
    HostServer         s;
    AmiNetLadderResult done;

    memset(&s, 0, sizeof(s));
    s.servers = 1;

    h_now      = 0;
    h_break_at = 0;

    done = ami_net_ask_until(h_ask, &s, 4UL * H_SECOND, h_break, NULL);

    h_check(done == AMI_NET_LADDER_SILENT,
            "losing the DNS mutex is silence, not no-such-host");
}

/* A caller that asked not to wait still gets its one look at the cache. */
static void h_case_no_wait(void)
{
    HostServer         s;
    AmiNetLadderResult done;

    memset(&s, 0, sizeof(s));
    s.servers   = 1;
    s.answer_at = 0;

    h_now      = 0;
    h_break_at = 0;

    done = ami_net_ask_until(h_ask, &s, 0UL, h_break, NULL);

    h_check(s.attempts == 1, "a zero budget is one query, not none");
    h_check(done != AMI_NET_LADDER_ANSWERED, "and it did not find anything");
}


int main(void)
{
    h_case_blackhole();
    h_case_break();
    h_case_break_first();
    h_case_fast_refusal();
    h_case_answer();
    h_case_contention();
    h_case_no_wait();

    printf("%lu checks, %lu failures\n", h_checks, h_failures);

    return (h_failures == 0) ? 0 : 1;
}
