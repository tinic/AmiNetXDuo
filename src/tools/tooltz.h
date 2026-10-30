/*
 * The time zone the Locale preferences show, in minutes west of Greenwich,
 * and the local time sntp sets from UTC.  No Amiga headers, so the host test
 * can compile it.
 *
 * loc_GMTOffset is minutes WEST of Greenwich: the Locale editor saves
 * Central Europe (UTC+1) as -60 and its own default, United States
 * ("5 Hours from GMT", UTC-5), as 300.  With no ENV:Sys/locale.prefs,
 * locale.library 40 (3.1) and 47 (3.2) report -300 instead, the opposite
 * sign of the zone the editor shows in that case, so the editor's 300 is
 * used there.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_TOOLTZ_H
#define AMINETXDUO_TOOLTZ_H

/* What the Locale editor shows when nothing was ever saved. */
#define TOOLTZ_DEFAULT_WEST 300L

/* loc_GMTOffset as read, and whether ENV:Sys/locale.prefs exists. */
long tooltz_minutes_west(long loc_gmtoffset, int prefs_loaded);

/* Local seconds from UTC seconds, both Amiga epoch.  A result outside
   32 bits leaves the UTC value, as before. */
unsigned long tooltz_local_secs(unsigned long utc_secs, long minutes_west);

#endif
