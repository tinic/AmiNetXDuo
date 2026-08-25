/*
 * anxnet.device: a wait that measures elapsed time instead of counting bus
 * reads, from the raster beam (VPOSR/VHPOSR at $DFF004, read as one longword),
 * the one clock that needs nothing opened and is untouched by CPU speed.
 *
 * Every wait also carries the caller's old iteration count as an unconditional
 * floor.  Waits below NETDEV_WAIT_MIN_US never consult the clock, which keeps
 * the calibration off the interrupt path.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETDEV_CLOCK_H
#define AMINETXDUO_NETDEV_CLOCK_H

#include <exec/types.h>

/*
 * Below this, a wait is a bus barrier and the caller's own count is the whole
 * of it.  Four raster lines: the clock cannot resolve less.
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
 * neither band.  Under the shortest line any Amiga display mode produces, so a
 * wait costed at it is never short on any machine.
 */
#define NETDEV_LINE_FLOOR_US    30u

/*
 * Arm a wait for `us` microseconds that will not end before `spins` iterations
 * of the caller's loop have also run.  `spins` is the floor, not the duration.
 */
VOID netdev_wait_begin(NetdevWait *w, ULONG us, ULONG spins);

/*
 * One iteration accounted for.  TRUE when the wait is over.  The caller's own
 * bus access stays in the loop body -- it is what paces the bus -- but it no
 * longer decides how long the loop runs.
 */
BOOL netdev_wait_done(NetdevWait *w);

/*
 * How many iterations of a bare spin fit in one raster line on this machine, or
 * 0 if there is no beam to measure against.  Decides nothing; published through
 * the probe record so CheckNetDevice can report how fast this CPU is.
 */
ULONG netdev_clock_spins_per_line(VOID);

/*
 * What this machine's scan line was costed at, in microseconds: 63 in the
 * 15 kHz modes, 31 in the 31 kHz ones, NETDEV_LINE_FLOOR_US when neither band
 * was recognised, 0 when there is no beam at all.
 */
ULONG netdev_clock_us_per_line(VOID);

/*
 * The raw field length the costing came from, in scan lines, or 0 if counting
 * one did not produce a length.  It tells "a display mode I do not know" apart
 * from "I could not count a field here" -- same fallback, different problems.
 */
ULONG netdev_clock_lines_per_field(VOID);

#endif /* AMINETXDUO_NETDEV_CLOCK_H */
