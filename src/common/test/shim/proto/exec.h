/*
 * <proto/exec.h> for the event-ring host test: the calls src/common/events.c
 * makes, and nothing else.  test_events.c defines them.
 *
 * Disable()/Enable() count their nesting there rather than doing nothing: a
 * path that returns still inside Disable() stops every interrupt on a real
 * machine, and that is not visible from the code.
 *
 * The semaphore calls are a list on the host, so that publishing and removing
 * the mark can be asserted on the same way a machine would see it: a mark
 * that is findable, and one that is not there any more.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_EVENTS_TEST_PROTO_EXEC_H
#define AMINETXDUO_EVENTS_TEST_PROTO_EXEC_H

#include <exec/types.h>
#include <exec/semaphores.h>

void Disable(void);
void Enable(void);
void Forbid(void);
void Permit(void);

/*
 * FindTask() and AvailMem() are for src/common/test/test_ami_random.c, which
 * drives the entropy gatherers: gather_exec() asks who it is and gather_memory()
 * asks how much of each kind is free.  Declared here rather than in a second
 * shim so one <proto/exec.h> serves both tests in this directory.
 */
struct Task *FindTask(STRPTR name);
ULONG        AvailMem(ULONG requirements);
APTR         AllocVec(ULONG size, ULONG requirements);
void         FreeVec(APTR memory);

#define MEMF_ANY        0UL
#define MEMF_PUBLIC     (1UL << 0)
#define MEMF_CHIP       (1UL << 1)
#define MEMF_FAST       (1UL << 2)
#define MEMF_LARGEST    (1UL << 17)
#define MEMF_TOTAL      (1UL << 19)

void InitSemaphore(struct SignalSemaphore *sem);
void AddSemaphore(struct SignalSemaphore *sem);
void RemSemaphore(struct SignalSemaphore *sem);
struct SignalSemaphore *FindSemaphore(STRPTR name);

#endif
