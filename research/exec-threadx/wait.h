/* Research only. SPDX-License-Identifier: MIT */
#ifndef ANX_RESEARCH_WAIT_H
#define ANX_RESEARCH_WAIT_H
#include <stdint.h>

#define ANX_WAIT_FOREVER UINT64_MAX
typedef enum {
    ANX_WAIT_IDLE, ANX_WAIT_PENDING, ANX_WAIT_READY, ANX_WAIT_TIMEOUT,
    ANX_WAIT_CANCELLED, ANX_WAIT_DELETED, ANX_WAIT_ERROR
} AnxWaitResult;

/* enter/leave serialize producers with the waiting task. notify must not block
 * and park must retain notifications sent before it starts (Exec signals do).
 * clock is monotonic microseconds. park returns zero when its deadline expires,
 * positive for a notification, negative for a platform failure.
 * cleanup and publish run under protection and must not block, reenter this
 * primitive or destroy w. This primitive serializes task-level producers;
 * interrupt producers require an adapter with interrupt-safe protection.
 * The owner retains the wait object until producers/timers have been quiesced;
 * generation checking rejects stale events, not accesses to freed memory. */
typedef struct {
    void (*enter)(void *);
    void (*leave)(void *);
    uint64_t (*clock)(void *);
    int (*park)(void *, uint64_t);
    void (*notify)(void *);
    void *context;
} AnxWaitOps;

typedef struct AnxWait {
    const AnxWaitOps *ops;
    uint32_t generation;
    AnxWaitResult result;
    uint64_t deadline;
    void (*cleanup)(void *, AnxWaitResult);
    void *cleanup_context;
} AnxWait;

void anx_wait_init(AnxWait *, const AnxWaitOps *);
/* Zero means failure. A terminal wait can be reused after the owner returns.
 * publish registers the waiter and releases its object protection atomically
 * under ops->enter. Resource preparation must have succeeded before this call.
 * A zero timeout belongs to the caller's no-wait operation, not this primitive. */
uint32_t anx_wait_begin(AnxWait *, uint64_t,
                       void (*publish)(void *, AnxWait *, uint32_t), void *,
                       void (*cleanup)(void *, AnxWaitResult), void *);
int anx_wait_complete(AnxWait *, uint32_t, AnxWaitResult);
AnxWaitResult anx_wait_run(AnxWait *, uint32_t);
#endif
