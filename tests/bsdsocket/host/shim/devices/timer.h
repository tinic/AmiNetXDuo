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
 * So the host keeps its own, and tr_time is spelled out in two ULONGs to hold
 * the size of AmiSocketBase steady.  The consequence is the boundary of what
 * this shim supports: options.c, select.c and tcp_handler.c read tv_secs and
 * tv_micro and will not compile against it.  Nothing else in src/bsdsocket
 * touches either name.
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

#define UNIT_VBLANK    1
#define UNIT_MICROHZ   0
#define TR_ADDREQUEST  9
#define TR_GETSYSTIME 11
#endif
