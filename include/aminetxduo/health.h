/*
 * AmiNetXDuo, the scheduling counters, published live under AMI_HEALTH_NAME.
 * Read with Forbid() + magic check + copy; never obtain the semaphore.
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_HEALTH_H
#define AMINETXDUO_HEALTH_H

#include <exec/types.h>
#include <exec/semaphores.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AMI_HEALTH_NAME     "AmiNetXDuo.Health"
#define AMI_HEALTH_MAGIC    0x414E5848UL        /* 'ANXH' */
/*
 * Bump whenever the shape of any hm_-pointed struct changes: a reader copies
 * them whole, so one that does not know the version must read none of them.
 */
#define AMI_HEALTH_VERSION  5

typedef struct AmiHealthMark
{
    /* First, so FindSemaphore() returns the mark itself. */
    struct SignalSemaphore hm_Semaphore;

    ULONG   hm_Magic;                   /* AMI_HEALTH_MAGIC                  */
    UWORD   hm_Version;                 /* AMI_HEALTH_VERSION                */
    UWORD   hm_Size;                    /* sizeof(AmiHealthMark)             */

    APTR    hm_Tick;                    /* TX_AMIGA_TICK_STATS *, live       */
    APTR    hm_Baton;                   /* AmiBatonStats *, live             */
    APTR    hm_Mem;                     /* AmiMemStats *, live               */

    /*
     * VOID * const *, live: the Exec Task that holds the ThreadX baton right
     * now, or NULL.  A reader that wants to act on it must read it and act
     * inside ONE Forbid(), or the holder can change underneath.  A thread that
     * holds the baton has necessarily been published, so this one word says
     * both "adopted and running in the stack" and "its slot is real".
     */
    APTR    hm_Holder;

    /*
     * struct SignalSemaphore *, live: the bsdsocket master lock, or NULL when
     * no base has published one.  A diagnostic reader wants ss_Owner, which is
     * Exec's own field, so this exposes nothing of bsdsocket's layout.  It is
     * here because a Task removed while owning this semaphore wedges the
     * library in a way the baton reclaim cannot and does not address, and a
     * test about the reclaim has to be able to keep out of that case.
     */
    APTR    hm_SbLock;
} AmiHealthMark;

/* bsdsocket publishes its master lock into hm_SbLock through this, and clears
   it with NULL when the base goes.  Implemented in src/netstack. */
VOID ami_netstack_health_set_sblock(APTR sem);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_HEALTH_H */
