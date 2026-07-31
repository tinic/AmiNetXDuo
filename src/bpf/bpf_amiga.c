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
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/timer.h>

/*
 * Forbid()/Permit(), not Disable()/Enable(). The taps run on the SANA-II
 * reader threads and on whatever thread is inside nx_tcp_socket_send, and the
 * vectors run on application tasks, so the channel table is shared; but
 * nothing here runs at interrupt level, and long Disable() regions break
 * serial, floppy and audio (docs/RESEARCH.md 6.2).
 *
 * The longest bracket is ami_bpf_capture(), which copies the captured prefix
 * of a frame inside it, bounded by the snap length. bpf_read()'s copy-out,
 * which can be a whole 32 KB buffer, is done outside the bracket.
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
    /* dos.library's Delay(), which waits on the calling Process's own timer.
       Deliberately not a MsgPort of ours: one made on another Process leaves
       mp_SigTask pointing at a task that may be gone (544398f). */
    if (ticks != 0)
        Delay((LONG)ticks);
}

ULONG ami_bpf_signals_set(ULONG mask)
{
    /* Read without consuming: the caller asked to be interrupted by these,
       and clearing them here would swallow the wake-up it is waiting for. */
    return (mask != 0) ? (SetSignal(0UL, 0UL) & mask) : 0UL;
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
 * counter; calling ami_millis() once forces that open. If it failed, fall back
 * to milliseconds since stack startup: monotonic and correctly spaced, but
 * with an epoch of whenever the stack came up, which a capture viewer renders
 * as 1970. That is cosmetic, where refusing to capture would not be.
 */
extern struct Device *TimerBase;

VOID ami_bpf_now(ULONG *sec, ULONG *usec)
{
    /* `struct timeval`, not `TimeVal_Type`: the typedef is NDK 3.2's, and NDK
       3.9 has no such name.  Both NDKs declare GetSysTime() as taking the
       Amiga timeval -- 3.2 spells that parameter TimeVal_Type, which is a
       typedef for exactly this struct -- and both give it tv_secs/tv_micro. */
    struct timeval tv;

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
