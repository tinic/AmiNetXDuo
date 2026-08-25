/*
 * Does a delay in the netdev cores still take the time it asks for when the
 * CPU is fast?
 *
 *   tests/netdev/run-netdelay.sh
 *
 * WHAT THIS MEASURES THAT THE HOST TEST CANNOT.
 * src/netdev/test/test_netdev_clock.c drives netdev_wait_*() against a beam it
 * supplies itself, so it can be exact and it can be two machines at once, and
 * it is the gate that runs on every push.  What it cannot be is a real
 * chipset.  The beam here is the emulated Amiga's own, the field count is
 * whatever display mode the machine really came up in, and the CPU is a real
 * 68060 core running real 68060 code -- so this is where "the driver's idea of
 * 300 ms" is checked against a clock that is not the driver's.
 *
 * AND THE CLOCK IS timer.device, WHICH THE DRIVER ITSELF MAY NOT OPEN.  That
 * is the whole reason netdev_clock.c exists: anxnet.device is a device, some
 * of its waits are at interrupt level, and dp8390.c:57, netdev_device.c:195
 * and netdev_cmds.c:606 each explain why it holds no timer base.  A Shell
 * command is under none of those constraints.  So the driver measures itself
 * against the raster beam, this measures the driver against the E-Clock, and
 * the two clocks have nothing in common except the truth -- which is what
 * makes this an independent check rather than the driver marking its own
 * homework.
 *
 * THE ARM THAT MATTERS IS 68060.  On a 14 MHz 68020 the old counted loop and
 * the new measured wait come out at much the same length, which is why the
 * defect went unseen for as long as it did; the two only separate when the CPU
 * is fast relative to the chipset.  run-netdelay.sh therefore runs this at
 * 68020 and at 68060 and requires both, and it is the 68060 arm that goes red
 * if the wait ever goes back to counting bus reads.
 *
 * WHAT IT CANNOT SETTLE.  Whether a real accelerated Amiga -- a PiStorm32, a
 * Blizzard, an ACA, a Vampire -- behaves like the emulated 68060 here.  Nobody
 * has run this on one.  What it does settle is that the fix works on a machine
 * whose CPU is fast and whose chipset is not, which is the condition the
 * defect needs.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <devices/timer.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>

#include "netdev_clock.h"

/* The hold a PC Card's reset is documented to need, and the call
   netdev_pcmcia.c makes for it.  Both numbers are that call site's. */
#define HOLD_US         300000UL
#define HOLD_SPINS      (HOLD_US * 4UL)

/* What the hold must not come out under.  The card documentation says 100 to
   200 ms and this driver has always asked for 300; a wait that reaches the
   figure it asked for is the assertion. */
#define HOLD_FLOOR_MS   300UL

/* And what the CLOCK on its own must not exceed.  A wait twice as long as
   asked for is safe and is still not a measurement -- it is what this file's
   own clock did before it counted the lines in a field.  This ceiling is
   asserted only against the arm that passes NO floor, because a floor is an
   iteration count and on a machine whose bus cycles are slow the count is
   legitimately the longer of the two: the guarantee is that no machine waits
   LESS than it used to, and honouring it can only ever make a wait longer. */
#define CLOCK_CEIL_MS   450UL

/* proto/timer.h declares TimerBase itself; this is the definition. */
struct Device *TimerBase;
static struct MsgPort  *tport;
static struct timerequest treq;

static LONG checks, failures;

static VOID check(CONST_STRPTR what, BOOL ok, LONG detail)
{
    checks++;
    if (!ok)
        failures++;

    Printf((CONST_STRPTR)"%s %s (%ld)\n",
           (CONST_STRPTR)(ok ? "ok  " : "FAIL"), what, detail);
}

/* ------------------------------------------------------------ the clock --- */

static ULONG eclock_hz;

static BOOL clock_open(VOID)
{
    tport = CreateMsgPort();
    if (tport == NULL)
        return FALSE;

    treq.tr_node.io_Message.mn_Node.ln_Type = NT_REPLYMSG;
    treq.tr_node.io_Message.mn_ReplyPort    = tport;

    if (OpenDevice((CONST_STRPTR)"timer.device", UNIT_ECLOCK,
                   (struct IORequest *)&treq, 0) != 0)
        return FALSE;

    TimerBase = treq.tr_node.io_Device;
    return TRUE;
}

static VOID clock_close(VOID)
{
    if (TimerBase != NULL)
    {
        CloseDevice((struct IORequest *)&treq);
        TimerBase = NULL;
    }
    if (tport != NULL)
    {
        DeleteMsgPort(tport);
        tport = NULL;
    }
}

static ULONG eclock_low(VOID)
{
    struct EClockVal ev;

    eclock_hz = ReadEClock(&ev);
    return ev.ev_lo;
}

/* Milliseconds between two low-word samples.  The E-Clock is about 709 kHz,
   so the low word alone wraps every 6053 seconds and nothing here runs for
   anything like that; the subtraction is unsigned and survives one wrap. */
static ULONG eclock_ms(ULONG from, ULONG to)
{
    ULONG ticks = to - from;

    if (eclock_hz == 0UL)
        return 0UL;

    return (ULONG)((ticks / (eclock_hz / 1000UL)));
}

/* ------------------------------------------------------- the two shapes --- */

