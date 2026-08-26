/*
 * The event ring: what the library did, as numbers.
 *
 * NOT ONE STRING MAY ENTER THIS FILE, or any call site of ami_event();
 * tools/check-no-diag-strings.sh is what says it stayed satisfied.  No
 * allocation ever, and Disable() not Forbid(): lib_expunge already holds one.
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
