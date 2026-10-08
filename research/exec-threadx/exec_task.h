/* Research task mechanism, not ThreadX thread-create API conformance.
 * SPDX-License-Identifier: MIT */
#ifndef ANX_RESEARCH_EXEC_TASK_H
#define ANX_RESEARCH_EXEC_TASK_H
#include "tx_bridge.h"
#include "exec_wait.h"
#include <exec/tasks.h>

enum { ANX_TASK_EMPTY, ANX_TASK_STARTING, ANX_TASK_RUNNING, ANX_TASK_FINISHED,
       ANX_TASK_REAPED };
typedef struct {
    struct Task task;
    TX_THREAD thread;
    AnxTxThread bridge;
    AnxExecWait wait;
    struct Task *creator;
    VOID (*entry)(ULONG);
    ULONG input, stack_size;
    APTR stack;
    CHAR *name;
    ULONG ack;
    BYTE signal;
    volatile unsigned state, started;
} AnxExecTask;

/* Zero-initialize record once. Runtime must be initialized first. Creator owns
 * record, name and aligned >=8192-byte stack until successful reap. Distinct
 * live records/stacks must not overlap. Exec task priority is exactly zero;
 * no ThreadX priorities, thresholds, timeslicing or forced termination promised.
 * Startup waits for owner-side private IO + bridge registration. Entry runs in
 * one outer bridge context, may use supported blocking services, must return
 * with all producers quiescent and no owned mutexes. It must not perform foreign
 * blocking Exec IO inside the boundary. An invalid return is fatal, not a reap.
 * Calls below require normal creator task context outside Forbid/Disable and
 * outside all bridge contexts. No interrupt/foreign-owner lifecycle calls.
 * Failed startup rolls back task/signal/private IO and returns zero. */
int anx_exec_task_start(AnxExecTask *, CHAR *, VOID (*)(ULONG), ULONG,
                        APTR stack, ULONG stack_size);
/* Nonblocking: zero while live or when caller/context is invalid. No control/
 * stack access is safe for reuse merely on FINISHED: successful reap is required.
 * join waits outside the bridge, then performs the same owner-only reap. */
int anx_exec_task_reap(AnxExecTask *);
int anx_exec_task_join(AnxExecTask *);
#endif
