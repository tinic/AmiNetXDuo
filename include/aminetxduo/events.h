/*
 * Recording an event, and the mark the ring is published at.
 *
 * The record itself, the codes and the sentence each code stands for are in
 * aminetxduo/netstatus.h and src/tools/tool_events.c.
 *
 * A call to ami_event() must never carry a string, a format or a pointer to
 * either.  See src/common/events.c, and tools/check-no-diag-strings.sh, which
 * fails the build if a diagnostic sentence appears in a shipped image.
 *
 * TWO WAYS TO THE SAME RING, which is what aminetxduo/health.h already does
 * for the scheduling counters and aminetxduo/anxdiag.h does for the driver's
 * probe.
 *
 *   NETSTATUS_EVENTS         for a program that has the library open.  It is
 *                            a copy, taken under the same rules as every
 *                            other selector.
 *   FindSemaphore(AMI_EVENTS_NAME)
 *                            for a command that must not open it.
 *
 * The second is not a convenience.  bsd_lib_open() calls netstack_startup(),
 * so opening the library brings the network up, and the events worth reading
 * are the ones a shutdown left behind: a diagnostic that restarted the stack
 * in order to report on its teardown would be reporting on a different
 * machine.  After a NetShutdown the library is still resident with an open
 * count of zero, which is exactly the state the mark is readable in and the
 * selector is not.
 *
 * The mark lives for as long as the segment does, not for as long as the stack
 * does: it is published in bsd_lib_init() and removed under Forbid() in
 * bsd_lib_expunge(), before the memory holding the ring can go away.  A reader
 * that holds Forbid() across the find and the copy therefore cannot be reading
 * a freed one.  Nothing obtains the semaphore; it is a name to be found.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_EVENTS_H
#define AMINETXDUO_EVENTS_H

#include <exec/types.h>
#include <exec/semaphores.h>

#include "aminetxduo/netstatus.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * How many events are kept.  A build option, because someone counting the last
 * few bytes of RAM should be able to shrink this rather than lose the
 * mechanism; the floor is in CMakeLists.txt.  It costs nothing in any image --
 * an AmigaOS hunk records BSS as a length and no bytes -- so the only reason to
 * move it is the machine's memory, at 16 bytes an entry.
 *
 * 32 covers a bring-up of four interfaces, the shutdown of the same, and the
 * expunge that follows, with room left over.
 */
#ifndef AMINETXDUO_EVENT_RING
#  define AMINETXDUO_EVENT_RING  32
#endif

/* Long enough that no other publisher can hit it, and it names the tree. */
#define AMI_EVENTS_NAME     "AmiNetXDuo.Events"
#define AMI_EVENTS_MAGIC    0x414E5845UL        /* 'ANXE' */
#define AMI_EVENTS_VERSION  1

/*
 * The header, and the ring immediately after it.
 *
 * THE RING IS NOT A MEMBER, and em_Entries is why: its length is a build
 * option, so a command and a library that disagree about AMINETXDUO_EVENT_RING
 * would disagree about sizeof(AmiEventMark) and refuse each other over a
 * difference that changes nothing about the shape of anything.  em_Size is the
 * header alone and em_EntrySize is one entry, so both checks are about shape
 * and neither is about size.  A reader takes the count from the library that
 * wrote it.
 *
 * em_Next is where the next event goes.  Once em_Seq has passed em_Entries it
 * is also the oldest surviving entry, which is what puts a reader's walk in
 * the order things happened.
 */
typedef struct AmiEventMark
{
    /* First, so FindSemaphore() returns the mark itself. */
    struct SignalSemaphore em_Semaphore;

    ULONG   em_Magic;                   /* AMI_EVENTS_MAGIC                  */
    UWORD   em_Version;                 /* AMI_EVENTS_VERSION                */
    UWORD   em_Size;                    /* sizeof(AmiEventMark), the header  */
    UWORD   em_Entries;                 /* ring slots the library keeps      */
    UWORD   em_EntrySize;               /* sizeof(NetStatusEvent)            */
    ULONG   em_Next;                    /* the slot the next event goes in   */
    ULONG   em_Seq;                     /* events ever recorded              */
} AmiEventMark;

/* The ring, which follows the header. */
#define AMI_EVENTS_RING(mark) \
    ((const NetStatusEvent *)((const UBYTE *)(mark) + (mark)->em_Size))

/* index is an interface index, or NETEVENT_NOINDEX. */
VOID  ami_event(UWORD code, UWORD index, ULONG value);

/*
 * Oldest first.  Returns how many were written, and sets *held to how many the
 * ring holds, which is what a caller with too small a buffer needs.  How many
 * the machine recorded altogether is not reported here: the last entry's
 * nse_Seq is that number, and the first entry's says which ones went past.
 */
ULONG ami_event_snapshot(NetStatusEvent *out, ULONG room, ULONG *held);

/*
 * Publish the ring under AMI_EVENTS_NAME, and take it back.  Called from the
 * library's init and from its expunge; the remove must happen before anything
 * in the segment is freed.  Both are safe to call twice.
 */
VOID  ami_event_publish(VOID);
VOID  ami_event_unpublish(VOID);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_EVENTS_H */
