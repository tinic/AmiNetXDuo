/*
 * anxnet.device: the beam clock behind netdev_wait_*().
 *
 * The header carries the argument.  This is the arithmetic.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netdev_clock.h"

/*
 * The beam, and the two seams this file has for a build that is not a driver.
 *
 * $DFF006 is VHPOSR: high byte the vertical position, low byte the horizontal
 * one.  Only the vertical byte is read here, because only its transitions are
 * counted, and a byte read of a word register is a second bus cycle on some
 * machines -- so the word is read and shifted rather than $DFF006 addressed as
 * a byte.
 *
 * test/test_netdev_clock.c defines NETDEV_CLOCK_TEST and supplies a beam it
 * drives itself, because the two properties that matter here -- that a wait
 * never ends before its floor, and that it does end once the beam has moved
 * far enough -- cannot be asserted on a machine whose beam nobody controls,
 * and are not visible in an emulator run either.
 *
 * Every other host build has no chipset at $DFF006 and must not dereference
 * it.  The m68k predefine is the discriminator: every build of this file that
 * runs on an Amiga is an m68k build, and every build that is not is a test.
 * With no beam nothing is measured, every wait falls back to its caller's
 * iteration count, and the host build of el3.c drives precisely the loops it
 * drove before this file existed.
 */
#if defined(NETDEV_CLOCK_TEST)
#define NDC_HAVE_BEAM   1
extern UWORD netdev_clock_test_beam(VOID);
static UWORD ndc_beam(VOID)
{
    return netdev_clock_test_beam();
}
#elif defined(__mc68000__)
#define NDC_HAVE_BEAM   1
static UWORD ndc_beam(VOID)
{
    return (UWORD)((*(volatile UWORD *)0x00DFF006UL) >> 8);
}
#else
#define NDC_HAVE_BEAM   0
#endif

/*
 * The shortest scan line any Amiga display mode produces is about 31.75 us
 * (the 31.5 kHz double-scan modes).  30 is under it, so a wait costed at 30 us
 * to the line is never short on any mode, and is about twice the requested
 * time on the 15 kHz modes almost every one of these machines runs in.
 */
#define NDC_US_PER_LINE     30u

/*
 * How long to look for a moving beam before concluding there is not one.
 * Generous on purpose: a stock 68000 goes round this loop in about 2 us, so a
 * scan line is a few dozen iterations and any real chipset answers in well
 * under a thousand.  The bound exists so that a machine with nothing at
 * $DFF006 cannot hang the claim, not to time anything.
 */
#define NDC_PROBE_SPINS     1000000UL

/*
 * How many iterations between reads of the beam.
 *
 * Not one, and the reason is the machine that already works.  A wait's floor
 * is the OLD iteration count, and its duration on a 14 MHz 68020 is that count
 * times the cost of one iteration -- so adding a chip read to every iteration
 * would lengthen every wait on the reference machine by whatever a custom
 * register costs there, which is comparable to the PCMCIA attribute read the
 * loop already does.  The 300 ms hold would come out at half a second or more
 * on a machine where nothing was wrong.
 *
 * Four is dense enough and cheap enough.  Four iterations of the pc_settle()
 * loop is a couple of microseconds against a 64 us line, so nothing is missed
 * there; the one loop where it is close is el3_reset_wait(), whose body is a
 * PCMCIA register access priced at 8.3 us in netdev_nic.h -- four of those is
 * 33 us, under a 15 kHz line and over a 31 kHz one.  That is not a correctness
 * question: a missed transition is a line not counted, so the wait comes out
 * LONGER than asked for and never shorter.
 */
#define NDC_BEAM_EVERY      4u

/*
 * 0 = not measured yet, 1 = the beam moves and the clock is usable,
 * 2 = nothing moves, every wait is its caller's count and nothing more.
 *
 * Written once.  Every caller is at task level (see NETDEV_WAIT_MIN_US in the
 * header: nothing on an interrupt path reaches this), so there is no
 * re-entrancy to guard -- and a second measurement would in any case write the
 * same answer.
 */
static UBYTE ndc_state;
static ULONG ndc_spins_line;

/*
 * Measure once: does the beam move, and how many iterations of a bare spin fit
 * between two of its transitions.
 *
 * The second number decides nothing.  It is the evidence: on a 14 MHz 68020 it
 * comes out in the tens, on an accelerator in the tens of thousands, and it is
 * the difference between those two figures that made every counted delay in
 * this driver wrong on the fast machine.  netdev_pcmcia.c publishes it through
 * the probe record.
 */
static VOID ndc_measure(VOID)
{
#if NDC_HAVE_BEAM
    ULONG spins;
    UWORD first;
    UWORD edge;

    ndc_state      = 2;
    ndc_spins_line = 0;

    /* Anything at all at $DFF006? */
    first = ndc_beam();
    for (spins = NDC_PROBE_SPINS; spins != 0u; spins--)
    {
        edge = ndc_beam();
        if (edge != first)
            break;
    }

    if (spins == 0u)
        return;                     /* no beam here: the counts are all there is */

    /* One whole line, from the transition just found to the next one. */
    ndc_spins_line = 0;
    for (spins = NDC_PROBE_SPINS; spins != 0u; spins--)
    {
        ndc_spins_line++;
        if (ndc_beam() != edge)
            break;
    }

    if (spins == 0u)
    {
        /* It moved once and then stopped.  Not a clock. */
        ndc_spins_line = 0;
        return;
    }

    ndc_state = 1;
#else
    ndc_state      = 2;
    ndc_spins_line = 0;
#endif
}

