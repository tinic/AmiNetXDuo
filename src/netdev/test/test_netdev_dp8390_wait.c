/*
 * The overwrite stop delay, against the real clock.
 *
 * test_netdev_dp8390.c stubs netdev_wait_begin/done and floor_spins, so it
 * proves only that dp8390_overwrite() hands the right arguments to a wait.  It
 * cannot prove the wait the driver actually links -- netdev_clock.c -- behaves
 * for the overwrite's 1.6 ms.  This file links the REAL netdev_clock.c and
 * drives dp8390_overwrite() through the three beam shapes a field can be in:
 *
 *   - timed:     the beam moves.  floor_spins() prices the wait from the
 *                measured per-line work, so the floor scales with the CPU and
 *                the wait spans the 1.6 ms beam bound, never the fixed 6400.
 *   - untimed:   no beam (the host, or the old driver's view).  floor_spins()
 *                answers with the caller's fallback and the wait runs the whole
 *                fallback as CR reads.
 *   - stalled:   the beam measures and then stops.  The wait must end through
 *                its cap rather than hang.
 *
 * The beam is driven exactly as test_netdev_clock.c drives it: NETDEV_CLOCK_TEST
 * makes netdev_clock.c read the beam through netdev_clock_test_vpos(), defined
 * here, and every register read advances the machine's clock by one bus cycle
 * so a wait's length is visible as beam lines.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include <exec/types.h>

#include "netdev_nic.h"
#include "netdev_clock.h"

/* --------------------------------------------------------- the machine ---- */

/*
 * One simulated machine: how many ticks of its clock a scan line lasts, and how
 * many lines its display mode puts in a field.  A stock 68020 gets round a few
 * hundred reads to the line, an accelerator tens of thousands.
 */
static ULONG mach_ticks;
static ULONG mach_ticks_per_line   = 256;
static ULONG mach_lines_per_field  = 313;
static ULONG beam_reads;
static int   beam_stuck;
static UWORD beam_frozen;

static ULONG mach_lines(void)
{
    return mach_ticks / mach_ticks_per_line;
}

/* Reading a register costs a bus cycle like anything else does. */
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

/* A machine with no beam at all, what netdev_clock.c sees on the host. */
static void machine_no_beam(void)
{
    machine(256, 313);
    beam_stuck  = 1;
    beam_frozen = 0;
}

/* ------------------------------------------------------------ the chip ---- */

#define PAGES         4
#define REGS          16
#define CHIP_ISR_REG  0x07u  /* ED_P0_ISR / ED_P1_CURR, before dp8390reg.h */

#define CR_STP_BIT  0x01u    /* ED_CR_STP */
#define CR_TXP_BIT  0x04u    /* ED_CR_TXP */

static UBYTE chip[PAGES][REGS];
static UBYTE chip_page;
static UBYTE chip_cr;
static UBYTE chip_txing;     /* CR.TXP's live read-back, beside the bank */
static ULONG cr_reads;       /* register-0 reads, the wait loop's bus cycles */

/*
 * CR is register 0 in every bank and selects the bank, so it is held outside
 * them.  The ISR is write-one-to-clear; the rest store plainly.
 */
static void chip_put(const NetdevNic *n, UWORD reg, UBYTE val)
{
    UBYTE r = (UBYTE)(reg & 0x0fu);

    (VOID)n;

    if (r == 0)
    {
        chip_cr   = val;
        chip_page = (UBYTE)((val >> 6) & 0x03u);
        if ((val & CR_TXP_BIT) != 0)
            chip_txing = 1;
        if ((val & CR_STP_BIT) != 0)
            chip_txing = 0;
        return;
    }

    if (chip_page == 0 && r == CHIP_ISR_REG)
        chip[0][r] &= (UBYTE)~val;
    else
        chip[chip_page][r] = val;
}

static UBYTE chip_get(const NetdevNic *n, UWORD reg)
{
    UBYTE r = (UBYTE)(reg & 0x0fu);

    (VOID)n;

    mach_ticks++;                /* a register read costs a bus cycle */
    if (r == 0)
        cr_reads++;

    return (r == 0)
           ? (UBYTE)(chip_cr | (chip_txing ? CR_TXP_BIT : 0))
           : chip[chip_page][r];
}

#define NIC_GET(nic, reg)       chip_get((nic), (UWORD)(reg))
#define NIC_PUT(nic, reg, val)  chip_put((nic), (UWORD)(reg), (UBYTE)(val))

#include "dp8390.c"

/* ------------------------------------------------------------ the checks --- */

static int failures;
static int checks;

static void expect(int ok, const char *what)
{
    checks++;
    if (ok)
        return;

    printf("FAIL %s\n", what);
    failures++;
}

static void expect_hex(const char *what, unsigned long got, unsigned long want)
{
    checks++;
    if (got == want)
        return;

    printf("FAIL %s: got %lu, want %lu\n", what, got, want);
    failures++;
}

static void expect_at_least(const char *what, unsigned long got,
                            unsigned long want)
{
    checks++;
    if (got >= want)
        return;

    printf("FAIL %s: got %lu, want at least %lu\n", what, got, want);
    failures++;
}

/* ------------------------------------------------------------ fixture ----- */

static NetdevNic nic;

