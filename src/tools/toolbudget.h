/*
 * toolbudget, how one connect timeout is divided over the addresses a name
 * resolved to. Plain C with no AmigaOS types, so src/tools/test/test_toolbudget.c
 * can compile it natively and assert on the ceilings.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_TOOLBUDGET_H
#define AMINETXDUO_TOOLBUDGET_H

/* How long one address gets while another is still untried. Ten seconds spans
   four SYNs on this stack -- 0, 1, 3 and 7 s. It is not a race delay. */
#define TOOL_BUDGET_TRY_SECS    10UL

typedef struct ToolBudget
{
    unsigned long total;        /* the caller's seconds, 0 for the stack's  */
    unsigned long count;        /* addresses the name resolved to           */
    unsigned long known;        /* seconds known spent without a clock      */
    unsigned long deferred;     /* bit per address that was cut short       */
} ToolBudget;

void tool_budget_init(ToolBudget *b, unsigned long total,
                      unsigned long count);

/*
 * Seconds of the caller's timeout not yet spent; `elapsed` is what the clock
 * says. ZERO IS BOTH ANSWERS: with no caller timeout there is nothing to
 * divide, and zero is what tool_sock_connect_timed() reads as the stack's own.
 */
unsigned long tool_budget_left(const ToolBudget *b, unsigned long elapsed);

/*
 * The ceiling for the first attempt at address `i`. 0 when the budget is gone
 * and the walk must stop, 1 otherwise. *secs is the ceiling, 0 meaning the
 * stack's own; *cut is 1 when it was shortened to leave room for later ones.
 */
int tool_budget_first(const ToolBudget *b, unsigned long i,
                      unsigned long elapsed, unsigned long *secs, int *cut);

/*
 * What an attempt cost. Only a timed-out attempt is counted, at its whole
 * ceiling. This is a floor under the clock: ami_millis() answers 0 where
 * timer.device did not open, which would make every attempt look free.
 */
void tool_budget_done(ToolBudget *b, unsigned long i, unsigned long secs,
                      int timed_out, int cut);

/* The ceiling for a second attempt at address `i`, 0 for "do not". */
unsigned long tool_budget_again(const ToolBudget *b, unsigned long i,
                                unsigned long elapsed);

#endif /* AMINETXDUO_TOOLBUDGET_H */
