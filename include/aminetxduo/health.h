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
#define AMI_HEALTH_VERSION  3

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
} AmiHealthMark;

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_HEALTH_H */
