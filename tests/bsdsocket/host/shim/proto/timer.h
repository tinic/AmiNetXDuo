/* <proto/timer.h> for the bsdsocket host tests.
   SPDX-License-Identifier: MIT */
#ifndef AMINETXDUO_BSD_TEST_PROTO_TIMER_H
#define AMINETXDUO_BSD_TEST_PROTO_TIMER_H
#include <devices/timer.h>
VOID  GetSysTime(APTR dest);
ULONG ReadEClock(APTR dest);
#endif
