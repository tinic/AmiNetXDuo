/*
 * AmiNetXDuo, the search-list walk inside one budget.
 * SPDX-License-Identifier: MIT
 */

#include "netstack_dns_search.h"

#include <stdio.h>
#include <string.h>

#define H_SECOND    ((ULONG)NX_IP_PERIODIC_RATE)
#define H_MAX_CALLS 32
#define H_ANSWER    1L

static ULONG h_now;

ULONG _tx_time_get(VOID)
{
    return h_now;
}

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

/* The script: what the bare name and each suffix's lookup do. */
static struct
{
    const char *suffix[8];
    UWORD       suffixes;

    LONG        bare_result;        /* once() on the name as given          */
    LONG        suffix_result;      /* once() on any qualified name         */
    LONG        result_at[8];       /* per suffix override, 0 = none,
                                       H_ANSWER = an address                */
    ULONG       cost;               /* ticks a once() takes, capped by ticks */
    BOOL        overrun;            /* take `cost` even past the ticks      */
    const char *hosts;              /* the one name local() knows           */

    ULONG       break_after;        /* give_up() TRUE once this many once()s */

    ULONG       once_calls;
    ULONG       once_ticks[H_MAX_CALLS];
    char        once_name[H_MAX_CALLS][AMI_DNS_NAME_MAX];
    ULONG       local_calls;
    ULONG       give_up_calls;
} h;

static void h_reset(UWORD suffixes)
{
    static const char *const names[7] = {
        "a.example", "b.example", "c.example", "d.example",
        "e.example", "f.example", "g.example"
    };
    UWORD i;

    memset(&h, 0, sizeof(h));
    for (i = 0; i < suffixes && i < 7; i++)
        h.suffix[i] = names[i];
    h.suffixes      = suffixes;
    h.bare_result   = AMI_NET_ERR_NONAME;
    h.suffix_result = AMI_NET_ERR_NONAME;
}

static LONG h_once(const char *name, VOID *out, ULONG ticks,
                   AmiNetGiveUpFn give_up, VOID *give_up_arg)
{
    ULONG n = h.once_calls++;
    ULONG take;
    UWORD i;

    (VOID)give_up;
    (VOID)give_up_arg;

    if (n < H_MAX_CALLS)
    {
        h.once_ticks[n] = ticks;
        strncpy(h.once_name[n], name, AMI_DNS_NAME_MAX - 1);
    }

    take = (h.overrun || h.cost < ticks) ? h.cost : ticks;
    h_now += take;

    if (n == 0)
        return h.bare_result;

    for (i = 0; i < h.suffixes; i++)
    {
        size_t len = strlen(h.suffix[i]);
        size_t nl  = strlen(name);

        if (nl > len && strcmp(name + nl - len, h.suffix[i]) == 0 &&
            h.result_at[i] != 0)
        {
            if (h.result_at[i] != H_ANSWER)
                return h.result_at[i];
            *(ULONG *)out = 0x0A000001UL;
            return AMI_NET_OK;
        }
    }

    return h.suffix_result;
}

static LONG h_local(const char *name, VOID *out)
{
    h.local_calls++;

    if (h.hosts != NULL && strcmp(name, h.hosts) == 0)
    {
        *(ULONG *)out = 0xC0A80001UL;
        return AMI_NET_OK;
    }

    return AMI_NET_ERR_NONAME;
}

static BOOL h_suffix_at(const char *name, UWORD at, char *qualified,
                        ULONG qualified_size, UWORD *count_out)
{
    if (count_out != NULL)
        *count_out = h.suffixes;

    if (at >= h.suffixes)
        return FALSE;

    snprintf(qualified, qualified_size, "%s.%s", name, h.suffix[at]);

    return TRUE;
}

static BOOL h_give_up(VOID *arg)
{
    (VOID)arg;

    h.give_up_calls++;

    return (BOOL)(h.break_after != 0UL && h.once_calls >= h.break_after);
}

static const AmiNsSearchOps h_ops = { h_once, h_local, h_suffix_at };

static LONG h_run(const char *name, ULONG budget, ULONG *addr)
{
    *addr = 0;

    return ami_ns_search(&h_ops, name, addr, budget, h_give_up, NULL);
}

