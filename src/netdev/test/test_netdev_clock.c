/*
 * The delay that is not a counted loop, on the host.
 *
 * WHY THIS IS A HOST TEST AND NOT AN EMULATOR RUN.  The defect this file
 * guards against is a wait that comes out short on a fast machine, and every
 * emulator this project runs against emulates a fast machine -- so a green
 * Amiberry run says nothing at all about it, in either direction.  A 68060 arm
 * reproduces the CONDITION, a CPU fast relative to the chipset, and the
 * emulated cards it drives have no reset timing to violate, so nothing there
 * turns red when the hold collapses.  What decides the question is the
 * relationship between three numbers: how many iterations the caller's loop is
 * worth, how far the beam moved while it ran, and when netdev_wait_done()
 * finally says yes.  On real hardware two of those belong to the machine.
 * Here all three belong to this file, because it supplies the beam -- which is
 * the only way to run the same binary as a 14 MHz 68020 and as an accelerator
 * and compare the answers.
 *
 * AND THE OLD CODE IS RUN HERE TOO, as spin_counted() below.  A regression
 * test that only exercises the fix cannot say what the fix was for, and this
 * one is asserting a DIFFERENCE: the same 300 ms asked for by the same call
 * site, on the same simulated accelerator, comes out at three milliseconds the
 * old way and three hundred the new way.  Delete netdev_clock.c and the
 * assertions that fail are the ones naming the old behaviour, which is what a
 * gate for this defect has to do.
 *
 * TIME IN THIS FILE IS A TICK COUNT, advanced by the loop bodies that would
 * really cost time on a machine -- the caller's bus access, and each read of
 * the beam.  The beam's position is then derived from the clock rather than
 * from how often it happens to be looked at, which is what a real beam does
 * and what the previous shape of this file could not model.  Without that,
 * a loop which never reads the beam -- which is exactly what the old code is
 * -- experiences no time passing at all, and the control below could not be
 * written.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>

#include "netdev_clock.h"

static int failures;

static void expect(const char *what, ULONG got, ULONG want)
{
    if (got == want)
    {
        printf("ok   %s = %lu\n", what, (unsigned long)got);
        return;
    }

    printf("FAIL %s: got %lu, want %lu\n", what,
           (unsigned long)got, (unsigned long)want);
    failures++;
}

static void expect_at_least(const char *what, ULONG got, ULONG want)
{
    if (got >= want)
    {
        printf("ok   %s = %lu (>= %lu)\n", what,
               (unsigned long)got, (unsigned long)want);
        return;
    }

    printf("FAIL %s: got %lu, want at least %lu\n", what,
           (unsigned long)got, (unsigned long)want);
    failures++;
}

static void expect_below(const char *what, ULONG got, ULONG want)
{
    if (got < want)
    {
        printf("ok   %s = %lu (< %lu)\n", what,
               (unsigned long)got, (unsigned long)want);
        return;
    }

    printf("FAIL %s: got %lu, want below %lu\n", what,
           (unsigned long)got, (unsigned long)want);
    failures++;
}

/* ------------------------------------------------------- the machine ------ */

/*
 * One simulated machine: how many ticks of its clock a scan line lasts, and
 * how many lines its display mode puts in a field.  The first is the axis the
 * real machines differ along -- a stock 68020 gets round a spin loop a few
 * hundred times to the line, an accelerated one tens of thousands of times --
 * and the second is the one the driver has to measure before it can price a
 * line at all.
 */
static ULONG mach_ticks;            /* the clock, in loop-iteration units    */
static ULONG mach_ticks_per_line = 256;
static ULONG mach_lines_per_field = 313;   /* a PAL field                     */
static ULONG beam_reads;
static int   beam_stuck;
static UWORD beam_frozen;

VOID  netdev_clock_test_forget(VOID);
ULONG netdev_clock_test_field(VOID);

static ULONG mach_lines(void)
{
    return mach_ticks / mach_ticks_per_line;
}

/* Reading a custom register costs a bus cycle like anything else does. */
UWORD netdev_clock_test_vpos(VOID)
{
    beam_reads++;
    mach_ticks++;

    if (beam_stuck)
        return beam_frozen;

    return (UWORD)(mach_lines() % mach_lines_per_field);
}

static void machine(ULONG ticks_per_line, ULONG lines_per_field)
{
    mach_ticks           = 0;
    mach_ticks_per_line  = ticks_per_line;
    mach_lines_per_field = lines_per_field;
    beam_reads           = 0;
    beam_stuck           = 0;
    beam_frozen          = 0;
    netdev_clock_test_forget();
}

/*
 * A machine with no beam at all: the host builds, and the old driver on every
 * machine.  A frozen position from the first read onwards is what
 * netdev_clock.c's probe sees as "nothing here".
 */
static void machine_no_beam(void)
{
    machine(256, 313);
    beam_stuck  = 1;
    beam_frozen = 0;
}

/* ------------------------------------------------- the two delay shapes --- */

