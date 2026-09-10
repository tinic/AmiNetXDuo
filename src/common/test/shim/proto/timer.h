/*
 * timer.device, for the src/common host tests.  The test binary defines
 * these: what ami_random.c does with a clock is mix it into a pool, so the
 * values only have to move, and the test is what makes them move in a way it
 * can predict.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AMINETXDUO_COMMON_TEST_PROTO_TIMER_H
#define AMINETXDUO_COMMON_TEST_PROTO_TIMER_H

#include <exec/types.h>
#include <devices/timer.h>

extern struct Library *TimerBase;

ULONG ReadEClock(struct EClockVal *dest);
VOID  GetSysTime(struct timeval *dest);

#endif
