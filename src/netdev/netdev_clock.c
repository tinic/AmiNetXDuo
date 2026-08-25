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
 * $DFF004 is VPOSR and $DFF006 is VHPOSR, and one longword read at $DFF004
 * takes both: the vertical position is spread across them, V10..V8 in the low
 * bits of VPOSR and V7..V0 in the high byte of VHPOSR.  All eleven bits are
 * wanted, not just the low eight, because this file measures the LENGTH OF A
 * FIELD as well as counting the lines in a wait, and a field is 313 lines on a
 * PAL machine and 625 in a multiscan mode -- neither of which fits in a byte,
 * and both of which have to be told apart from the wrap of one.
 *
 * The two words are read as one longword rather than separately, so that the
 * pair cannot be split across a line boundary by an interrupt; the chip bus is
 * 16 bits wide and turns it into the same two cycles either way.
 *
 * test/test_netdev_clock.c defines NETDEV_CLOCK_TEST and supplies a beam it
 * drives itself, because the three properties that matter here -- that a wait
 * never ends before its floor, that it does end once the beam has moved far
 * enough, and that a line is costed from the field it was measured in --
 * cannot be asserted on a machine whose beam nobody controls, and are not
 * visible in an emulator run either.
 *
 * Every other host build has no chipset at $DFF004 and must not dereference
 * it.  The m68k predefine is the discriminator: every build of this file that
 * runs on an Amiga is an m68k build, and every build that is not is a test.
 * With no beam nothing is measured, every wait falls back to its caller's
 * iteration count, and the host build of el3.c drives precisely the loops it
 * drove before this file existed.
 */
#if defined(NETDEV_CLOCK_TEST)
#define NDC_HAVE_BEAM   1
extern UWORD netdev_clock_test_vpos(VOID);
static UWORD ndc_vpos(VOID)
{
    return netdev_clock_test_vpos();
}
#elif defined(__mc68000__)
#define NDC_HAVE_BEAM   1
static UWORD ndc_vpos(VOID)
{
    ULONG v = *(volatile ULONG *)0x00DFF004UL;

    return (UWORD)((((v >> 16) & 0x0007UL) << 8) | ((v >> 8) & 0x00ffUL));
}
#else
#define NDC_HAVE_BEAM   0
#endif

/*
 * What a scan line costs, once the field it lives in has been measured.
 *
 * Two bands and nothing between them.  A 15 kHz field is 262 lines on an NTSC
 * machine and 313 on a PAL one; a 31 kHz multiscan field is twice either.  The
 * bands below are wide enough to hold every mode and its odd/even field, and
 * far enough apart that no measurement can be ambiguous.
 *
 * Both costs are UNDER the true line, 63 against NTSC's 63.56 us and 31
 * against Productivity's 31.75, which is the direction that matters: a wait
 * costed low comes out long by a fraction of a percent, and a wait costed high
 * comes out short, which is the whole defect this file exists to remove.
 */
#define NDC_FIELD_15K_MIN   240u
#define NDC_FIELD_15K_MAX   400u
#define NDC_US_LINE_15K     63u
#define NDC_FIELD_31K_MIN   401u
#define NDC_FIELD_31K_MAX   800u
#define NDC_US_LINE_31K     31u

/*
 * How long to look for a moving beam before concluding there is not one.
 * Generous on purpose: a stock 68000 goes round this loop in about 2 us, so a
 * scan line is a few dozen iterations and any real chipset answers in well
 * under a thousand.  The bound exists so that a machine with nothing at
 * $DFF004 cannot hang the claim, not to time anything.
 *
 * The field measurement below needs a bound of its own and it cannot be this
 * one: it watches four whole fields, a field is 313 lines, and an accelerated
 * machine goes round this loop tens of thousands of times per line, so the
 * honest worst case is tens of millions.  It is a separate number for that
 * reason and not because the two mean anything different.
 */
#define NDC_PROBE_SPINS     1000000UL
#define NDC_FIELD_SPINS     100000000UL

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
 * Four is dense enough to be exact and cheap enough not to matter.  A scan
 * line is at least 31 us; four iterations of the slowest loop that consults
 * the clock -- el3_reset_wait(), whose body is a PCMCIA register access priced
 * at 8.3 us in netdev_nic.h -- is 33 us, and four of the fastest is a fraction
 * of a microsecond.  If a transition is ever missed anyway the wait comes out
 * LONGER, never shorter, because a missed transition is a line not counted.
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
static ULONG ndc_us_line;
static ULONG ndc_field_lines;

/*
 * How many fields to watch before answering.  More than one because the
 * measurement below is a maximum, and a maximum wants more than one chance at
 * the top of the range; four is 80 ms at 50 Hz, once, on a claim path that
 * spends 300 ms holding the card in reset immediately afterwards.
 */
#define NDC_FIELDS_WATCHED  4u

