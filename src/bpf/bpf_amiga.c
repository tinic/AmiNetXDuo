/*
 * AmiNetXDuo, the AmigaOS half of the bpf_* path.
 *
 * Four functions, so that bpf_filter.c, bpf_validate.c, bpf_channel.c and
 * bpf_tap.c stay host-buildable and host-testable.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bpf_internal.h"

#include <devices/timer.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/timer.h>

/*
 * A real semaphore, not a machine-wide Forbid().  Control calls may wait for
 * one another without stopping unrelated tasks.  Packet taps use Attempt and
 * skip capture on contention: they can run from an adopted ThreadX caller,
 * where an Exec Wait() would stop the network scheduler which must release
 * the lock.  Capture is observational, so dropping that record is the only
 * safe backpressure policy.
 */
static struct SignalSemaphore ami_bpf_sem;
static BOOL                   ami_bpf_sem_ready;

static VOID ami_bpf_lock_init(VOID)
{
    Forbid();
    if (!ami_bpf_sem_ready)
    {
        InitSemaphore(&ami_bpf_sem);
        ami_bpf_sem_ready = TRUE;
    }
    Permit();
}

VOID ami_bpf_lock(VOID)
{
    ami_bpf_lock_init();
    ObtainSemaphore(&ami_bpf_sem);
}

BOOL ami_bpf_try_lock(VOID)
{
    ami_bpf_lock_init();
    return AttemptSemaphore(&ami_bpf_sem) ? TRUE : FALSE;
}

VOID ami_bpf_unlock(VOID)
{
    ReleaseSemaphore(&ami_bpf_sem);
}

APTR ami_bpf_current_task(VOID)
{
    return (APTR)FindTask(NULL);
}

VOID ami_bpf_notify(APTR task, ULONG mask)
{
    if (task == NULL || mask == 0)
        return;

    Signal((struct Task *)task, mask);
}

VOID ami_bpf_sleep(ULONG ticks)
{
    /* dos.library Delay(), which waits on the timer of the calling Process.
       Deliberately not a MsgPort here: one made on another Process leaves
       mp_SigTask set to a task that can be gone (544398f). */
    if (ticks != 0)
        Delay((LONG)ticks);
}

ULONG ami_bpf_signals_set(ULONG mask)
{
    /* Read the signals without a clear: the caller asked to be interrupted by
       these, and a clear here loses the wake-up that it waits for. */
    return (mask != 0) ? (SetSignal(0UL, 0UL) & mask) : 0UL;
}

/*
 * bh_tstamp, as seconds and microseconds since the Unix epoch.
 *
 * GetSysTime() rather than DateStamp(): it is a timer.device library call, so
 * it is safe from a Task that is not a Process. The SANA-II readers are such
 * Tasks. GetSysTime() reports Amiga time, seconds since 1978-01-01, so the
 * epoch difference goes on top. Without it every capture appears to date from
 * the 1970s.
 *
 * TimerBase is opened by src/common/compat.c for its EClock millisecond
 * counter, and ami_bpf_time_init() forces that open from ami_bpf_open(). It
 * must happen there and not here: ami_bpf_capture() calls ami_bpf_now() with
 * the channel lock held, and ami_millis() reaches
 * OpenDevice("timer.device") on the first call. Here that runs on a SANA-II
 * reader thread with task switching off, once per captured frame for as long
 * as the open keeps failing.
 *
 * With no timer the capture still happens and only the timestamps are zero,
 * which a viewer shows as 1970. That is preferred to a refusal to capture.
 */
extern struct Device *TimerBase;

VOID ami_bpf_time_init(VOID)
{
    (VOID)ami_millis();
}

VOID ami_bpf_now(ULONG *sec, ULONG *usec)
{
    /* `struct timeval`, not `TimeVal_Type`: that typedef belongs to NDK 3.2,
       and NDK 3.9 has no such name. Both NDKs declare GetSysTime() with the
       Amiga timeval, and both give it tv_secs and tv_micro. NDK 3.2 spells
       the parameter TimeVal_Type, a typedef for this same struct. */
    struct timeval tv;

    if (TimerBase == NULL)
    {
        *sec  = 0UL;
        *usec = 0UL;
        return;
    }

    GetSysTime(&tv);
    *sec  = tv.tv_secs + AMI_BPF_AMIGA_EPOCH;
    *usec = tv.tv_micro;
}
