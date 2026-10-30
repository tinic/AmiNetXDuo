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

#include <exec/semaphores.h>
#include <exec/tasks.h>
#include <exec/types.h>

/* Defined by the test, so it can choose who is asking. */
extern struct Task *shim_current_task;
extern int          shim_forbid_depth;
extern int          shim_semaphore_depth;

static inline struct Task *FindTask(STRPTR name)
{
    (void)name;         /* only FindTask(NULL) -- "me" -- is used */
    return shim_current_task;
}

/* The calling task's received signals, as SetSignal() reads and changes
   them.  Defined by a test that calls it. */
extern ULONG shim_signals;

static inline ULONG SetSignal(ULONG new_signals, ULONG signal_set)
{
    ULONG old = shim_signals;

    shim_signals = (old & ~signal_set) | (new_signals & signal_set);
    return old;
}

/* Defined by a test that links a caller: ug_copy_vectors() in ug_context.c
   copies a jump table and must flush after it. */
void CopyMem(APTR source, APTR dest, ULONG size);
void CacheClearU(void);

static inline void Forbid(void) { ++shim_forbid_depth; }
static inline void Permit(void) { --shim_forbid_depth; }

/* Nothing here contends, but the depth is counted so a test can assert the
   pairing the database loader relies on. */
static inline void ObtainSemaphore(struct SignalSemaphore *s)
{ (void)s; ++shim_semaphore_depth; }

static inline void ReleaseSemaphore(struct SignalSemaphore *s)
{ (void)s; --shim_semaphore_depth; }

#endif /* AMINETXDUO_SHIM_PROTO_EXEC_H */
