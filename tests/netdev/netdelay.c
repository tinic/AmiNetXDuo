/*
 * Does a delay in the netdev cores still take the time it asks for when the
 * CPU is fast?
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

static ULONG counted_hold_ms(VOID)
{
    ULONG t0 = eclock_low();
    ULONG n  = HOLD_US * 4UL;

    while (n-- != 0UL)
        (VOID)*beam;

    return eclock_ms(t0, eclock_low());
}

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

    spins   = netdev_clock_spins_per_line();
    us_line = netdev_clock_us_per_line();
    field   = netdev_clock_lines_per_field();

    Printf((CONST_STRPTR)"beam: %lu spin(s) per raster line, %lu line(s) to a "
                         "field, a line costed at %lu us\n",
           spins, field, us_line);

    check((CONST_STRPTR)"the beam was found and is running", spins != 0UL,
          (LONG)spins);

    check((CONST_STRPTR)"the field count recognised a real display mode",
          us_line == 63UL || us_line == 31UL, (LONG)us_line);

    clock_only = timed_hold_ms(0UL);
    Printf((CONST_STRPTR)"\n300 ms from the clock alone: %lu ms\n", clock_only);

    check((CONST_STRPTR)"the clock reaches the 300 ms it was asked for",
          clock_only >= HOLD_FLOOR_MS, (LONG)clock_only);
    check((CONST_STRPTR)"and does not overshoot it",
          clock_only <= CLOCK_CEIL_MS, (LONG)clock_only);

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
