/*
 * <devices/timer.h> for the src/common host tests.
 *
 * struct timeval IS defined here, unlike the bsdsocket shim, which leaves it
 * out on purpose because netinet/in.h wants the POSIX one and the two share a
 * tag.  Nothing in src/common includes a socket header, so the Amiga shape is
 * unambiguous and gather_clock() can read it.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AMINETXDUO_COMMON_TEST_DEVICES_TIMER_H
#define AMINETXDUO_COMMON_TEST_DEVICES_TIMER_H

#include <exec/types.h>

struct timeval
{
    ULONG tv_secs;
    ULONG tv_micro;
};

struct EClockVal
{
    ULONG ev_hi;
    ULONG ev_lo;
};

#define TIMERNAME      "timer.device"
#define UNIT_VBLANK    1
#define UNIT_MICROHZ   0

#endif
