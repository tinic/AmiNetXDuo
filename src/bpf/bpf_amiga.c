/*
 * AmiNetXDuo -- the AmigaOS half of the bpf_* path.
 *
 * Four functions, so that bpf_filter.c, bpf_validate.c, bpf_channel.c and
 * bpf_tap.c stay host-buildable and host-testable.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bpf_internal.h"

#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/timer.h>

/*
 * Forbid()/Permit(), not Disable()/Enable(). The taps run on the SANA-II
 * reader threads and on whatever thread happens to be inside
 * nx_tcp_socket_send, and the vectors run on application tasks, so the channel
 * table is genuinely shared -- but nothing here runs at interrupt level, and
 * long Disable() regions break serial, floppy and audio (docs/RESEARCH.md 6.2).
 *
 * The one place this is nearly long enough to matter is ami_bpf_capture(),
 * which copies the captured prefix of a frame inside the bracket. That is
 * bounded by the snap length and is why bpf_read()'s copy-out -- which can be
 * a whole 32 KB buffer -- was moved outside it instead.
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

/*
 * bh_tstamp, as seconds and microseconds since the Unix epoch.
 *
 * GetSysTime() rather than DateStamp(): it is a timer.device library call, so
 * it is safe from a Task that is not a Process, which the SANA-II readers are
 * not. It reports Amiga time -- seconds since 1978-01-01 -- so the epoch
 * difference goes on top, or every capture would appear to have happened in
 * the 1970s.
 *
 * TimerBase is opened by src/common/compat.c for its EClock millisecond
 * counter; calling ami_millis() once is what forces that open. If it failed,
 * fall back to milliseconds since stack startup: monotonic and correctly
 * spaced, but with an epoch of "whenever the stack came up", which a capture
 * viewer will render as 1970. Wrong absolute time is a cosmetic problem;
 * refusing to capture would not be.
 */
extern struct Device *TimerBase;

VOID ami_bpf_now(ULONG *sec, ULONG *usec)
{
    TimeVal_Type tv;

    (VOID)ami_millis();

    if (TimerBase != NULL)
    {
        GetSysTime(&tv);
        *sec  = tv.tv_secs + AMI_BPF_AMIGA_EPOCH;
        *usec = tv.tv_micro;
        return;
    }

    {
        ULONG ms = ami_millis();

        *sec  = ms / 1000UL;
        *usec = (ms % 1000UL) * 1000UL;
    }
}
