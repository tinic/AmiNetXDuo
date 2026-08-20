/*
 * toolbudget, see toolbudget.h for the rules and why they are written down
 * somewhere a host test can read them.
 *
 * Includes nothing but its own header, deliberately.
 *
 * SPDX-License-Identifier: MIT
 */

#include "toolbudget.h"

/* Above the bits in `deferred`.  TOOL_ADDR_TRIES is four, so this is a
   guard against the list growing rather than a case that happens. */
#define TOOL_BUDGET_MAX_BITS    32UL

void tool_budget_init(ToolBudget *b, unsigned long total, unsigned long count)
{
    b->total    = total;
    b->count    = count;
    b->known    = 0UL;
    b->deferred = 0UL;
}

unsigned long tool_budget_left(const ToolBudget *b, unsigned long elapsed)
{
    unsigned long spent = (elapsed < b->known) ? b->known : elapsed;

    if (b->total == 0UL)
        return 0UL;

    return (spent >= b->total) ? 0UL : (b->total - spent);
}

int tool_budget_first(const ToolBudget *b, unsigned long i,
                      unsigned long elapsed, unsigned long *secs, int *cut)
{
    unsigned long left = tool_budget_left(b, elapsed);
    int           shortened;

    *secs = 0UL;
    *cut  = 0;

    if (i >= b->count)
        return 0;

    /* Gone, and only a caller that named a number can run out of it. */
    if (b->total != 0UL && left == 0UL)
        return 0;

    /* Capped only while there is another address behind this one, and only
       while the cap is actually shorter than what is left: with two seconds
       remaining, handing this address ten would break rule 1. */
    shortened = (i + 1UL < b->count) &&
                (b->total == 0UL || left > TOOL_BUDGET_TRY_SECS);

    *secs = shortened ? TOOL_BUDGET_TRY_SECS : left;
    *cut  = shortened ? 1 : 0;

    return 1;
}

void tool_budget_done(ToolBudget *b, unsigned long i, unsigned long secs,
                      int timed_out, int cut)
{
    if (!timed_out)
        return;

    b->known += secs;

    if (cut && i < TOOL_BUDGET_MAX_BITS)
        b->deferred |= 1UL << i;
}

unsigned long tool_budget_again(const ToolBudget *b, unsigned long i,
                                unsigned long elapsed)
{
    unsigned long left;

    /* Rule 6: nothing to draw from. */
    if (b->total == 0UL)
        return 0UL;

    if (i >= TOOL_BUDGET_MAX_BITS || (b->deferred & (1UL << i)) == 0UL)
        return 0UL;

    /* Rule 5: a ceiling no longer than the first attempt's cannot tell us
       anything the first attempt did not. */
    left = tool_budget_left(b, elapsed);

    return (left > TOOL_BUDGET_TRY_SECS) ? left : 0UL;
}
