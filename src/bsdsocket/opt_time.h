/*
 * bsdsocket.library socket-option time conversions.
 *
 * Kept in a header so the host socket-option regression exercises the same
 * arithmetic that sets NetX Duo's TCP_USER_TIMEOUT field.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_BSDSOCKET_OPT_TIME_H
#define AMINETXDUO_BSDSOCKET_OPT_TIME_H

#include <exec/types.h>

/* Milliseconds -> ticks, rounded up without forming ms * rate.  The latter
   overflows a 32-bit ULONG at 85,899,326 ms with the Amiga's 50 Hz tick even
   though the result (4,294,967 ticks) is nowhere near ULONG_MAX. */
static ULONG bsd_ms_ticks(ULONG ms, ULONG rate)
{
    ULONG whole;
    ULONG fraction;

    whole = (ms / 1000UL) * rate;
    fraction = ((ms % 1000UL) * rate + 999UL) / 1000UL;

    return whole + fraction;
}

#endif /* AMINETXDUO_BSDSOCKET_OPT_TIME_H */
