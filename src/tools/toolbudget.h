/*
 * toolbudget, how one connect timeout is divided over the addresses a name
 * resolved to.
 *
 * Its own file, and written in plain C with no AmigaOS types, for the same
 * reason fetchurl.c is: this is arithmetic about time that nothing on the
 * wire reveals.  A schedule that quietly grew from ten seconds to three
 * minutes looks identical in a capture, in a log and in the source -- it is
 * only visible as a user waiting -- so it is compiled twice, once for m68k as
 * part of every command that links toolsock.c and once natively by
 * src/tools/test/test_toolbudget.c, where the ceilings can be asserted on
 * rather than reasoned about.
 *
 * WHAT THE RULES ARE, all of them:
 *
 *   1.  TIMEOUT/-w bounds the CALL, not each address in it.  Three addresses
 *       and TIMEOUT=30 means the command returns by 30 seconds, not by 90.
 *   2.  Every address but the last is capped at TOOL_BUDGET_TRY_SECS, so one
 *       dead address cannot eat the whole budget before the live one is
 *       reached.
 *   3.  What the capped trials did not use goes to the last address.
 *   4.  If budget still remains after the whole list -- which is what happens
 *       when a later address fails fast, with a reset or with no route --
 *       the addresses that were cut short are tried again out of it.  A slow
 *       path must not be abandoned because a different address failed
 *       quickly.
 *   5.  A second attempt is only ever offered a LONGER ceiling than the
 *       first.  Each attempt opens a new socket and so repeats the SYN
 *       schedule from zero; one given the same ten seconds again can only
 *       fail the same way, ten seconds later.
 *   6.  A caller that set no timeout gets no second round.  There is no
 *       leftover budget to spend, and the stack's own ceiling is not a
 *       leftover -- the last address in the list was given exactly that
 *       already.  This is the rule that keeps `telnet host`, which sets no
 *       timeout, from spending the stack's whole 191-second SYN schedule
 *       retrying a blackholed AAAA after the A had already refused.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_TOOLBUDGET_H
#define AMINETXDUO_TOOLBUDGET_H

/*
 * How long one address gets while another is still untried.
 *
 * Ten seconds spans four SYNs on this stack -- 0, 1, 3 and 7 s -- so three
 * lost in a row are ridden out before the next address is tried, which is
 * well past what a slow link costs a handshake.  It is not a race delay.
 */
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
 * Seconds of the caller's timeout not yet spent.
 *
 * `elapsed` is what the clock says.  ZERO IS BOTH ANSWERS and deliberately
 * so: with no caller timeout there is nothing to divide, and zero is what
 * tool_sock_connect_timed() reads as "the stack's own ceiling", which is
 * exactly what an undivided call should get.
 */
unsigned long tool_budget_left(const ToolBudget *b, unsigned long elapsed);

/*
 * The ceiling for the first attempt at address `i`.
 *
 * 0 when the budget is gone and the walk must stop, 1 otherwise.  *secs is
 * the ceiling for the attempt, 0 meaning the stack's own; *cut is 1 when it
 * was shortened to leave room for the addresses behind it, which is what
 * makes the address a candidate for a second attempt.
 */
int tool_budget_first(const ToolBudget *b, unsigned long i,
                      unsigned long elapsed, unsigned long *secs, int *cut);

/*
 * What an attempt cost.
 *
 * Only a timed-out attempt is counted, and it is counted at its whole
 * ceiling, because that is what it ran.  This is a floor under the clock:
 * ami_millis() answers 0 on a machine where timer.device did not open, and
 * without the floor a missing clock would make every attempt look free and
 * hand the full timeout out again on the next one.
 */
void tool_budget_done(ToolBudget *b, unsigned long i, unsigned long secs,
                      int timed_out, int cut);

/* The ceiling for a second attempt at address `i`, 0 for "do not". */
unsigned long tool_budget_again(const ToolBudget *b, unsigned long i,
                                unsigned long elapsed);

#endif /* AMINETXDUO_TOOLBUDGET_H */
