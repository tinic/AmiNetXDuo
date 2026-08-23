/*
 * The event ring: what the library did, as numbers.
 *
 * WHY THIS IS NOT A LOG.  AMI_ERROR/AMI_WARN/AMI_INFO compile away unless
 * AMINETXDUO_LOG is defined, and it is off in every shipped binary because the
 * format strings cost 12,820 bytes on the 68000 tier.  Every diagnostic in the
 * tree is therefore absent from every binary a user has.  This records the
 * same facts in a form that costs no strings at all: a code, an index, a value
 * and a time, in a fixed array in BSS.  The sentences live in
 * src/tools/tool_events.c and are read by a Shell command.
 *
 * NOT ONE STRING MAY ENTER THIS FILE, or any call site of ami_event().  That
 * is the constraint the whole design exists to satisfy and
 * tools/check-no-diag-strings.sh is what says it stayed satisfied.
 *
 * NO ALLOCATION, EVER.  Two of the call sites are a teardown that has already
 * failed and an expunge with the segment about to be handed back; neither can
 * afford a path that can fail or block.  The ring is a static array, and
 * recording is a bounded number of instructions between Disable() and
 * Enable().
 *
 * DISABLE AND NOT FORBID.  Recording happens on application tasks, on ThreadX
 * threads, on SANA-II reader Tasks and inside lib_expunge, which Exec calls
 * with Forbid() already held.  Disable() is correct in all of them and is the
 * only one that would still be correct if a call site ever moved into an
 * interrupt.  The window is a few dozen instructions.
 *
 * THE CLOCK IS OPTIONAL.  ami_millis() opens timer.device on its first call,
 * which Waits, and a Wait inside lib_expunge's Forbid() would break it.
 * ami_millis_quick() answers 0 rather than opening anything, so an event
 * recorded before the stack ever asked the time carries no time.  Zero is
 * distinguishable: nse_Seq still orders it.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <proto/exec.h>

#include "aminetxduo/compat.h"
#include "aminetxduo/events.h"

/*
 * The ring.  BSS, so it costs nothing in the file: an AmigaOS hunk records a
 * BSS section as a length and no bytes.  Making it bigger costs memory on the
 * machine and nothing on disk, which is why the floor is where it is and the
 * default is not at it.
 */
static NetStatusEvent   ami_event_ring[AMINETXDUO_EVENT_RING];
static ULONG            ami_event_seq;      /* events ever recorded          */
static ULONG            ami_event_next;     /* where the next one goes       */

VOID ami_event(UWORD code, UWORD index, ULONG value)
{
    NetStatusEvent *e;
    ULONG           tick = ami_millis_quick();

    Disable();

    e = &ami_event_ring[ami_event_next];

    ami_event_next = (ami_event_next + 1UL) % (ULONG)AMINETXDUO_EVENT_RING;
    ami_event_seq++;

    e->nse_Code  = code;
    e->nse_Index = index;
    e->nse_Value = value;
    e->nse_Tick  = tick;
    e->nse_Seq   = ami_event_seq;

    Enable();
}

/*
 * Oldest first, so a reader prints them in the order they happened.  The
 * entries that survive are the last min(seq, AMINETXDUO_EVENT_RING) of them,
 * and the oldest of those sits at ami_event_next once the ring has wrapped.
 *
 * The copy is done inside Disable() for the reason the whole record is copied
 * rather than pointed at: the caller prints afterwards, and an entry half
 * overwritten while it was being read would be a time from one event and a
 * value from another.  AMINETXDUO_EVENT_RING is small and the copy is a few
 * hundred bytes.
 */
ULONG ami_event_snapshot(NetStatusEvent *out, ULONG room, ULONG *held)
{
    ULONG have;
    ULONG first;
    ULONG i;
    ULONG written = 0;

    Disable();

    have = (ami_event_seq < (ULONG)AMINETXDUO_EVENT_RING)
               ? ami_event_seq : (ULONG)AMINETXDUO_EVENT_RING;
    first = (ami_event_seq <= (ULONG)AMINETXDUO_EVENT_RING) ? 0UL
                                                            : ami_event_next;

    for (i = 0; i < have && written < room; i++)
    {
        out[written] = ami_event_ring[(first + i) %
                                      (ULONG)AMINETXDUO_EVENT_RING];
        written++;
    }

    Enable();

    if (held != NULL)
        *held = have;

    return written;
}
