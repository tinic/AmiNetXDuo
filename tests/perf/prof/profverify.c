/*
 * AmiNetXDuo, does the sampler report the PC it thinks it does?
 *
 * A sampling profiler is the one measurement tool that can be comprehensively
 * wrong and still look right.  Read the exception frame two bytes off and
 * every recorded value is half an SR concatenated with half a PC, a number
 * that lands somewhere, resolves to some function, and ranks it convincingly.
 * Nothing downstream can detect that.  So it is established here, first,
 * against kernels whose exact byte ranges are known from the linker.
 *
 * Four phases, each preceded by prof_mark():
 *
 *   spinA/spinB/spinC   assembly loops in profverify.S with explicit _end
 *                       labels.  No calls, so every sample taken during a
 *                       phase must land inside that one function's range.
 *   exec                Forbid()/Permit() in a loop.  Those are ROM code
 *                       reached through exec.library's jump table, so the
 *                       phase tests the other half of the attribution: that
 *                       a Kickstart sample is recognised as Kickstart and
 *                       named from the LVO table rather than dropped.
 *
 * Three things are checked, and all three have to hold:
 *
 *   1. CONTAINMENT.  >= 90% of each spin phase's samples inside that
 *      function's [start, end).  This is the frame-offset test and it is
 *      sharp, a wrong offset scores approximately zero, not a bit less.
 *   2. PROPORTIONALITY.  Each phase's share of samples within four percentage
 *      points of its share of measured wall-clock.  A sampler can be
 *      correctly aimed and still biased, e.g. by only firing when some
 *      periodic thing is also running; this is what catches that.
 *   3. RATE.  Interrupts actually taken within 15% of rate x elapsed, and
 *      every frame format $0 on a 68010 and up.
 *
 * Run it on more than one CPU, because the frame is what is in question:
 *
 *   -m A500  68000   six-byte frame, no format word, VBR fixed at 0
 *   -m A1200 68020   eight-byte frame, format word, VBR readable
 *   -m A3000 68030   as the 68020, different memory system
 *
 * SPDX-License-Identifier: MIT
 */

#include "prof.h"
#include "aminetxduo/compat.h"

#include <exec/execbase.h>
#include <proto/exec.h>

#include <string.h>

extern VOID pv_spin_a(ULONG reps);
extern VOID pv_spin_b(ULONG reps);
extern VOID pv_spin_c(ULONG reps);
extern UBYTE pv_spin_a_end, pv_spin_b_end, pv_spin_c_end;

#define PV_RATE         1000UL
#define PV_MAX_SAMPLES  40000UL
#define PV_PHASES       4

/* Roughly 6:3:1 of wall clock, and long enough that a phase is thousands of
   samples rather than hundreds.  The exact figures do not matter, the test
   compares sampled share against measured share, it does not assume either. */
static const ULONG pv_reps[PV_PHASES] = { 2700000UL, 900000UL, 220000UL, 0UL };

static const char *pv_names[PV_PHASES] = { "spinA", "spinB", "spinC", "exec" };

static ULONG pv_ms[PV_PHASES];
static ULONG pv_count[PV_PHASES];
static ULONG pv_inrange[PV_PHASES];
static ULONG pv_super[PV_PHASES];

static ULONG pv_failures;

static VOID pv_check(BOOL ok, const char *what)
{
    if (!ok)
    {
        pv_failures++;
    }
    prof_log("  %-4s  %s", ok ? "ok" : "FAIL", what);
}

