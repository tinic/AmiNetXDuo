/*
 * Does tx_time_get() still tell the time after the tick task is starved?
 *
 * The port's tick task measures elapsed time from ReadEClock() but is only
 * allowed to deliver TX_AMIGA_TIMER_MAX_CATCHUP ticks per wakeup, so a long
 * stall leaves the timer wheel short.  Whether that also costs TIMEKEEPING
 * depends on where the ThreadX clock comes from, and that is what this probe
 * measures: it holds the machine in Forbid() for longer than the cap allows,
 * then compares how far tx_time_get() moved against how far the E-Clock moved
 * over the same window.
 *
 * A port that counts ticks reports the stall as time that never happened.  A
 * port that derives the clock from the E-Clock reports it as timers running
 * late, which is what tx_amiga_tick_skew is for.  Both are printed, so the
 * numbers say which port this is rather than the probe having to know.
 *
 * ami_millis() is the reference.  It is E-Clock based and has nothing to do
 * with ThreadX, which is the point, and it keeps working inside the Forbid(),
 * because ReadEClock() reads a CIA and never waits.
 *
 *   AMINETXDUO_RUN_TAG=tick ./tools/amiberry-run.sh   -t 120 build/cm/tools/smoke/tickclock
 *   AMINETXDUO_RUN_TAG=tick ./tools/amiberry-run.sh -t 120 build/cm/tools/smoke/tickclock
 *
 * SPDX-License-Identifier: MIT
 */

/* tx_api.h FIRST: exec/types.h does #define VOID void, which collides with
   tx_port.h's typedef void VOID if the Amiga headers land first. */
#include "tx_api.h"
#include "tx_amiga.h"

#include <proto/exec.h>
#include <proto/dos.h>

#include "aminetxduo/compat.h"

#define STALL_MS        750     /* well past TX_AMIGA_TIMER_MAX_CATCHUP */
#define STALLS          3
#define SETTLE_MS       500     /* time for the wheel to work off its backlog */
#define IDLE_MS         2000

/* Ticks the clock may be out by and still count as tracking real time.  One
   for the rounding at each end of the window, one for the tick that is in
   flight when a sample is taken. */
#define TOLERANCE       3

static LONG checks, failures;

static void check(const char *what, BOOL ok, LONG detail)
{
    checks++;
    if (!ok)
        failures++;
    Printf((CONST_STRPTR)"  %s %s (%ld)\n", (LONG)(ok ? "ok  " : "FAIL"),
           (LONG)what, detail);
}

VOID tx_application_define(VOID *first_unused)
{
    (VOID)first_unused;
}

/* ms of real time, as ThreadX would report it. */
static ULONG tx_ms(void)
{
    return (tx_time_get() * 1000UL) / (ULONG)TX_TIMER_TICKS_PER_SECOND;
}

/*
 * Hold the machine for `ms` and give the tick task no chance to run.  Forbid()
 * stops every task switch, so the tick's timer.device requests still complete
 * and still Signal() it, and it still is not dispatched, which is exactly the
 * shape of the 745 ms stall in docs/RESEARCH.md 42.6.
 */
static ULONG hold_machine(ULONG ms)
{
    ULONG t0;
    ULONG now;

    (VOID)ami_millis();                 /* open the device outside the Forbid */

    Forbid();
    t0 = ami_millis();
    do
    {
        now = ami_millis();
    }
    while ((now - t0) < ms);
    Permit();

    return now - t0;
}

/* |a - b|, for two ULONG millisecond readings that should be equal. */
static ULONG gap(ULONG a, ULONG b)
{
    return (a > b) ? (a - b) : (b - a);
}

static void report_ticks(const char *when)
{
    TX_AMIGA_TICK_STATS s;

    tx_amiga_tick_stats(&s);

    Printf((CONST_STRPTR)"  %s: delivered %lu, wakeups %lu, uptime %lu ms\n",
           (LONG)when, s.tx_amiga_tick_delivered, s.tx_amiga_tick_wakeups,
           tx_amiga_uptime_ms(&s));
    Printf((CONST_STRPTR)"  %s: clipped %lu, lost %lu, over budget %lu, deferred %lu\n",
           (LONG)when, s.tx_amiga_tick_clipped, s.tx_amiga_tick_lost,
           s.tx_amiga_tick_over_budget, s.tx_amiga_tick_deferred);
    Printf((CONST_STRPTR)"  %s: wheel %lu ticks late, worst %lu; worst stall %lu ms\n",
           (LONG)when, s.tx_amiga_tick_skew, s.tx_amiga_tick_skew_peak,
           s.tx_amiga_tick_worst_stall_ms);
}

