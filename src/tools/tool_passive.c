/*
 * The published records, read without opening anything.  See tool_passive.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tool_passive.h"

#include <exec/semaphores.h>

/* The driver's own name plus the suffix, so anxnet.device and anxgenet.device
   each have a record.  A path is reduced to the name Exec knows.  Built
   before Forbid(). */
static char tp_sem_name[64];

static const char *tp_sem_name_for(const char *device)
{
    const char *base = device;
    const char *p;
    ULONG       n    = 0;

    for (p = device; *p != '\0'; p++)
    {
        if (*p == '/' || *p == ':')
            base = p + 1;
    }
    for (p = base; *p != '\0' && n < sizeof(tp_sem_name) - 1; p++)
        tp_sem_name[n++] = *p;
    for (p = ANXDIAG_NAME_SUFFIX; *p != '\0' && n < sizeof(tp_sem_name) - 1; p++)
        tp_sem_name[n++] = *p;
    tp_sem_name[n] = '\0';

    return tp_sem_name;
}

UWORD tool_anxdiag_read(const char *device, AnxDiagMark *out)
{
    const AnxDiagMark *mark;
    UWORD              status = TOOL_PASSIVE_ABSENT;
    const char        *name   = tp_sem_name_for(device);

    Forbid();

    /* (STRPTR): NDK 3.9 declares FindSemaphore(STRPTR), 3.2 CONST_STRPTR. */
    mark = (const AnxDiagMark *)FindSemaphore((STRPTR)name);

    if (mark != NULL && mark->ad_Magic == ANXDIAG_MAGIC)
    {
        if (mark->ad_Version == (UWORD)ANXDIAG_VERSION &&
            mark->ad_Size    == (UWORD)sizeof(AnxDiagMark))
        {
            *out   = *mark;
            status = TOOL_PASSIVE_OK;
        }
        else
        {
            /* The shapes disagree, so no other field can be believed. */
            out->ad_Version = mark->ad_Version;
            out->ad_Size    = mark->ad_Size;
            status = TOOL_PASSIVE_BAD_VERSION;
        }
    }

    Permit();

    return status;
}

LONG tool_events_read(AmiEventMark *mark_out, NetStatusEvent *out, ULONG max,
                      UWORD *status)
{
    const AmiEventMark   *mark;
    const NetStatusEvent *ring;
    LONG                  copied = 0;

    *status = TOOL_PASSIVE_ABSENT;

    Forbid();

    mark = (const AmiEventMark *)FindSemaphore((STRPTR)AMI_EVENTS_NAME);

    if (mark != NULL && mark->em_Magic == AMI_EVENTS_MAGIC)
    {
        /* The header's shape and one entry's, and not the whole object's
           size: the ring's length is a build option. */
        if (mark->em_Version   == (UWORD)AMI_EVENTS_VERSION &&
            mark->em_Size      == (UWORD)sizeof(AmiEventMark) &&
            mark->em_EntrySize == (UWORD)sizeof(NetStatusEvent))
        {
            ULONG entries = (ULONG)mark->em_Entries;
            ULONG have;
            ULONG first;
            ULONG i;

            *mark_out = *mark;
            ring      = AMI_EVENTS_RING(mark);

            have  = (mark->em_Seq < entries) ? mark->em_Seq : entries;
            first = (mark->em_Seq <= entries) ? 0UL : mark->em_Next;

            for (i = 0; i < have && (ULONG)copied < max; i++)
            {
                out[copied] = ring[(first + i) % entries];
                copied++;
            }

            *status = TOOL_PASSIVE_OK;
        }
        else
        {
            *status = TOOL_PASSIVE_BAD_VERSION;
        }
    }

    Permit();

    return (*status == TOOL_PASSIVE_OK) ? copied : -1;
}