int main(void)
{
struct ExecBase         *eb = (struct ExecBase *)SysBase;
const struct ProfSample *s;
const struct ProfMark   *marks;
ULONG                    nmarks, stored, i, p;
ULONG                    total_ms = 0UL, total_samples = 0UL;
ULONG                    lo[PV_PHASES], hi[PV_PHASES];
ULONG                    t0, expect;

    prof_log("AmiNetXDuo, PC sampler self-test");
    prof_log("AttnFlags $%08lx, Exec %ld",
             (unsigned long)eb->AttnFlags, (long)eb->LibNode.lib_Version);

    lo[0] = (ULONG)pv_spin_a; hi[0] = (ULONG)&pv_spin_a_end;
    lo[1] = (ULONG)pv_spin_b; hi[1] = (ULONG)&pv_spin_b_end;
    lo[2] = (ULONG)pv_spin_c; hi[2] = (ULONG)&pv_spin_c_end;
    lo[3] = 0UL;              hi[3] = 0UL;

    for (p = 0UL; p < 3UL; p++)
    {
        prof_log("%s at $%08lx..$%08lx (%ld bytes)", pv_names[p],
                 (unsigned long)lo[p], (unsigned long)hi[p],
                 (long)(hi[p] - lo[p]));
    }

    if (!prof_start(PV_MAX_SAMPLES, PV_RATE))
    {
        prof_log("FATAL: prof_start: %s", prof_error());
        return(20);
    }

    prof_log("sampling from %s at %ld Hz on interrupt level %ld",
             prof_source(), (long)prof_actual_rate(), (long)prof_level());

    for (p = 0UL; p < PV_PHASES; p++)
    {
        prof_mark(pv_names[p]);
        t0 = ami_millis();

        if (p < 3UL)
        {
            switch (p)
            {
                case 0UL: pv_spin_a(pv_reps[0]); break;
                case 1UL: pv_spin_b(pv_reps[1]); break;
                default:  pv_spin_c(pv_reps[2]); break;
            }
        }
        else
        {
            /* Long enough to be comparable with spinC.  Forbid()/Permit() is
               a jump-table call into ROM either way. */
            for (i = 0UL; i < 120000UL; i++)
            {
                Forbid();
                Permit();
            }
        }

        pv_ms[p] = ami_millis() - t0;
        total_ms += pv_ms[p];
    }

    prof_mark("end");
    prof_stop();

    stored = prof_stored();
    marks  = prof_mark_table(&nmarks);
    s      = prof_buffer();

    prof_log("");
    prof_log("%ld samples, %ld interrupts, %ld from our timer, %ld dropped",
             (long)stored, (long)prof_hit_count(), (long)prof_cia_count(),
             (long)prof_drop_count());

    if (stored == 0UL || nmarks < PV_PHASES + 1UL)
    {
        prof_log("FATAL: nothing to check");
        return(20);
    }

    /* Bucket by phase, using the mark indices as the boundaries. */
    for (p = 0UL; p < PV_PHASES; p++)
    {
    ULONG from = marks[p].pm_Index;
    ULONG to   = marks[p + 1UL].pm_Index;

        for (i = from; i < to && i < stored; i++)
        {
            pv_count[p]++;
            if ((s[i].ps_SR & 0x2000U) != 0U)
            {
                pv_super[p]++;
            }
            if (hi[p] != 0UL && s[i].ps_PC >= lo[p] && s[i].ps_PC < hi[p])
            {
                pv_inrange[p]++;
            }
        }
        total_samples += pv_count[p];
    }

    prof_log("");
    prof_log("phase   ms   share    samples  share   in range   super");
    for (p = 0UL; p < PV_PHASES; p++)
    {
        prof_log("%-6s %5ld  %3ld%%     %6ld   %3ld%%   %6ld     %ld",
                 pv_names[p], (long)pv_ms[p],
                 (long)(total_ms ? (pv_ms[p] * 100UL / total_ms) : 0UL),
                 (long)pv_count[p],
                 (long)(total_samples ? (pv_count[p] * 100UL / total_samples) : 0UL),
                 (long)pv_inrange[p], (long)pv_super[p]);
    }

    prof_log("");
    prof_log("checks:");

    /* 1. Containment. */
    for (p = 0UL; p < 3UL; p++)
    {
    ULONG pct = pv_count[p] ? (pv_inrange[p] * 100UL / pv_count[p]) : 0UL;

        prof_log("  %s: %ld%% of %ld samples inside its own code",
                 pv_names[p], (long)pct, (long)pv_count[p]);
        pv_check((BOOL)(pv_count[p] >= 200UL && pct >= 90UL),
                 "sampled PC lands in the function that was running");
    }

    /* 2. Proportionality. */
    for (p = 0UL; p < PV_PHASES; p++)
    {
    LONG ts = total_ms ? (LONG)(pv_ms[p] * 1000UL / total_ms) : 0L;
    LONG ss = total_samples ? (LONG)(pv_count[p] * 1000UL / total_samples) : 0L;
    LONG d  = ts - ss;

        if (d < 0L) { d = -d; }
        prof_log("  %s: time %ld.%ld%%, samples %ld.%ld%%, delta %ld.%ld points",
                 pv_names[p], (long)(ts / 10L), (long)(ts % 10L),
                 (long)(ss / 10L), (long)(ss % 10L),
                 (long)(d / 10L), (long)(d % 10L));
        pv_check((BOOL)(d <= 40L), "sample share tracks measured wall clock");
    }

    /* 3. Rate and frame shape. */
    expect = prof_actual_rate() * total_ms / 1000UL;
    prof_log("  interrupts: %ld taken, %ld expected from %ld ms at %ld Hz",
             (long)prof_hit_count(), (long)expect, (long)total_ms,
             (long)prof_actual_rate());
    pv_check((BOOL)(expect != 0UL &&
                    prof_hit_count() > (expect - expect / 6UL) &&
                    prof_hit_count() < (expect + expect / 6UL)),
             "interrupt count matches the programmed rate");

    pv_check((BOOL)(prof_odd_formats() == 0UL),
             "every exception frame was format $0");

    pv_check((BOOL)(prof_drop_count() == 0UL), "no samples dropped");

    /* The exec phase must not be attributed to our own code at all: those
       PCs belong in ROM.  A sampler that reported them inside the caller
       would hide the Forbid()/Signal()/Wait() cost the real profile is
       hunting for. */
    {
    ULONG from = marks[3].pm_Index;
    ULONG to   = marks[4].pm_Index;
    ULONG rom  = 0UL;

        for (i = from; i < to && i < stored; i++)
        {
            if (s[i].ps_PC >= 0x00E00000UL)
            {
                rom++;
            }
        }
        prof_log("  exec: %ld of %ld samples in Kickstart",
                 (long)rom, (long)pv_count[3]);
        pv_check((BOOL)(pv_count[3] >= 100UL && rom > 0UL),
                 "Kickstart samples are recorded, not dropped");
    }

    if (!prof_write("profverify.bin"))
    {
        prof_log("WARNING: could not write profverify.bin: %s", prof_error());
    }
    else
    {
        prof_log("wrote profverify.bin (%ld samples)", (long)stored);
    }

    prof_free();

    prof_log("");
    prof_log("%ld failures, %s", (long)pv_failures,
             pv_failures == 0UL ? "PASS" : "FAIL");

    return(pv_failures == 0UL ? 0 : 20);
}
