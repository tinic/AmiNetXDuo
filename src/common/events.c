/*
 * The event ring: what the library did, as numbers.
 *
 * WHY THIS IS NOT A LOG.  AMI_ERROR/AMI_WARN/AMI_INFO compile away unless
 * AMINETXDUO_LOG is defined, and it is off in every shipped binary because the
 * format strings cost 12,820 bytes on the 68000 tier.  Every diagnostic in the
 * tree is therefore absent from every binary a user has.  This records the
 * same facts in a form that costs no strings at all: a code, an index, a value
 * and a time, in a fixed array in BSS.  The words live in
 * src/tools/tool_events.c and are printed by a Shell command.
 *
 * NOT ONE STRING MAY ENTER THIS FILE, or any call site of ami_event().  That
 * is the constraint the whole design exists to satisfy, and
 * tools/check-no-diag-strings.sh is what says it stayed satisfied.
 *
 * NO ALLOCATION, EVER.  Two of the call sites are a teardown that has already
 * failed and an expunge with the segment about to be handed back; neither can
 * afford a path that can fail or block.  The ring is a static array and
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
 * and that ObtainSemaphore() can Wait; a Wait inside lib_expunge's Forbid()
 * would break the Forbid on the one path whose job is to decide whether the
 * segment may be unloaded.  ami_millis_quick() answers 0 rather than opening
 * anything, so an event recorded before the stack ever asked the time carries
 * no time.  Zero is not ambiguous: nse_Seq still orders it.
 *
 * WHY THE RING IS INSIDE THE PUBLISHED MARK.  A reader must be able to get at
 * it without opening the library, since opening it starts the network
 * (aminetxduo/events.h).  Putting the ring in the mark rather than beside it
 * means there is one copy and no synchronising between them.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/semaphores.h>
#include <proto/exec.h>

#include "aminetxduo/compat.h"
#include "aminetxduo/events.h"

/*
 * The mark and its ring, adjacent and in that order, which is what
 * AMI_EVENTS_RING() assumes.  A struct rather than two objects, because two
 * objects in BSS are not promised to be adjacent.
 *
 * BSS, so it costs nothing in the file: an AmigaOS hunk records a BSS section
 * as a length and no bytes.  Making the ring bigger costs memory on the
 * machine and nothing on disk.
 */
static struct
{
    AmiEventMark    mark;
    NetStatusEvent  ring[AMINETXDUO_EVENT_RING];
} ami_event_store;

VOID ami_event(UWORD code, UWORD index, ULONG value)
{
    AmiEventMark   *m = &ami_event_store.mark;
    NetStatusEvent *e;
    ULONG           tick = ami_millis_quick();

    Disable();

    e = &ami_event_store.ring[m->em_Next];

    m->em_Next = (m->em_Next + 1UL) % (ULONG)AMINETXDUO_EVENT_RING;
    m->em_Seq++;

    e->nse_Code  = code;
    e->nse_Index = index;
    e->nse_Value = value;
    e->nse_Tick  = tick;
    e->nse_Seq   = m->em_Seq;

    Enable();
}

/*
 * Oldest first, so a reader prints them in the order they happened.  The
 * entries that survive are the last min(seq, AMINETXDUO_EVENT_RING) of them,
 * and the oldest of those sits at em_Next once the ring has wrapped.
 *
 * The copy is done inside Disable() for the reason the whole record is copied
 * rather than pointed at: the caller prints afterwards, and an entry half
 * overwritten while it was being read would be a time from one event and a
 * value from another.  The ring is small and the copy is a few hundred bytes.
 */
ULONG ami_event_snapshot(NetStatusEvent *out, ULONG room, ULONG *held)
{
    const AmiEventMark *m = &ami_event_store.mark;
    ULONG have;
    ULONG first;
    ULONG i;
    ULONG written = 0;

    Disable();

    have = (m->em_Seq < (ULONG)AMINETXDUO_EVENT_RING)
               ? m->em_Seq : (ULONG)AMINETXDUO_EVENT_RING;
    first = (m->em_Seq <= (ULONG)AMINETXDUO_EVENT_RING) ? 0UL : m->em_Next;

    for (i = 0; i < have && written < room; i++)
    {
        out[written] = ami_event_store.ring[(first + i) %
                                            (ULONG)AMINETXDUO_EVENT_RING];
        written++;
    }

    Enable();

    if (held != NULL)
        *held = have;

    return written;
}

/* ------------------------------------------------------------ the mark, */

static BOOL ami_event_published;

VOID ami_event_publish(VOID)
{
    AmiEventMark *m = &ami_event_store.mark;

    Forbid();

    if (!ami_event_published)
    {
        InitSemaphore(&m->em_Semaphore);
        m->em_Semaphore.ss_Link.ln_Name = (char *)AMI_EVENTS_NAME;
        m->em_Semaphore.ss_Link.ln_Pri  = 0;

        m->em_Magic     = AMI_EVENTS_MAGIC;
        m->em_Version   = (UWORD)AMI_EVENTS_VERSION;
        m->em_Size      = (UWORD)sizeof(AmiEventMark);
        m->em_Entries   = (UWORD)AMINETXDUO_EVENT_RING;
        m->em_EntrySize = (UWORD)sizeof(NetStatusEvent);

        AddSemaphore(&m->em_Semaphore);
        ami_event_published = TRUE;
    }

    Permit();
}

/*
 * Under Forbid(), and before anything in the segment is freed: a reader holds
 * Forbid() across its find and its copy, so a mark removed inside one cannot
 * be the mark a reader is in the middle of.
 */
VOID ami_event_unpublish(VOID)
{
    Forbid();

    if (ami_event_published)
    {
        RemSemaphore(&ami_event_store.mark.em_Semaphore);
        ami_event_store.mark.em_Magic = 0UL;
        ami_event_published = FALSE;
    }

    Permit();
}
