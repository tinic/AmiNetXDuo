/*
 * The delay that is not a counted loop, on the host.
 *
 * WHY THIS IS A HOST TEST AND NOT AN EMULATOR RUN.  The defect this file
 * guards against is a wait that comes out short on a fast machine, and every
 * emulator this project runs against emulates a fast machine -- so a green
 * Amiberry run says nothing at all about it, in either direction.  What
 * decides the question is the relationship between three numbers: how many
 * iterations the caller's loop is worth, how far the beam moved while it ran,
 * and when netdev_wait_done() finally says yes.  On real hardware two of those
 * belong to the machine.  Here all three belong to this file, because it
 * supplies the beam -- which is the only way to run the same binary as a
 * 14 MHz 68020 and as an accelerator and compare the answers.
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

/* ------------------------------------------------------------- the beam --- */

/*
 * The seam netdev_clock.c opens under NETDEV_CLOCK_TEST.  beam_every is how
 * many reads of the beam it takes to advance one scan line, and it is exactly
 * the axis the real machines differ along: a stock 68020 gets round a spin
 * loop a few dozen times to the line, an accelerated one tens of thousands of
 * times.  Everything below is one binary run at several values of it.
 *
 * A real beam advances with time and this one advances with reads, so the
 * iteration counts below are this model's and not a machine's -- they move
 * with NDC_BEAM_EVERY, which decides how many iterations there are between two
 * reads.  Every assertion on them is a lower bound for that reason.  What is
 * exact, and what the test is for, is the number of LINES a wait waits.
 */
static ULONG beam_reads;
static ULONG beam_every = 1;
static ULONG beam_lines;                /* every line ever advanced, no wrap */
static UWORD beam_pos;
static int   beam_stuck;

VOID netdev_clock_test_forget(VOID);

UWORD netdev_clock_test_beam(VOID)
{
    beam_reads++;

    if (!beam_stuck && beam_every != 0u && (beam_reads % beam_every) == 0u)
    {
        beam_lines++;
        beam_pos = (UWORD)((beam_pos + 1u) & 0xffu);
    }

    return beam_pos;
}

static void beam_set(ULONG every, int stuck)
{
    beam_reads = 0;
    beam_lines = 0;
    beam_every = every;
    beam_pos   = 0;
    beam_stuck = stuck;
    netdev_clock_test_forget();
}

/* Run a wait to completion and answer how many times round the loop it went. */
static ULONG spin(ULONG us, ULONG spins)
{
    NetdevWait w;
    ULONG      iters = 0;

    netdev_wait_begin(&w, us, spins);

    do
        iters++;
    while (!netdev_wait_done(&w));

    return iters;
}

/* The measurement's own cost, so a wait's line count can be read out of
   beam_lines without the calibration's lines in it. */
static ULONG lines_after_measure(void)
{
    (VOID)netdev_clock_spins_per_line();
    return beam_lines;
}

int main(void)
{
    ULONG base;
    ULONG iters;

    /*
     * A MACHINE WITH NO BEAM is the old driver exactly.  Nothing measures
     * anything and the caller's count is the whole of the wait -- which is
     * also what the host build of el3.c sees, since it compiles
     * netdev_clock.c on a machine with no chipset at all.
     */
    beam_set(1, 1);
    expect("no beam, 1000 spins", spin(300000, 1000), 1000);
    expect("no beam, 4 spins", spin(2000, 4), 4);
    expect("no beam, no clock", netdev_clock_spins_per_line(), 0);

    /*
     * A SLOW MACHINE: one line per loop iteration, the shape a 14 MHz 68020
     * has when the loop body is a real bus access.  Count and clock are then
     * close, and the longer of the two wins.  A 2 ms wait is 67 lines at 30 us
     * to the line, so a floor of 8000 iterations is much the longer and the
     * wait runs all 8000: the machine this driver was tuned on gets precisely
     * what it always got.
     */
    beam_set(1, 0);
    expect("slow machine, count dominates", spin(2000, 8000), 8000);

    /*
     * THE SAME WAIT ON AN ACCELERATOR.  Same binary, same floor -- but the
     * loop is now 500 times faster relative to the chipset, the count runs out
     * long before the time does, and the beam ends the wait.  This is the
     * whole defect in two assertions: under the old code the answer here was
     * 8000 iterations and a few microseconds of wall clock.
     */
    beam_set(500, 0);
    base  = lines_after_measure();
    iters = spin(2000, 8000);
    expect_at_least("fast machine, clock dominates", iters, 66UL * 500UL);
    expect_at_least("fast machine, lines waited", beam_lines - base, 67);

    /*
     * AND IT SCALES WITH THE TIME ASKED FOR rather than with the count: the
     * 300 ms Gayle reset hold is 10000 lines at 30 us to the line on every
     * machine, which is the wait whose collapse takes a PC Card's power-up
     * sequence with it.
     */
    beam_set(500, 0);
    base  = lines_after_measure();
    iters = spin(300000, 4);
    expect_at_least("gayle hold, lines waited", beam_lines - base, 10000);
    expect_at_least("gayle hold, iterations", iters, 9999UL * 500UL);

    /*
     * A WAIT BELOW NETDEV_WAIT_MIN_US NEVER CONSULTS THE CLOCK.  ne2000.c's
     * one-microsecond arm runs inside the receive drain at interrupt level,
     * where netdev_clock.c's measurement must never be reached, and it is a
     * bus barrier rather than a duration in any case.  Four iterations in,
     * four iterations out, and not one read of the beam.
     */
    beam_set(1, 0);
    expect("barrier, iterations", spin(1, 4), 4);
    expect("barrier, beam untouched", beam_reads, 0);

    beam_set(1, 0);
    expect("249 us is still a barrier", spin(249, 996), 996);
    expect("249 us, beam untouched", beam_reads, 0);

    /*
     * THE COUNT IS A FLOOR AND NOT A CEILING.  A caller that asks for time and
     * passes no count at all still waits the time.
     */
    beam_set(4, 0);
    base = lines_after_measure();
    (VOID)spin(3000, 0);
    expect_at_least("no floor, still timed", beam_lines - base, 100);

    /*
     * A BEAM THAT MOVES ONCE AND THEN STOPS is not a clock, and it must not
     * hang the claim.  No Amiga's beam does this; the guard exists because a
     * wait that cannot end is worse than a wait that is wrong, and because
     * nothing else in this file could show that the guard is there.  The floor
     * is still honoured: the guard ends the wait, it never shortens it.
     */
    beam_set(1, 0);
    (VOID)netdev_clock_spins_per_line();
    beam_stuck = 1;
    expect_at_least("stuck beam terminates", spin(300000, 32), 32);

    printf("%s\n", failures == 0 ? "PASS" : "FAIL");

    return failures == 0 ? 0 : 1;
}