/*
 * THE FIX.  Run a wait to completion; answer how many lines of the machine's
 * clock went by while it ran, which is the only thing a caller of pc_settle()
 * ever wanted.
 */
static ULONG timed_lines(ULONG us, ULONG spins, ULONG *iters_out)
{
    NetdevWait w;
    ULONG      start;
    ULONG      iters = 0;

    (VOID)netdev_clock_us_per_line();       /* measure before the clock starts */
    start = mach_lines();

    netdev_wait_begin(&w, us, spins);

    do
    {
        iters++;
        mach_ticks++;                       /* the caller's own bus access */
    }
    while (!netdev_wait_done(&w));

    if (iters_out != NULL)
        *iters_out = iters;

    return mach_lines() - start;
}

/*
 * THE OLD CODE, byte for byte in shape: pc_settle(), ne_delay(), pnp_delay()
 * and el3_reset_wait() were all this loop with a different register in the
 * body.  Four reads to the microsecond, asserted rather than measured.
 */
static ULONG counted_lines(ULONG us)
{
    ULONG start = mach_lines();
    ULONG n     = us * 4u;

    while (n-- != 0u)
        mach_ticks++;

    return mach_lines() - start;
}

/* ---------------------------------------------------------------- arms ---- */

/*
 * The two machines every assertion below is run on.  REFERENCE is the 14 MHz
 * 68020 the counts in this driver were chosen against: a PCMCIA attribute read
 * is about a quarter of a microsecond, so a 63 us line holds roughly 256 of
 * them.  ACCELERATED is the same chipset with a CPU a hundred times quicker
 * through the loop, which is a PiStorm32, a Blizzard, an ACA or a Vampire.
 */
#define REFERENCE_TICKS_PER_LINE    256u
#define ACCELERATED_TICKS_PER_LINE  25600u

/* 300 ms at 63 us to the line, which is what the Gayle hold has to reach. */
#define GAYLE_HOLD_US       300000u
#define GAYLE_HOLD_SPINS    (GAYLE_HOLD_US * 4u)
#define GAYLE_HOLD_LINES    (GAYLE_HOLD_US / 63u)

