/* Native adapter for the research wait primitive. SPDX-License-Identifier: MIT */
#ifndef ANX_RESEARCH_EXEC_WAIT_H
#define ANX_RESEARCH_EXEC_WAIT_H
#include "wait.h"
#include <exec/types.h>
#include <devices/timer.h>
typedef struct {
    AnxWait wait;
    AnxWaitOps ops;
    struct Task *owner;
    struct MsgPort *port;
    struct timerequest *timer;
    BYTE signal;
    UBYTE opened;
    /* Research lifecycle proof counters: submitted requests must be reaped
     * before close. Owner writes; observers inspect under Forbid. Not timing. */
    volatile ULONG timer_sends,timer_reaps;
} AnxExecWait;
/* Open, begin/run and close belong to the same task. complete may be called
 * by another task while this object is retained, never from an interrupt.
 * Never close a pending wait. Quiesce producers before closing/reusing storage.
 * Forbid protects this task-level primitive, not ThreadX scheduler semantics. */
int anx_exec_wait_open(AnxExecWait *);
AnxWaitResult anx_exec_wait_run(AnxExecWait *, uint32_t);
int anx_exec_wait_close(AnxExecWait *);
#endif
