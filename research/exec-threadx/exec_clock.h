/* Single automatic application-clock owner, research only. SPDX-License-Identifier: MIT */
#ifndef ANX_EXEC_CLOCK_H
#define ANX_EXEC_CLOCK_H
#include "tx_bridge.h"
#include "exec_wait.h"
#include <exec/tasks.h>
enum { ANX_CLOCK_EMPTY, ANX_CLOCK_STARTING, ANX_CLOCK_RUNNING,
       ANX_CLOCK_STOPPING, ANX_CLOCK_FINISHED, ANX_CLOCK_REAPED };
typedef struct {
    struct Task task;
    AnxExecWait wait;
    struct Task *creator;
    APTR stack;
    ULONG stack_size,ack;
    BYTE signal;
    volatile unsigned state,started;
    uint64_t next;
    uint32_t token;
    ULONG ticks,batches,catchup_batches;
} AnxExecClock;
/* Zero-init once. One clock per domain. Creator owns disjoint record, name,
 * aligned >=8192 stack until join. Start/join outside all boundaries; stop
 * inside creator's normal serialized boundary. The clock has no public
 * ThreadX thread and waits outside its marked callback boundary. It advances
 * application timers, not the already elapsed-time-based private waits.
 * Absolute EClock phase; max eight ticks per boundary, no skipped backlog.
 * No interrupt calls, timeslicing or automatic producer-lifetime inference.
 * Caller must close producers/deactivate timers before their storage release. */
int anx_exec_clock_start(AnxExecClock *, CHAR *, APTR, ULONG);
int anx_exec_clock_can_stop(AnxExecClock *);
int anx_exec_clock_stop(AnxExecClock *);
int anx_exec_clock_join(AnxExecClock *);
#endif