static void reset_chip(void)
{
    memset(chip, 0, sizeof(chip));
    memset(&nic, 0, sizeof(nic));
    chip_cr   = 0;
    chip_page = 0;
    chip_txing = 0;
    cr_reads   = 0;

    /* An NE2000's geometry, just enough for the overwrite path. */
    nic.cr_proto       = ED_CR_RD2;
    nic.mem_size       = 16384;
    nic.tx_page_start  = 0x40;
    nic.rec_page_start = (UBYTE)(0x40 + 2 * ED_TXBUF_SIZE);
    nic.rec_page_stop  = 0x80;
    nic.txb_cnt        = 2;
    nic.txb_inuse      = 0;      /* nothing owned: no resend, ever */
    nic.txb_next_tx    = 0;
    nic.next_packet    = (UWORD)nic.rec_page_start;
    nic.resets         = 0;

    /* An empty ring: CURR equals next_packet, so rint hands nothing up and
       overwrite returns FALSE (no reset, no resend). */
    chip[1][ED_P1_CURR] = (UBYTE)nic.next_packet;
}

/* ------------------------------------------------------------ the arms ---- */

#define REFERENCE_TICKS_PER_LINE     256u
#define SLOW_TICKS_PER_LINE          32u
#define OVW_LINES   ((DP8390_OVW_STOP_WAIT_US + 62u) / 63u)  /* ceil(1600/63) */

static void timed_wait_uses_the_measured_floor(void)
{
    ULONG spins_line;
    ULONG floor_ref;
    ULONG start;
    ULONG lines;
    BOOL  rc;

    /* Beam up: the floor is priced from the measured per-line work, not the
       6400 fallback. */
    machine(REFERENCE_TICKS_PER_LINE, 313);
    spins_line = netdev_clock_spins_per_line();
    floor_ref  = netdev_clock_floor_spins(DP8390_OVW_STOP_WAIT_US,
                                          DP8390_OVW_STOP_SPINS);
    expect_hex("timed floor is the measured floor, reference",
               floor_ref, spins_line * OVW_LINES);
    expect(floor_ref != DP8390_OVW_STOP_SPINS, "not the fixed 6400 fallback");

    /* Drive the overwrite and watch the beam: it is ~1.6 ms of beam time, not
       the fallback's iteration count.  A lower bound is all a beam-line count
       can assert here, and it cannot tell the measured floor from the 6400
       fallback on this machine -- both are about 26 lines. */
    machine(REFERENCE_TICKS_PER_LINE, 313);
    (VOID)netdev_clock_us_per_line();   /* measure before the clock starts */
    reset_chip();
    start = mach_lines();
    rc = dp8390_overwrite(&nic, ED_ISR_OVW);
    lines = mach_lines() - start;
    expect(rc == FALSE, "a timed overwrite completes without a reset");
    expect_at_least("and the wait spans the 1.6 ms beam bound",
                    lines, DP8390_OVW_STOP_WAIT_US / 63u);

    /*
     * THE OVER-SPIN GATE the lower bound cannot give.  On a slow bus -- 32
     * reads to the line, the A3000's end of the range -- the measured floor
     * is 26 * 32 = 832 reads and the wait is ~32 lines; a reverted fixed 6400
     * floor runs 6400 reads and ~250 lines.  The upper bound names which
     * floor the wait used, where the reference machine cannot tell them
     * apart.
     */
    machine(SLOW_TICKS_PER_LINE, 313);
    (VOID)netdev_clock_us_per_line();
    reset_chip();
    start = mach_lines();
    rc = dp8390_overwrite(&nic, ED_ISR_OVW);
    lines = mach_lines() - start;
    expect(rc == FALSE, "a slow-bus overwrite completes without a reset");
    expect(lines <= 3u * OVW_LINES, "and the wait is not over-spun");
}

static void untimed_wait_runs_the_fallback(void)
{
    BOOL rc;

    machine_no_beam();
    expect_hex("no beam keeps the 6400 fallback",
               netdev_clock_floor_spins(DP8390_OVW_STOP_WAIT_US,
                                        DP8390_OVW_STOP_SPINS),
               DP8390_OVW_STOP_SPINS);

    reset_chip();
    rc = dp8390_overwrite(&nic, ED_ISR_OVW);
    expect(rc == FALSE, "an untimed overwrite completes without a reset");
    /* One CR read to snapshot TXP, the fallback reads in the wait loop, and
       one CR read each in rint's pause and the trailing pause. */
    expect_hex("and the wait runs the whole fallback as CR reads",
               cr_reads, DP8390_OVW_STOP_SPINS + 3u);
}

static void a_stalled_beam_ends_via_the_cap(void)
{
    BOOL rc;

    machine(REFERENCE_TICKS_PER_LINE, 313);
    (VOID)netdev_clock_us_per_line();   /* measure, then the beam stops */
    beam_stuck  = 1;
    beam_frozen = 7;

    reset_chip();
    rc = dp8390_overwrite(&nic, ED_ISR_OVW);
    expect(rc == FALSE, "a stalled beam ends the wait through its cap");
    expect(cr_reads < 200000u, "and does not spin without bound");
}

int main(void)
{
    timed_wait_uses_the_measured_floor();
    untimed_wait_runs_the_fallback();
    a_stalled_beam_ends_via_the_cap();

    if (failures != 0)
    {
        printf("netdev_dp8390_wait: %d of %d checks failed\n",
               failures, checks);
        return 1;
    }

    printf("netdev_dp8390_wait: %d checks ok\n", checks);

    return 0;
}
