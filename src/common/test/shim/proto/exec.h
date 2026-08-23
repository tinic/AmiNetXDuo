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

void InitSemaphore(struct SignalSemaphore *sem);
void AddSemaphore(struct SignalSemaphore *sem);
void RemSemaphore(struct SignalSemaphore *sem);
struct SignalSemaphore *FindSemaphore(STRPTR name);

#endif
