/* Research only. SPDX-License-Identifier: MIT */
#include "wait.h"

void anx_wait_init(AnxWait *w, const AnxWaitOps *ops)
{
    w->ops = ops;
    w->generation = 0;
    w->result = ANX_WAIT_IDLE;
    w->deadline = 0;
    w->cleanup = 0;
    w->cleanup_context = 0;
}

uint32_t anx_wait_begin(AnxWait *w, uint64_t timeout,
                       void (*publish)(void *, AnxWait *, uint32_t), void *arg,
                       void (*cleanup)(void *, AnxWaitResult), void *cleanup_arg)
{
    const AnxWaitOps *ops = w->ops;
    uint64_t now;
    uint32_t token;
    if (!timeout)
        return 0;
    ops->enter(ops->context);
    now = ops->clock(ops->context);
    if (w->result == ANX_WAIT_PENDING || w->generation == UINT32_MAX ||
        (timeout != ANX_WAIT_FOREVER && timeout >= UINT64_MAX - now)) {
        ops->leave(ops->context);
        return 0;
    }
    token = ++w->generation;
    w->deadline = timeout == ANX_WAIT_FOREVER ? ANX_WAIT_FOREVER : now + timeout;
    w->cleanup = cleanup;
    w->cleanup_context = cleanup_arg;
    w->result = ANX_WAIT_PENDING;
    if (publish)
        publish(arg, w, token);
    ops->leave(ops->context);
    return token;
}

int anx_wait_complete(AnxWait *w, uint32_t token, AnxWaitResult result)
{
    const AnxWaitOps *ops = w->ops;
    int won = 0;
    if (result < ANX_WAIT_READY || result > ANX_WAIT_ERROR)
        return 0;
    ops->enter(ops->context);
    if (token && token == w->generation && w->result == ANX_WAIT_PENDING) {
        w->result = result;
        if (w->cleanup)
            w->cleanup(w->cleanup_context, result);
        /* Signal under protection: the owner cannot return and release its
         * wakeup resources before the winning producer finishes notifying. */
        ops->notify(ops->context);
        won = 1;
    }
    ops->leave(ops->context);
    return won;
}

AnxWaitResult anx_wait_run(AnxWait *w, uint32_t token)
{
    const AnxWaitOps *ops = w->ops;
    for (;;) {
        AnxWaitResult result;
        uint64_t deadline;
        ops->enter(ops->context);
        result = token && token == w->generation ? w->result : ANX_WAIT_ERROR;
        deadline = w->deadline;
        ops->leave(ops->context);
        if (result != ANX_WAIT_PENDING)
            return result == ANX_WAIT_IDLE ? ANX_WAIT_ERROR : result;
        int parked = ops->park(ops->context, deadline);
        if (parked <= 0)
            (void)anx_wait_complete(w, token,
                                   parked < 0 ? ANX_WAIT_ERROR : ANX_WAIT_TIMEOUT);
        /* Spurious notifications keep the original deadline. A completion
         * committed while park reports expiry wins over that late expiry. */
    }
}