/*
 * The loop body both arms spin on: a read of VHPOSR, which is a real chip-bus
 * cycle on every Amiga and is safe to read with nothing plugged into anything.
 * The driver's own loops read a PCMCIA attribute or a card register, which are
 * bus cycles of the same order.  Using the same body in both arms is what
 * makes the two numbers comparable, and using a BUS access rather than a
 * register-only spin is the conservative choice: it understates how far the
 * counted loop collapses on a fast CPU rather than overstating it.
 */
static volatile UWORD *const beam = (volatile UWORD *)0x00DFF006UL;

/* THE FIX.  `spins` is the floor the old call site's count becomes; pass 0 to
   time the clock on its own, or HOLD_SPINS to run exactly what
   netdev_pcmcia.c's pc_settle() runs. */
static ULONG timed_hold_ms(ULONG spins)
{
    NetdevWait w;
    ULONG      t0;

    t0 = eclock_low();

    netdev_wait_begin(&w, HOLD_US, spins);
    do
        (VOID)*beam;
    while (!netdev_wait_done(&w));

    return eclock_ms(t0, eclock_low());
}

/* THE OLD CODE: the same call site as it was written before netdev_clock.c.
   Reported, never asserted -- what this loop does depends on the cost of a bus
   cycle on the machine, and the point of the exercise is that the cost of a
   bus cycle is not a thing the driver is entitled to assume. */
static ULONG counted_hold_ms(VOID)
{
    ULONG t0 = eclock_low();
    ULONG n  = HOLD_US * 4UL;

    while (n-- != 0UL)
        (VOID)*beam;

    return eclock_ms(t0, eclock_low());
}

/* ---------------------------------------------------------------- main ---- */

int main(VOID)
{
    ULONG spins, us_line, field, clock_only, at_site, counted;

    Printf((CONST_STRPTR)"netdelay: the driver's wait against the E-Clock\n\n");

    if (!clock_open())
    {
        Printf((CONST_STRPTR)"netdelay: no timer.device, nothing to measure "
                             "against\n0 checks, 1 failures, FAIL\n");
        clock_close();
        return RETURN_ERROR;
    }

    (VOID)eclock_low();
    Printf((CONST_STRPTR)"E-Clock %lu Hz\n", eclock_hz);
    check((CONST_STRPTR)"the E-Clock is a plausible Amiga one",
          eclock_hz > 600000UL && eclock_hz < 800000UL, (LONG)eclock_hz);

    /* ---- what the driver measured about this machine -------------------- */

    spins   = netdev_clock_spins_per_line();
    us_line = netdev_clock_us_per_line();
    field   = netdev_clock_lines_per_field();

    Printf((CONST_STRPTR)"beam: %lu spin(s) per raster line, %lu line(s) to a "
                         "field, a line costed at %lu us\n",
           spins, field, us_line);

    check((CONST_STRPTR)"the beam was found and is running", spins != 0UL,
          (LONG)spins);

    /*
     * 63 is any 15 kHz mode and 31 is any 31 kHz one; 30 is the fallback the
     * driver uses when counting the lines in a field gave a length no Amiga
     * display mode has.  On a real chipset it should never be the fallback,
     * and that is worth failing on here even though the driver treats it as
     * merely imprecise: this is the one place with a real beam to check it
     * against.
     */
    check((CONST_STRPTR)"the field count recognised a real display mode",
          us_line == 63UL || us_line == 31UL, (LONG)us_line);

    /* ---- the hold ------------------------------------------------------- */

    /*
     * THE CLOCK ON ITS OWN, with no floor under it.  This is the arm that says
     * whether a wait MEASURES TIME: 300 ms asked for, 300 ms taken, on
     * whatever CPU this is.  Both ends are asserted, because a wait that comes
     * out at twice the request is safe and is still not a measurement.
     */
    clock_only = timed_hold_ms(0UL);
    Printf((CONST_STRPTR)"\n300 ms from the clock alone: %lu ms\n", clock_only);

    check((CONST_STRPTR)"the clock reaches the 300 ms it was asked for",
          clock_only >= HOLD_FLOOR_MS, (LONG)clock_only);
    check((CONST_STRPTR)"and does not overshoot it",
          clock_only <= CLOCK_CEIL_MS, (LONG)clock_only);

    /*
     * AND THE CALL SITE AS IT REALLY RUNS, floor and all.  Only the minimum is
     * asserted here.  The floor is the iteration count the old code used, kept
     * so that no machine gets a shorter wait than it did before; on a machine
     * whose bus cycles are slow that count is the longer of the two and the
     * hold legitimately runs well past 300 ms, exactly as it always did.
     */
    at_site = timed_hold_ms(HOLD_SPINS);
    Printf((CONST_STRPTR)"the pc_settle() call site, floor and all: %lu ms\n",
           at_site);

    check((CONST_STRPTR)"the call site never comes out under 300 ms",
          at_site >= HOLD_FLOOR_MS, (LONG)at_site);

    counted = counted_hold_ms();
    Printf((CONST_STRPTR)"the same hold as a counted loop: %lu ms\n", counted);
    Printf((CONST_STRPTR)"  (reported, not asserted -- see the file header)\n");

    clock_close();

    Printf((CONST_STRPTR)"\n%ld checks, %ld failures, %s\n", checks, failures,
           (CONST_STRPTR)(failures == 0 ? "PASS" : "FAIL"));

    return (failures == 0) ? RETURN_OK : RETURN_ERROR;
}
