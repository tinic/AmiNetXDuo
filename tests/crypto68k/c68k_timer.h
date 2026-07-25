/*
 * AmiNetXDuo -- crypto68k test: E-Clock timing.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_C68K_TIMER_H
#define AMINETXDUO_C68K_TIMER_H

#include <exec/types.h>

BOOL  c68k_timer_open(VOID);
VOID  c68k_timer_close(VOID);

/* Raw 32-bit E-Clock low word; wraps every ~100 minutes, and a difference
   stays correct across a single wrap. */
ULONG c68k_eclock(VOID);
ULONG c68k_eclock_hz(VOID);

/* Tick delta to milliseconds, and to microseconds.  Microseconds in a ULONG
   top out at 4,295 seconds, which is longer than anything measured here. */
ULONG c68k_eclock_millis(ULONG ticks);
ULONG c68k_eclock_micros(ULONG ticks);

#endif /* AMINETXDUO_C68K_TIMER_H */
