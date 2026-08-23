/*
 * <proto/exec.h> for the event-ring host test: the two calls
 * src/common/events.c makes, and nothing else.  test_events.c defines them,
 * and counts the nesting rather than ignoring it -- a path that returns still
 * inside Disable() stops every interrupt on a real machine.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_EVENTS_TEST_PROTO_EXEC_H
#define AMINETXDUO_EVENTS_TEST_PROTO_EXEC_H

void Disable(void);
void Enable(void);

#endif
