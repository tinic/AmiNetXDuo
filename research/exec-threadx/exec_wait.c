/* Research only; no ThreadX replacement is selected by this adapter.
 * SPDX-License-Identifier: MIT */
#include "exec_wait.h"
#include <proto/exec.h>
#include <inline/timer.h>
#include <stdint.h>

static void ew_enter(void *arg) { (void)arg; Forbid(); }
static void ew_leave(void *arg) { (void)arg; Permit(); }

static uint64_t ew_clock(void *arg)
{
    AnxExecWait *e = arg;
    struct EClockVal clock;
    ULONG hz = __ReadEClock_base(e->timer->tr_node.io_Device, &clock);
    uint64_t ticks = ((uint64_t)clock.ev_hi << 32) | clock.ev_lo;
    /* Avoid overflow in the intermediate ticks * 1000000 calculation.
     * Microseconds themselves must remain representable (2^64 us). */
    return (ticks / hz) * 1000000 + ((ticks % hz) * 1000000) / hz;
}

static void ew_notify(void *arg)
{
    AnxExecWait *e = arg;
    Signal(e->owner, 1UL << e->signal);
}

static int ew_park(void *arg, uint64_t deadline)
{
    AnxExecWait *e = arg;
    ULONG wake = 1UL << e->signal;
    ULONG timer_signal = 1UL << e->port->mp_SigBit;
    ULONG received;
    if (deadline == ANX_WAIT_FOREVER) {
        (void)Wait(wake);
        return 1;
    }
    uint64_t now = ew_clock(e);
    if (now >= deadline)
        return 0;
    uint64_t remaining = deadline - now;
    /* timer.device seconds are 32 bits. A far-future wait is split into
     * bounded timer requests, without changing its absolute deadline. */
    uint64_t seconds = remaining / 1000000;
    if (seconds > UINT32_MAX) {
        e->timer->tr_time.tv_secs = UINT32_MAX;
        e->timer->tr_time.tv_micro = 0;
    } else {
        e->timer->tr_time.tv_secs = (ULONG)seconds;
        e->timer->tr_time.tv_micro = (ULONG)(remaining % 1000000);
    }
    e->timer->tr_node.io_Command = TR_ADDREQUEST;
    e->timer->tr_node.io_Error = 0;
    (void)SetSignal(0, timer_signal);
    SendIO((struct IORequest *)e->timer);
    e->timer_sends++;
    received = Wait(wake | timer_signal);
    if (!CheckIO((struct IORequest *)e->timer))
        AbortIO((struct IORequest *)e->timer);
    (void)WaitIO((struct IORequest *)e->timer);
    e->timer_reaps++;
    (void)SetSignal(0, timer_signal);
    if (e->timer->tr_node.io_Error && !(received & wake))
        return -1;
    /* A capped request is not the deadline; recheck the clock. */
    return (received & wake) != 0 || ew_clock(e) < deadline;
}

int anx_exec_wait_open(AnxExecWait *e)
{
    e->owner = FindTask(0);
    e->signal = -1;
    e->port = 0;
    e->timer = 0;
    e->opened = 0;
    e->timer_sends = e->timer_reaps = 0;
    e->signal = AllocSignal(-1);
    if (e->signal < 0)
        return 0;
    e->port = CreateMsgPort();
    if (!e->port)
        goto fail;
    e->timer = (struct timerequest *)CreateIORequest(e->port, sizeof(*e->timer));
    if (!e->timer || OpenDevice("timer.device", UNIT_MICROHZ,
                               (struct IORequest *)e->timer, 0))
        goto fail;
    e->opened = 1;
    e->ops = (AnxWaitOps){ew_enter, ew_leave, ew_clock, ew_park, ew_notify, e};
    anx_wait_init(&e->wait, &e->ops);
    (void)SetSignal(0, 1UL << e->signal);
    return 1;
fail:
    if (e->timer)
        DeleteIORequest((struct IORequest *)e->timer);
    if (e->port)
        DeleteMsgPort(e->port);
    FreeSignal(e->signal);
    e->signal = -1;
    e->timer = 0;
    e->port = 0;
    return 0;
}

AnxWaitResult anx_exec_wait_run(AnxExecWait *e, uint32_t token)
{
    if (!e->opened || FindTask(0) != e->owner)
        return ANX_WAIT_ERROR;
    return anx_wait_run(&e->wait, token);
}

int anx_exec_wait_close(AnxExecWait *e)
{
    if (!e->opened || FindTask(0) != e->owner)
        return 0;
    Forbid();
    if (e->wait.result == ANX_WAIT_PENDING || e->timer_sends!=e->timer_reaps) {
        Permit();
        return 0;
    }
    e->opened = 0;
    Permit();
    /* The caller has quiesced all producers. Each timed park reaps its IO. */
    CloseDevice((struct IORequest *)e->timer);
    DeleteIORequest((struct IORequest *)e->timer);
    DeleteMsgPort(e->port);
    (void)SetSignal(0, 1UL << e->signal);
    FreeSignal(e->signal);
    return 1;
}
