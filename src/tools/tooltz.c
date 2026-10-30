/*
 * The time zone the Locale preferences show.  See tooltz.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tooltz.h"

long tooltz_minutes_west(long loc_gmtoffset, int prefs_loaded)
{
    return prefs_loaded ? loc_gmtoffset : TOOLTZ_DEFAULT_WEST;
}

unsigned long tooltz_local_secs(unsigned long utc_secs, long minutes_west)
{
    /* 32-bit arithmetic: no 64-bit helpers in a 68000 command. */
    unsigned long shift = (unsigned long)(minutes_west < 0 ? -minutes_west
                                                           : minutes_west)
                          * 60UL;

    if (minutes_west >= 0)
        return (utc_secs >= shift) ? utc_secs - shift : utc_secs;
    return (utc_secs <= 0xffffffffUL - shift) ? utc_secs + shift : utc_secs;
}
