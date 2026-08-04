/* struct SignalSemaphore, for the bsdsocket host tests.
   SPDX-License-Identifier: MIT */
#ifndef AMINETXDUO_BSD_TEST_EXEC_SEMAPHORES_H
#define AMINETXDUO_BSD_TEST_EXEC_SEMAPHORES_H
#include <exec/nodes.h>
#include <exec/lists.h>
#include <exec/tasks.h>

struct SemaphoreRequest {
    struct MinNode sr_Link;
    struct Task   *sr_Waiter;
};

struct SignalSemaphore {
    struct Node             ss_Link;
    WORD                    ss_NestCount;
    struct MinList          ss_WaitQueue;
    struct SemaphoreRequest ss_MultipleLink;
    struct Task            *ss_Owner;
    WORD                    ss_QueueCount;
};

#endif