int main(void)
{
    ULONG lines;
    ULONG iters;

    /* -------------------------------------------------------------------- */
    printf("-- the line is priced from the field it was measured in\n");

    /*
     * Every Amiga display mode, and the two that are not one.  A PAL or NTSC
     * field is 262 or 313 lines and its line is 63 us; a multiscan field is
     * twice either and its line is 31.  Nothing else is recognised, and
     * anything else is costed at the floor -- which is under the shortest line
     * any of them produces, so an unrecognised machine waits too long rather
     * than not long enough.
     */
    machine(REFERENCE_TICKS_PER_LINE, 313);
    expect("PAL field, lines", netdev_clock_test_field(), 313);
    expect("PAL line, us", netdev_clock_us_per_line(), 63);

    machine(REFERENCE_TICKS_PER_LINE, 262);
    expect("NTSC line, us", netdev_clock_us_per_line(), 63);

    machine(REFERENCE_TICKS_PER_LINE, 625);
    expect("DblPAL field, lines", netdev_clock_test_field(), 625);
    expect("DblPAL line, us", netdev_clock_us_per_line(), 31);

    machine(REFERENCE_TICKS_PER_LINE, 524);
    expect("Productivity line, us", netdev_clock_us_per_line(), 31);

    machine(REFERENCE_TICKS_PER_LINE, 100);
    expect("a field too short to be one falls back",
           netdev_clock_us_per_line(), NETDEV_LINE_FLOOR_US);

    machine(REFERENCE_TICKS_PER_LINE, 1500);
    expect("a field too long to be one falls back",
           netdev_clock_us_per_line(), NETDEV_LINE_FLOOR_US);

    /* And the measurement does not depend on how fast the CPU reading it is. */
    machine(ACCELERATED_TICKS_PER_LINE, 313);
    expect("the field is the same behind an accelerator",
           netdev_clock_test_field(), 313);
    expect("and so is the price of a line", netdev_clock_us_per_line(), 63);

    /* -------------------------------------------------------------------- */
    printf("\n-- the defect: the old loop on the two machines\n");

    /*
     * THIS IS WHAT WAS WRONG.  The same call, pc_settle(300000), on the two
     * machines, using the loop the driver used to run.  On the machine the
     * count was chosen for it is very nearly the 300 ms intended.  On the
     * accelerated machine the identical code waits about three, because a
     * count of bus reads was never a measure of time and the CPU got a hundred
     * times faster while the chipset did not.
     */
    machine(REFERENCE_TICKS_PER_LINE, 313);
    lines = counted_lines(GAYLE_HOLD_US);
    expect_at_least("old code, reference machine, lines", lines, 4600);

    machine(ACCELERATED_TICKS_PER_LINE, 313);
    lines = counted_lines(GAYLE_HOLD_US);
    expect_below("old code, accelerated machine, lines", lines, 100);

    /* -------------------------------------------------------------------- */
    printf("\n-- the fix: the same wait, measured\n");

    /*
     * THE SAME TWO MACHINES THROUGH netdev_wait_*().  Both reach the hold the
     * card's documentation requires, and the accelerated one reaches it by a
     * factor of a hundred more iterations than the reference one -- which is
     * the point.  The wait is the same length; the loop that fills it is not.
     */
    machine(REFERENCE_TICKS_PER_LINE, 313);
    lines = timed_lines(GAYLE_HOLD_US, GAYLE_HOLD_SPINS, &iters);
    expect_at_least("new code, reference machine, lines", lines, GAYLE_HOLD_LINES);
    expect_at_least("new code, reference machine, floor honoured",
                    iters, GAYLE_HOLD_SPINS);

    machine(ACCELERATED_TICKS_PER_LINE, 313);
    lines = timed_lines(GAYLE_HOLD_US, GAYLE_HOLD_SPINS, &iters);
    expect_at_least("new code, accelerated machine, lines", lines, GAYLE_HOLD_LINES);

    /*
     * AND IT IS NOT MERELY LONGER, IT IS RIGHT.  4762 lines at 63 us is 300
     * ms; the old file costed every line at 30 us and asked for 10000 of them,
     * which on a 15 kHz machine is 640 ms for a 300 ms hold.  Twice as long as
     * asked for is safe and is still not a measurement, so the ceiling is
     * asserted as well as the floor.
     */
    expect_below("new code, and not twice as long as asked", lines,
                 (GAYLE_HOLD_LINES * 12u) / 10u);

    /* -------------------------------------------------------------------- */
    printf("\n-- the floor, the barrier and the guard\n");

    /*
     * A MACHINE WITH NO BEAM is the old driver exactly.  Nothing measures
     * anything and the caller's count is the whole of the wait -- which is
     * also what the host build of el3.c sees, since it compiles
     * netdev_clock.c on a machine with no chipset at all.
     */
    machine_no_beam();
    (VOID)timed_lines(GAYLE_HOLD_US, 1000, &iters);
    expect("no beam, 1000 spins", iters, 1000);
    machine_no_beam();
    (VOID)timed_lines(2000, 4, &iters);
    expect("no beam, 4 spins", iters, 4);
    expect("no beam, no clock", netdev_clock_spins_per_line(), 0);
    expect("no beam, no line price", netdev_clock_us_per_line(), 0);

    /*
     * THE COUNT IS A FLOOR AND NOT A CEILING, in both directions.  On the
     * reference machine the count is the longer of the two and every one of
     * its iterations still runs; a caller that passes no count at all still
     * waits the time.
     */
    machine(REFERENCE_TICKS_PER_LINE, 313);
    lines = timed_lines(2000, 8000, &iters);
    expect_at_least("slow machine, every iteration of the count runs",
                    iters, 8000);
    expect_at_least("slow machine, and the time as well", lines, 2000u / 63u);

    machine(ACCELERATED_TICKS_PER_LINE, 313);
    lines = timed_lines(3000, 0, NULL);
    expect_at_least("no floor, still timed", lines, 3000u / 63u);

    /*
     * A WAIT BELOW NETDEV_WAIT_MIN_US NEVER CONSULTS THE CLOCK.  ne2000.c's
     * one-microsecond arm runs inside the receive drain at interrupt level,
     * where netdev_clock.c's measurement must never be reached, and it is a
     * bus barrier rather than a duration in any case.  Four iterations in,
     * four iterations out, and not one read of the beam.
     */
    /* The clock is measured first and the counter zeroed after it, so that
       what is asserted is that the WAIT never reads the beam -- not that the
       one-off calibration did not happen. */
    machine(REFERENCE_TICKS_PER_LINE, 313);
    (VOID)netdev_clock_us_per_line();
    beam_reads = 0;
    (VOID)timed_lines(1, 4, &iters);
    expect("barrier, iterations", iters, 4);
    expect("barrier, beam untouched", beam_reads, 0);

    machine(REFERENCE_TICKS_PER_LINE, 313);
    (VOID)netdev_clock_us_per_line();
    beam_reads = 0;
    (VOID)timed_lines(249, 996, &iters);
    expect("249 us is still a barrier", iters, 996);
    expect("249 us, beam untouched", beam_reads, 0);

    /*
     * A BEAM THAT MOVES AND THEN STOPS is not a clock, and it must not hang
     * the claim.  No Amiga's beam does this; the guard exists because a wait
     * that cannot end is worse than a wait that is wrong, and because nothing
     * else in this file could show that the guard is there.  The floor is
     * still honoured: the guard ends the wait, it never shortens it.
     */
    machine(REFERENCE_TICKS_PER_LINE, 313);
    (VOID)netdev_clock_us_per_line();
    beam_stuck  = 1;
    beam_frozen = 7;
    (VOID)timed_lines(GAYLE_HOLD_US, 32, &iters);
    expect_at_least("stuck beam terminates, floor kept", iters, 32);

    printf("\n%s\n", failures == 0 ? "PASS" : "FAIL");

    return failures == 0 ? 0 : 1;
}
