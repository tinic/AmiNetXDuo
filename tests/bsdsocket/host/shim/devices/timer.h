/*
 * <devices/timer.h> for the bsdsocket host tests.
 *
 * struct timeval is DELIBERATELY not defined here.  The Amiga's is
 * { ULONG tv_secs; ULONG tv_micro; } and POSIX's is
 * { time_t tv_sec; suseconds_t tv_usec; }: different member names, different
 * types, same tag.  Defining ours would collide with the one <sys/select.h>
 * has already brought in, and defining neither is not an option because
 * netinet/in.h needs the POSIX one.
 *
 * host_prelude.h resolves it by renaming the tag once the C library's headers
 * are parsed, so tr_time below is the Amiga's two ULONGs and the C library
 * keeps its own.  tv_secs and tv_micro are therefore readable here, which is
 * what lets options.c and select.c compile in this tier.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AMINETXDUO_BSD_TEST_DEVICES_TIMER_H
#define AMINETXDUO_BSD_TEST_DEVICES_TIMER_H
#include <exec/types.h>
#include <exec/io.h>
#include <exec/ports.h>

struct timerequest {
    struct IORequest tr_node;
    struct timeval   tr_time;      /* host_prelude.h renamed the tag */
};

#define TIMERNAME      "timer.device"
#define UNIT_VBLANK    1
#define UNIT_MICROHZ   0
#define TR_ADDREQUEST  9
#define TR_GETSYSTIME 11
#endif
