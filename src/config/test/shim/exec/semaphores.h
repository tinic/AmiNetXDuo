/*
 * Host test shim -- struct SignalSemaphore. Nothing on the host locks it; it
 * is here so a structure that embeds one has a size. See exec/types.h.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_TEST_EXEC_SEMAPHORES_H
#define AMINETXDUO_TEST_EXEC_SEMAPHORES_H

#include <exec/types.h>
#include <exec/lists.h>

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