/* An explicit 0 queries nothing, but still reads the hosts file. */
static void h_case_explicit_zero(void)
{
    ULONG addr;
    LONG  err;

    h_reset(3);
    h.bare_result = AMI_NET_ERR_TIMEOUT;   /* what once() says to 0 ticks */
    h_now = 1000;

    err = h_run("box", 0UL, &addr);

    h_check(err == AMI_NET_ERR_TIMEOUT, "zero: a miss is TIMEOUT");
    h_check(h.once_calls == 1 && h.once_ticks[0] == 0UL,
            "zero: one once() for the bare name, with 0 ticks");
    h_check(h.local_calls == 3, "zero: every suffix tried in hosts");
    h_check(h_now == 1000, "zero: no time passes");

    h_reset(3);
    h.bare_result = AMI_NET_ERR_TIMEOUT;
    h.hosts       = "box.b.example";

    err = h_run("box", 0UL, &addr);

    h_check(err == AMI_NET_OK && addr == 0xC0A80001UL,
            "zero: a hosts entry under a suffix answers");
    h_check(h.once_calls == 1, "zero: still no qualified query");

    h_reset(3);
    h.bare_result = AMI_NET_ERR_STATE;

    err = h_run("box", 0UL, &addr);

    h_check(err == AMI_NET_ERR_STATE, "zero: resolver down stays STATE");
    h_check(h.local_calls == 3, "zero: and hosts is still read");
}

/* Every lookup burns what it was given; the walk still ends inside T. */
static void h_case_seven_full(void)
{
    ULONG addr;
    ULONG start = 50000;
    ULONG budget = 30UL * H_SECOND;
    LONG  err;

    h_reset(7);
    h.cost = 0xFFFFFFFFUL;              /* takes everything it is given */
    h_now  = start;

    err = h_run("box", budget, &addr);

    h_check(err == AMI_NET_ERR_NONAME, "seven: the bare name's NONAME");
    h_check(h_now - start <= budget, "seven: whole walk within T");
    h_check(h.once_calls == 1, "seven: no DNS once the budget is gone");
    h_check(h.local_calls == 7, "seven: hosts for every suffix");
}

/* Time shared out across the suffixes, through a tick-counter wrap. */
static void h_case_wrap(void)
{
    ULONG addr;
    ULONG start  = 0xFFFFFFF0UL;
    ULONG budget = 30UL * H_SECOND;
    LONG  err;
    ULONG i;
    int   shrinking = 1;

    h_reset(7);
    h.cost = 5UL * H_SECOND;
    h_now  = start;

    err = h_run("box", budget, &addr);

    h_check(err == AMI_NET_ERR_NONAME, "wrap: the bare name's NONAME");
    h_check(h_now < start, "wrap: the clock did wrap");
    h_check(h_now - start <= budget, "wrap: whole walk within T");
    h_check(h.once_calls == 6, "wrap: bare + five suffixes at 5 s each");
    h_check(h.local_calls == 2, "wrap: the last two hosts only");

    for (i = 0; i < h.once_calls && i < H_MAX_CALLS; i++)
        if (h.once_ticks[i] != budget - i * h.cost)
            shrinking = 0;
    h_check(shrinking, "wrap: each lookup gets the remainder");
}

/* A lookup that overruns its ticks does not make the next one negative. */
static void h_case_overrun(void)
{
    ULONG addr;
    ULONG budget = 3UL * H_SECOND;
    LONG  err;

    h_reset(4);
    h.cost    = 2UL * H_SECOND;
    h.overrun = TRUE;
    h_now     = 0xFFFFFF00UL;

    err = h_run("box", budget, &addr);

    h_check(err == AMI_NET_ERR_NONAME, "overrun: NONAME");
    h_check(h.once_calls == 2, "overrun: bare + one suffix, then spent");
    h_check(h.once_ticks[1] == 1UL * H_SECOND, "overrun: remainder given");
    h_check(h.local_calls == 3, "overrun: the rest hosts only");
}