#if NDC_HAVE_BEAM
/*
 * How many lines a field has on this machine, so that a line can be costed
 * from the mode the machine is actually in rather than from the shortest one
 * it could be in.
 *
 * THE HIGHEST POSITION SEEN, NOT THE NUMBER OF CHANGES SEEN.  Counting
 * transitions is the obvious way and it is wrong, because it assumes the
 * observer is never descheduled: anything that stops this loop for longer than
 * a line -- an interrupt, a task switch, or an emulator that runs the guest in
 * bursts and sleeps to keep pace with the wall clock -- makes the beam jump
 * several lines between two reads, and every line in the jump is a line not
 * counted.  Measured, not theorised: under Amiberry at 68020 the transition
 * count made a 313-line PAL field come out at 58, while at 68060 the same code
 * on the same machine got 313.  A maximum cannot be undercounted that way; it
 * only needs the top of the range to be sampled once in four fields.
 *
 * The full eleven bits are read for the same reason the longword is: the low
 * byte alone tops out at 255 and would call every mode a 15 kHz one.
 *
 * WHICH WAY THE REMAINING ERROR GOES.  If this is ever short anyway, the mode
 * is costed at a longer line than it has and waits come out UNDER the time
 * they ask for -- the one direction that matters here.  That is why nothing
 * rests on it alone: netdev_wait_begin() also carries the caller's original
 * iteration count as an unconditional floor, so a wait is never shorter than
 * the old counted loop made it on the same machine, whatever this returns.
 */
static ULONG ndc_measure_field(VOID)
{
    ULONG spins;
    ULONG fields = 0;
    UWORD max    = 0;
    UWORD last;
    UWORD v;

    last = ndc_vpos();

    for (spins = NDC_FIELD_SPINS; spins != 0u; spins--)
    {
        v = ndc_vpos();

        if (v > max)
            max = v;

        /* Any decrease is the end of a field.  A jump backwards cannot be
           anything else: the position only ever counts up within one. */
        if (v < last && ++fields >= (ULONG)NDC_FIELDS_WATCHED)
            return (ULONG)max + 1u;     /* positions are counted from zero */

        last = v;
    }

    return 0;       /* it never wrapped: not a field, and not a clock */
}
#endif

/*
 * Measure once: does the beam move, how many iterations of a bare spin fit
 * between two of its transitions, and how long a line is on this machine.
 *
 * The middle number decides nothing.  It is the evidence: on a 14 MHz 68020 it
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

    ndc_state       = 2;
    ndc_spins_line  = 0;
    ndc_us_line     = 0;
    ndc_field_lines = 0;

    /* Anything at all at $DFF004? */
    first = ndc_vpos();
    for (spins = NDC_PROBE_SPINS; spins != 0u; spins--)
    {
        edge = ndc_vpos();
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
        if (ndc_vpos() != edge)
            break;
    }

    if (spins == 0u)
    {
        /* It moved once and then stopped.  Not a clock. */
        ndc_spins_line = 0;
        return;
    }

    /*
     * And which of the two modes this is.  A measurement in neither band is
     * not an error and does not disqualify the clock: the beam demonstrably
     * moves, so the wait is still measured rather than counted, and only the
     * price of a line falls back to the floor.  A wait then comes out roughly
     * twice as long as asked for, which is the behaviour this file shipped
     * with before the field was counted at all.
     */
    ndc_field_lines = ndc_measure_field();
    ndc_us_line     = (ULONG)NETDEV_LINE_FLOOR_US;

    if (ndc_field_lines >= (ULONG)NDC_FIELD_15K_MIN &&
        ndc_field_lines <= (ULONG)NDC_FIELD_15K_MAX)
        ndc_us_line = (ULONG)NDC_US_LINE_15K;
    else if (ndc_field_lines >= (ULONG)NDC_FIELD_31K_MIN &&
             ndc_field_lines <= (ULONG)NDC_FIELD_31K_MAX)
        ndc_us_line = (ULONG)NDC_US_LINE_31K;

    ndc_state = 1;
#else
    ndc_state       = 2;
    ndc_spins_line  = 0;
    ndc_us_line     = 0;
    ndc_field_lines = 0;
#endif
}

ULONG netdev_clock_spins_per_line(VOID)
{
    if (ndc_state == 0)
        ndc_measure();

    return ndc_spins_line;
}

ULONG netdev_clock_us_per_line(VOID)
{
    if (ndc_state == 0)
        ndc_measure();

    return ndc_us_line;
}

ULONG netdev_clock_lines_per_field(VOID)
{
    if (ndc_state == 0)
        ndc_measure();

    return ndc_field_lines;
}

#if defined(NETDEV_CLOCK_TEST)
/* Forget the measurement, so that one test process can drive several
   machines past this file.  Nothing in the driver calls it and nothing in
   the driver could: a machine does not change its chipset at runtime. */
VOID netdev_clock_test_forget(VOID)
{
    ndc_state       = 0;
    ndc_spins_line  = 0;
    ndc_us_line     = 0;
    ndc_field_lines = 0;
}

ULONG netdev_clock_test_field(VOID)
{
    if (ndc_state == 0)
        ndc_measure();

    return ndc_field_lines;
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
    w->nw_Lines = (us + ndc_us_line - 1u) / ndc_us_line;
    w->nw_Cap   = ndc_cap(w->nw_Lines);
    w->nw_Beam  = ndc_vpos();
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
        now        = ndc_vpos();

        /*
         * A change is one line, never more.  The caller's loop body is a bus
         * access of a microsecond or so and a line is at least thirty-one, so
         * a transition cannot be missed in normal running -- and if one is
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
