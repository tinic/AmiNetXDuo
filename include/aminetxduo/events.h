/*
 * Recording an event, and the mark the ring is published at.  A call to
 * ami_event() must never carry a string, a format or a pointer to either.
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

/* A build option; the floor is in CMakeLists.txt. */
#ifndef AMINETXDUO_EVENT_RING
#  define AMINETXDUO_EVENT_RING  32
#endif

/* Long enough that no other publisher can hit it, and it names the tree. */
#define AMI_EVENTS_NAME     "AmiNetXDuo.Events"
#define AMI_EVENTS_MAGIC    0x414E5845UL        /* 'ANXE' */
#define AMI_EVENTS_VERSION  1

/*
 * The header, with the ring immediately after it and never a member: em_Size
 * is the header alone, em_EntrySize one entry, and a reader must take the slot
 * count from em_Entries rather than from its own AMINETXDUO_EVENT_RING.
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

/* Oldest first.  Returns how many were written, and sets *held to how many the
   ring holds. */
ULONG ami_event_snapshot(NetStatusEvent *out, ULONG room, ULONG *held);

/* Publish the ring under AMI_EVENTS_NAME, and take it back.  The unpublish
   must happen before anything in the segment is freed.  Both idempotent. */
VOID  ami_event_publish(VOID);
VOID  ami_event_unpublish(VOID);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_EVENTS_H */
