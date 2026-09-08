/*
 * AmiNetXDuo host-test shim: the exec.library calls the usergroup context
 * makes, and nothing else.
 *
 * ug_context.c reaches exec three times -- FindTask(), Forbid(), Permit().
 * Forbid/Permit are a scheduler lock; a single-threaded host test has no
 * scheduler to lock, so they count instead, which lets a test assert the
 * pairing that a cross-task walk depends on.  FindTask() returns whatever the
 * test set, so a test can be two different tasks in turn without threads.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_SHIM_PROTO_EXEC_H
#define AMINETXDUO_SHIM_PROTO_EXEC_H

#include <exec/tasks.h>
#include <exec/types.h>

/* Defined by the test, so it can choose who is asking. */
extern struct Task *shim_current_task;
extern int          shim_forbid_depth;

static inline struct Task *FindTask(STRPTR name)
{
    (void)name;         /* only FindTask(NULL) -- "me" -- is used */
    return shim_current_task;
}

static inline void Forbid(void) { ++shim_forbid_depth; }
static inline void Permit(void) { --shim_forbid_depth; }

#endif /* AMINETXDUO_SHIM_PROTO_EXEC_H */