/* The budget gone, a hosts entry under a later suffix still answers. */
static void h_case_hosts_after(void)
{
    ULONG addr;
    LONG  err;

    h_reset(7);
    h.cost  = 0xFFFFFFFFUL;
    h.hosts = "box.e.example";
    h_now   = 7;

    err = h_run("box", 10UL * H_SECOND, &addr);

    h_check(err == AMI_NET_OK && addr == 0xC0A80001UL,
            "hosts: answered after the budget");
    h_check(h.local_calls == 5, "hosts: stopped at the hit");
}

static void h_case_break(void)
{
    ULONG addr;
    LONG  err;

    h_reset(7);
    h.cost        = 1UL * H_SECOND;
    h.break_after = 3;
    h_now         = 0;

    err = h_run("box", 30UL * H_SECOND, &addr);

    h_check(err == AMI_NET_ERR_ABORTED, "break: Ctrl-C mid-walk is ABORTED");
    h_check(h.once_calls == 3, "break: no lookup after it");

    /* ABORTED from inside a suffix's lookup, not between them. */
    h_reset(4);
    h.cost         = 1UL * H_SECOND;
    h.result_at[1] = AMI_NET_ERR_ABORTED;

    err = h_run("box", 30UL * H_SECOND, &addr);

    h_check(err == AMI_NET_ERR_ABORTED, "break: a suffix's ABORTED kept");
    h_check(h.once_calls == 3, "break: and the walk stops there");

    /* Budget spent, hosts-only, Ctrl-C still honoured. */
    h_reset(4);
    h.cost        = 0xFFFFFFFFUL;
    h.break_after = 1;

    err = h_run("box", 5UL * H_SECOND, &addr);

    h_check(err == AMI_NET_ERR_ABORTED, "break: hosts-only walk aborts too");
    h_check(h.local_calls == 0, "break: before the first hosts lookup");
}

static void h_case_mapping(void)
{
    ULONG addr;
    LONG  err;

    /* A suffix's TIMEOUT ends the walk and reports the bare NONAME. */
    h_reset(4);
    h.cost          = 1UL * H_SECOND;
    h.suffix_result = AMI_NET_ERR_TIMEOUT;

    err = h_run("box", 30UL * H_SECOND, &addr);

    h_check(err == AMI_NET_ERR_NONAME, "map: first failure reported");
    h_check(h.once_calls == 2, "map: TIMEOUT under a suffix stops the walk");

    /* STATE walks, and is what comes back. */
    h_reset(4);
    h.bare_result   = AMI_NET_ERR_STATE;
    h.suffix_result = AMI_NET_ERR_STATE;

    err = h_run("box", 30UL * H_SECOND, &addr);

    h_check(err == AMI_NET_ERR_STATE, "map: STATE kept");
    h_check(h.once_calls == 5, "map: STATE walks every suffix");

    /* TIMEOUT with time given says nothing about the name: no walk. */
    h_reset(4);
    h.bare_result = AMI_NET_ERR_TIMEOUT;

    err = h_run("box", 30UL * H_SECOND, &addr);

    h_check(err == AMI_NET_ERR_TIMEOUT && h.once_calls == 1 &&
            h.local_calls == 0, "map: bare TIMEOUT returned, no walk");

    /* A dotted name is never suffixed. */
    h_reset(4);

    err = h_run("box.lan", 30UL * H_SECOND, &addr);

    h_check(err == AMI_NET_ERR_NONAME && h.once_calls == 1,
            "map: dotted name not walked");

    /* A suffix answers. */
    h_reset(4);
    h.result_at[2] = H_ANSWER;

    err = h_run("box", 30UL * H_SECOND, &addr);

    h_check(err == AMI_NET_OK && addr == 0x0A000001UL,
            "map: a suffix's answer returned");
    h_check(strcmp(h.once_name[3], "box.c.example") == 0,
            "map: suffixes in order");

    h_check(h_run("", 30UL, &addr) == AMI_NET_ERR_CONFIG,
            "map: empty name is CONFIG");
}

int main(void)
{
    h_case_explicit_zero();
    h_case_seven_full();
    h_case_wrap();
    h_case_overrun();
    h_case_hosts_after();
    h_case_break();
    h_case_mapping();

    printf("%lu checks, %lu failures\n", h_checks, h_failures);

    return (h_failures == 0) ? 0 : 1;
}
