/*
 * <devices/timer.h> for the bsdsocket host tests.
 *
 * struct timeval is DELIBERATELY not defined here: the Amiga's and POSIX's share
 * a tag with different members, and netinet/in.h needs the POSIX one.
 * host_prelude.h renames the tag once the C library's headers are parsed.
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
