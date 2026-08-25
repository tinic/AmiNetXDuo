/*
 * anxnet.device: the beam clock behind netdev_wait_*().
 *
 * The header carries the argument.  This is the arithmetic.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netdev_clock.h"

/*
 * $DFF004 is VPOSR and $DFF006 is VHPOSR, and one longword read at $DFF004
 * takes both.  All eleven vertical bits are wanted (V10..V8 low in VPOSR,
 * V7..V0 high in VHPOSR): a field is 313 lines on PAL and 625 in a multiscan
 * mode, neither of which fits a byte.  One longword, so an interrupt cannot
 * split the pair across a line boundary.
 *
 * Every non-m68k build has no chipset at $DFF004 and must not dereference it;
 * the m68k predefine is the discriminator.  test/test_netdev_clock.c defines
 * NETDEV_CLOCK_TEST and supplies a beam it drives itself.
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
 * Two bands and nothing between them: a 15 kHz field is 262 lines on NTSC and
 * 313 on PAL; a 31 kHz multiscan field is twice either.  Both costs are UNDER
 * the true line (63 against 63.56 us, 31 against 31.75), so a wait comes out
 * long by a fraction of a percent rather than short.
 */
#define NDC_FIELD_15K_MIN   240u
#define NDC_FIELD_15K_MAX   400u
#define NDC_US_LINE_15K     63u
#define NDC_FIELD_31K_MIN   401u
#define NDC_FIELD_31K_MAX   800u
#define NDC_US_LINE_31K     31u

/*
 * How long to look for a moving beam before concluding there is not one.  The
 * bound exists so that a machine with nothing at $DFF004 cannot hang the claim,
 * not to time anything.  The field measurement needs its own, far larger bound.
 */
#define NDC_PROBE_SPINS     1000000UL
#define NDC_FIELD_SPINS     100000000UL

/*
 * How many iterations between reads of the beam.  Not one: a wait's floor is
 * the OLD iteration count, so a chip read per iteration would lengthen every
 * wait on the reference machine.  A missed transition makes a wait LONGER,
 * never shorter, because it is a line not counted.
 */
#define NDC_BEAM_EVERY      4u

/*
 * 0 = not measured yet, 1 = the beam moves and the clock is usable,
 * 2 = nothing moves, every wait is its caller's count and nothing more.
 * Written once; every caller is at task level, so there is nothing to guard.
 */
static UBYTE ndc_state;
static ULONG ndc_spins_line;
static ULONG ndc_us_line;
static ULONG ndc_field_lines;

/*
 * How many fields to watch before answering.  More than one because the
 * measurement below is a maximum, and a maximum wants more than one chance at
 * the top of the range; four is 80 ms at 50 Hz, once, on the claim path.
 */
#define NDC_FIELDS_WATCHED  4u

#if NDC_HAVE_BEAM
/*
 * How many lines a field has on this machine.  THE HIGHEST POSITION SEEN, NOT
 * THE NUMBER OF CHANGES SEEN: anything that stops this loop for longer than a
 * line makes the beam jump, and every line in the jump is a line not counted.
 * The full eleven bits are read; the low byte alone tops out at 255.
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
 * between two of its transitions, and how long a line is on this machine.  The
 * middle number decides nothing; netdev_pcmcia.c publishes it as evidence.
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
     * A measurement in neither band is not an error and does not disqualify the
     * clock: the beam demonstrably moves, so the wait is still measured, and
     * only the price of a line falls back to the floor.
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
 * How many iterations to give the clock before deciding it has stopped, derived
 * as NDC_CAP_SLACK times what a line was measured to be worth.  It exists only
 * so a beam that stops after it was measured cannot hang the claim, and it can
 * never shorten a wait.  Saturating: a guard that wrapped would be the hang.
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
         * A change is one line, never more: the caller's loop body is a bus
         * access of a microsecond or so and a line is at least thirty-one, and
         * a missed transition makes the wait longer, not shorter.  Transitions
         * rather than subtracted positions, which would have to know a field's
         * length to survive the wrap at the end of one.
         */
        if (now != w->nw_Beam)
        {
            w->nw_Beam = now;
            if (w->nw_Lines != 0u)
                w->nw_Lines--;
        }

        /*
         * The floor is unconditional.  The cap is only what stops a stopped
         * beam from being a hang: a cap that runs out ends the wait, and a
         * floor that has not run out never does.
         */
        return (BOOL)(w->nw_Spins == 0u &&
                      (w->nw_Lines == 0u || w->nw_Cap == 0u));
    }
#endif

    return (BOOL)(w->nw_Spins == 0u);
}