/*
 * Wall time and ThreadX time over the same window.  `label` says what happened
 * inside it; `lost_ms` is how far ThreadX fell behind, which is the number the
 * whole probe exists to print.
 */
static ULONG window(const char *label, ULONG wall0, ULONG tx0)
{
    ULONG wall = ami_millis() - wall0;
    ULONG tx   = tx_ms() - tx0;
    ULONG out  = gap(wall, tx);

    Printf((CONST_STRPTR)"  %s: %lu ms wall, %lu ms on the ThreadX clock, "
                         "out by %lu ms\n",
           (LONG)label, wall, tx, out);

    return out;
}

int main(int argc, char **argv)
{
    UINT  status;
    ULONG wall0, tx0, out;
    ULONG held, monotonic_fail;
    ULONG last, i;
    int   n;

    (void)argc;
    (void)argv;

    Printf((CONST_STRPTR)"tickclock: is the ThreadX clock the E-Clock's or the "
                         "tick counter's?\n\n");

    status = tx_amiga_kernel_start();
    if (status != TX_SUCCESS)
    {
        Printf((CONST_STRPTR)"tx_amiga_kernel_start() failed: %lu\n", (ULONG)status);
        return RETURN_FAIL;
    }

    /* The tick validates its wakeup source for ~250 ms before the first tick. */
    Delay(50);
    report_ticks("start");

    /* ---- undisturbed ---------------------------------------------------- */

    wall0 = ami_millis();
    tx0   = tx_ms();
    Delay(IDLE_MS / 20UL);
    out = window("idle", wall0, tx0);

    check("clock tracks real time while nothing interferes",
          out <= (TOLERANCE * 1000UL) / (ULONG)TX_TIMER_TICKS_PER_SECOND,
          (LONG)out);

    /* ---- starved -------------------------------------------------------- */

    wall0 = ami_millis();
    tx0   = tx_ms();

    for (n = 0; n < STALLS; n++)
    {
        held = hold_machine(STALL_MS);
        Printf((CONST_STRPTR)"  held the machine for %lu ms\n", held);
        Delay(SETTLE_MS / 20UL);
    }

    out = window("stalled", wall0, tx0);
    report_ticks("after");

    check("clock tracks real time across a stall that drops ticks",
          out <= (TOLERANCE * 1000UL) / (ULONG)TX_TIMER_TICKS_PER_SECOND,
          (LONG)out);

    {
        TX_AMIGA_TICK_STATS s;

        tx_amiga_tick_stats(&s);

        /* Without this the run proves nothing: if nothing was dropped, a port
           that counts ticks would pass the check above as well. */
        check("the stall did drop ticks", s.tx_amiga_tick_lost > 0UL,
              (LONG)s.tx_amiga_tick_lost);
        check("the drop is visible as wheel lateness",
              s.tx_amiga_tick_skew_peak > 0UL, (LONG)s.tx_amiga_tick_skew_peak);
        check("delivered + lost accounts for the elapsed ticks",
              gap((s.tx_amiga_tick_delivered + s.tx_amiga_tick_lost) * 1000UL
                      / (ULONG)TX_TIMER_TICKS_PER_SECOND,
                  tx_amiga_uptime_ms(&s)) < 200UL,
              (LONG)(s.tx_amiga_tick_delivered + s.tx_amiga_tick_lost));
    }

    /* ---- forward only --------------------------------------------------- */

    monotonic_fail = 0UL;
    last = tx_time_get();
    for (i = 0UL; i < 20000UL; i++)
    {
        ULONG t = tx_time_get();

        if (t < last)
            monotonic_fail++;
        last = t;
    }
    (VOID)hold_machine(STALL_MS);
    for (i = 0UL; i < 20000UL; i++)
    {
        ULONG t = tx_time_get();

        if (t < last)
            monotonic_fail++;
        last = t;
    }

    check("the clock never went backwards", monotonic_fail == 0UL,
          (LONG)monotonic_fail);

    status = tx_amiga_kernel_stop();
    if (status != TX_SUCCESS)
        Printf((CONST_STRPTR)"tx_amiga_kernel_stop() returned %lu\n", (ULONG)status);

    Printf((CONST_STRPTR)"\n%ld checks, %ld failure(s), %s\n", checks, failures,
           (LONG)(failures == 0 ? "PASS" : "FAIL"));

    return (failures == 0) ? RETURN_OK : RETURN_ERROR;
}
