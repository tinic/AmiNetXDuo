/*
 * anxnet.device: a wait that measures time instead of counting bus reads.
 *
 * Every delay in this driver used to be a counted loop over a bus access, and
 * every one of them was written as if the count were a duration:
 *
 *     ULONG n = us * 4u;                  -- "four reads to the microsecond"
 *     while (n-- != 0) (VOID)*attr;
 *
 * That is not a measure of time.  It is a measure of how many times this CPU
 * can go round a loop, and the answer differs by orders of magnitude across
 * the machines this driver runs on.  On the 14 MHz 68020 the numbers were
 * chosen for, the arithmetic comes out close enough.  On an accelerated Amiga
 * -- PiStorm, Blizzard, ACA, Vampire -- the same loop finishes in some
 * fraction of the intended time, and the delays that matter here are the ones
 * that hold hardware in reset.  The Gayle hold is 300 ms because CardReset's
 * documentation is blunt that "PC Cards require the reset time 100 or 200 mS";
 * a hold that comes out under that is out of the card's specification, and a
 * card whose power-up sequence half-ran is one whose registers still answer
 * and whose receiver may not come up.  NOBODY HAS MEASURED THAT HAPPENING:
 * this was found by reading the code during an accelerator report whose cause
 * turned out to be somewhere else entirely, and no machine here has an
 * accelerator to reproduce it on.  It is fixed because a documented minimum
 * implemented as an iteration count is not a minimum, whoever it bites.
 *
 * The fix is to measure elapsed time, from a clock that is not the CPU.
 *
 * WHICH CLOCK.  Not timer.device: this is a device, some of these waits are on
 * paths that run before anything of ours is open, and the driver's refusal to
 * hold a timer base is load-bearing elsewhere.  Not the vertical-blank server
 * in netdev_device.c either: it is added when a unit is first opened, and the
 * longest wait here -- the Gayle reset hold -- runs during the claim, before
 * any unit exists to open.  What is left is the one clock on an Amiga that
 * needs nothing opened, nothing owned, and no context: the raster beam.
 * VHPOSR at $DFF006 is a free-running counter driven by the chipset's own
 * oscillator, one word read away, legal at task level, at interrupt level and
 * under Disable(), and completely untouched by how fast the CPU is.  Its high
 * byte is the vertical position, which advances once per scan line.
 * netdev_device.c already reads it for exactly this reason, under
 * -DAMINETXDUO_NETDEV_TIME.
 *
 * WHAT A LINE IS WORTH.  A scan line is 64 us on a 15 kHz PAL or NTSC display
 * and 32 us on the 31 kHz multiscan modes an AGA machine can be in.  That is
 * the whole range: no Amiga display mode has a line rate above about 31.5 kHz.
 * A line is therefore worth AT LEAST 30 us, and a wait costed at 30 us to the
 * line is never short.  On the ordinary 15 kHz machine it comes out about
 * twice the requested time, which is spent once per claim on paths that
 * already take hundreds of milliseconds by design.  This is an assumption
 * about the chipset, which varies by a factor of two, in place of an
 * assumption about the CPU, which varies by a factor of a hundred.
 *
 * NEVER SHORTER THAN IT WAS.  Every wait also carries the iteration count the
 * old code used, and it ends only when BOTH the count is exhausted and the
 * measured time has passed.  On the 14 MHz machine these delays were tuned on,
 * the count is the slower of the two and nothing changes at all.  On a fast
 * machine the count evaporates and the clock decides.  A machine with no
 * readable beam -- the host test builds, and anything else that is not a
 * 68k Amiga -- falls back to the count alone, which is exactly today's
 * behaviour.
 *
 * WHAT IS DELIBERATELY NOT CONVERTED.  A short spin over a bus access is not
 * always a delay: dp8390.c's dp_pause() and the sub-millisecond arms of
 * ne_delay() are barriers, N real bus cycles between two register writes, and
 * a bus cycle does not get faster because the CPU did.  Those stay counts, and
 * waits below NETDEV_WAIT_MIN_US never consult the clock at all -- which also
 * keeps the clock off the interrupt path, where the calibration below must
 * never run.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETDEV_CLOCK_H
#define AMINETXDUO_NETDEV_CLOCK_H

#include <exec/types.h>

/*
 * Below this, a wait is a bus barrier and the caller's own count is the whole
 * of it.  Four raster lines: the clock cannot resolve less, and nothing
 * shorter than this is asking for a duration in the first place.
 */
#define NETDEV_WAIT_MIN_US  250u

typedef struct NetdevWait
{
    ULONG   nw_Spins;   /* iterations still owed to the caller's own count  */
    ULONG   nw_Lines;   /* raster lines still owed to the measured clock    */
    ULONG   nw_Cap;     /* iterations before the clock is given up on       */
    UWORD   nw_Beam;    /* vertical position when the last line was counted */
    UBYTE   nw_Timed;   /* the clock is in play for this wait               */
    UBYTE   nw_Tick;    /* iterations left before the beam is read again    */
} NetdevWait;

/*
 * Arm a wait for `us` microseconds that will not end before `spins`
 * iterations of the caller's loop have also run.  `spins` is the count the
 * call site used before there was a clock; it is the floor, not the duration.
 */
VOID netdev_wait_begin(NetdevWait *w, ULONG us, ULONG spins);

/*
 * One iteration accounted for.  TRUE when the wait is over.  The caller's own
 * bus access stays in the loop body -- it is what paces the bus, and on the
 * PCMCIA slot it is also what keeps the socket's access timing honest -- but
 * it no longer decides how long the loop runs.
 */
BOOL netdev_wait_done(NetdevWait *w);

/*
 * How many iterations of a bare spin fit in one raster line on this machine,
 * measured once against the beam, or 0 if there is no beam to measure
 * against.  Nothing uses it to decide anything; it is published through the
 * probe record so that CheckNetDevice can say out loud how fast the CPU under
 * this driver is, which is the one number that separates a stock A1200 from an
 * accelerated one and explains every timing failure that follows.
 */
ULONG netdev_clock_spins_per_line(VOID);

#endif /* AMINETXDUO_NETDEV_CLOCK_H */
