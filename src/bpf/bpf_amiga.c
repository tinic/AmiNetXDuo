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
 * Forbid()/Permit(), not Disable()/Enable(). The taps run on the SANA-II
 * reader threads and on whatever thread is inside nx_tcp_socket_send, and the
 * vectors run on application tasks, so the channel table is shared. Nothing
 * here runs at interrupt level, and long Disable() regions break serial,
 * floppy and audio (docs/RESEARCH.md 6.2).
 *
 * The longest bracket is ami_bpf_capture(), which copies the captured prefix
 * of a frame inside it, bounded by the snap length. The copy-out in
 * bpf_read(), which can be a whole 32 KB buffer, happens outside the bracket.
 */
VOID ami_bpf_lock(VOID)
{
    Forbid();
}

VOID ami_bpf_unlock(VOID)
{
    Permit();
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
       Deliberately not a MsgPort of ours: one made on another Process leaves
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
 * the channel lock, a Forbid(), held, and ami_millis() reaches
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