ULONG netdev_clock_spins_per_line(VOID)
{
    if (ndc_state == 0)
        ndc_measure();

    return ndc_spins_line;
}

#if defined(NETDEV_CLOCK_TEST)
/* Forget the measurement, so that one test process can drive several
   machines past this file.  Nothing in the driver calls it and nothing in
   the driver could: a machine does not change its chipset at runtime. */
VOID netdev_clock_test_forget(VOID)
{
    ndc_state      = 0;
    ndc_spins_line = 0;
}
#endif

/*
 * How many iterations to give the clock before deciding it has stopped.
 *
 * This is the one number in the file that is still an iteration count, and it
 * is DERIVED rather than assumed: NDC_CAP_SLACK times as many iterations as
 * the measurement above says a line is worth.  It has to be generous in BOTH
 * directions, because a real wait's loop is not the measurement's loop: it
 * carries the caller's bus access, which makes it slower, and it reads the
 * beam only every NDC_BEAM_EVERY iterations, which makes it faster.  On a
 * PCMCIA attribute read against a chip-register read the second effect wins
 * and a real wait can be three or four times denser per line than the
 * calibration was.  Sixteen covers that with room over.
 *
 * It exists only so that a beam which stops after it was measured -- which no
 * Amiga's does, and which the host test arranges deliberately -- cannot hang
 * the claim.  It can never shorten a wait: netdev_wait_done() honours the
 * caller's floor whatever this says.  Saturating, because a guard that wrapped
 * would be the hang it is guarding against.
 */
#define NDC_CAP_SLACK   16u

#if NDC_HAVE_BEAM
static ULONG ndc_cap(ULONG lines)
{
    ULONG per = ndc_spins_line;
    ULONG cap;

    if (per == 0u)
        per = 1u;

    if (lines > 0xffffffffUL / per)
        return 0xffffffffUL;

    cap = lines * per;

    if (cap > 0xffffffffUL / (ULONG)NDC_CAP_SLACK)
        return 0xffffffffUL;

    return cap * (ULONG)NDC_CAP_SLACK;
}
#endif

VOID netdev_wait_begin(NetdevWait *w, ULONG us, ULONG spins)
{
    w->nw_Spins = spins;
    w->nw_Lines = 0;
    w->nw_Cap   = 0;
    w->nw_Beam  = 0;
    w->nw_Timed = 0;
    w->nw_Tick  = 0;

    /*
     * A barrier rather than a duration, and the path this keeps the clock off:
     * ne2000.c's one-microsecond arm runs inside the receive drain, at
     * interrupt level, where ndc_measure() must never be reached.
     */
    if (us < NETDEV_WAIT_MIN_US)
        return;

    if (ndc_state == 0)
        ndc_measure();

    if (ndc_state != 1)
        return;

#if NDC_HAVE_BEAM
    /* Round up: a wait for one microsecond more than a line is two lines. */
    w->nw_Lines = (us + (ULONG)NDC_US_PER_LINE - 1u) / (ULONG)NDC_US_PER_LINE;
    w->nw_Cap   = ndc_cap(w->nw_Lines);
    w->nw_Beam  = ndc_beam();
    w->nw_Timed = 1;
    w->nw_Tick  = (UBYTE)NDC_BEAM_EVERY;
#endif
}

BOOL netdev_wait_done(NetdevWait *w)
{
    if (w->nw_Spins != 0u)
        w->nw_Spins--;

#if NDC_HAVE_BEAM
    if (w->nw_Timed != 0)
    {
        UWORD now;

        if (w->nw_Cap != 0u)
            w->nw_Cap--;

        if (--w->nw_Tick != 0)
            return FALSE;

        w->nw_Tick = (UBYTE)NDC_BEAM_EVERY;
        now        = ndc_beam();

        /*
         * A change is one line, never more.  The caller's loop body is a bus
         * access of a microsecond or so and a line is at least thirty, so a
         * transition cannot be missed in normal running -- and if one is
         * missed, because an interrupt landed in the middle, the wait comes
         * out LONGER than asked for rather than shorter.  That is the safe
         * direction and it is why this counts transitions rather than
         * subtracting positions: a subtraction would have to know how many
         * lines are in a field to survive the wrap at the end of one, and
         * getting that wrong is the failure this whole file exists to remove.
         */
        if (now != w->nw_Beam)
        {
            w->nw_Beam = now;
            if (w->nw_Lines != 0u)
                w->nw_Lines--;
        }

        /*
         * The floor is unconditional.  The clock is what the wait is FOR, and
         * the cap is only what stops a stopped beam from being a hang -- so a
         * cap that runs out ends the wait, and a floor that has not run out
         * never does.
         */
        return (BOOL)(w->nw_Spins == 0u &&
                      (w->nw_Lines == 0u || w->nw_Cap == 0u));
    }
#endif

    return (BOOL)(w->nw_Spins == 0u);
}
