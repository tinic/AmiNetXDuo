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
 * oscillator, one longword read away, legal at task level, at interrupt level
 * and under Disable(), and completely untouched by how fast the CPU is.  The
 * longword at $DFF004 is VPOSR and VHPOSR together, and the vertical position
 * they carry between them advances once per scan line.  netdev_device.c
 * already reads the beam for exactly this reason, under
 * -DAMINETXDUO_NETDEV_TIME.
 *
 * WHAT A LINE IS WORTH, AND WHY IT IS MEASURED RATHER THAN ASSUMED.  A wait
 * here is counted in scan lines, so the driver has to know what a line costs.
 * There are only two answers on an Amiga.  Every 15 kHz mode -- which is
 * nearly every machine, nearly all the time -- has a line of about 64 us, and
 * PAL and NTSC differ by under one percent of it because the two master clocks
 * do; every 31 kHz multiscan mode has a line of about 32 us.  Nothing else
 * exists: no Amiga display mode has a line rate above about 31.5 kHz.
 *
 * So the question is not "how long is a line", it is "which of the two is this
 * machine in", and that is one measurement: how far down the screen the beam
 * gets before it starts again.  A 15 kHz field is 262 or 313 lines and a
 * 31 kHz field is 524 or 625, so the two bands do not come close to touching,
 * and the field RATE never has to be known at all.  It costs four fields --
 * 80 ms, once, on the claim path, against a reset hold on the same path that
 * is 300 ms by design.
 *
 * The costing is deliberately UNDER the truth in both bands, 63 us against
 * 63.56 and 31 us against 31.75, so that a wait is long by a fraction of a
 * percent rather than ever being short by one.  A field length in neither band
 * means something is not a beam, and the fallback is 30 us -- under the
 * shortest line any Amiga produces, and therefore never short whatever the
 * machine turns out to be.  And under all of it sits the floor below, which is
 * what makes even a wrong answer here safe.
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
 * The fallback cost of a scan line, used when the field measurement lands in
 * neither band and the driver has to assume rather than know.  Under the
 * shortest line any Amiga display mode produces, so a wait costed at it is
 * never short on any machine.
 */
#define NETDEV_LINE_FLOOR_US    30u

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

/*
 * What this machine's scan line was costed at, in microseconds: 63 on the
 * 15 kHz modes, 31 on the 31 kHz ones, NETDEV_LINE_FLOOR_US when the field
 * measurement recognised neither and the driver fell back to assuming.  Zero
 * when there is no beam at all.  Published beside the figure above so that a
 * machine which times its waits wrongly can be told apart from one that cannot
 * time them at all.
 */
ULONG netdev_clock_us_per_line(VOID);

/*
 * The raw field length the costing above came from, in scan lines, or 0 if
 * counting one did not produce a length at all.  It decides nothing on its
 * own; it is what tells "this machine is in a display mode I do not know"
 * apart from "I could not count a field here", which are the same 30 us
 * fallback and very different problems.
 */
ULONG netdev_clock_lines_per_field(VOID);

#endif /* AMINETXDUO_NETDEV_CLOCK_H */
