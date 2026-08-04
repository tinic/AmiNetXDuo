/*
 * AmiNetXDuo, the scheduling counters, where a freeze cannot hide them.
 *
 * A total lockup leaves no Enforcer hit, no MungWall hit and no log line: the
 * machine stops before anything can be written, and the reset that follows
 * takes the evidence with it.  So the counters that say how the stack was
 * being scheduled, the tick task's stalls (TX_AMIGA_TICK_STATS) and what the
 * baton bracket did (AmiBatonStats), are published at a place that can be
 * found without the stack's cooperation.
 *
 * NETSTATUS_HEALTH reports the same numbers and is the way to read them from a
 * program.  This is the other way in, for the cases where that call cannot be
 * made: a debugger on the frozen machine, or a command that must not risk
 * opening a library whose stack may be the thing that is stuck.
 *
 * One AmiHealthMark is published under the public semaphore name
 * AMI_HEALTH_NAME for as long as the stack is up.  Three ways to it, in order
 * of how much working machine each needs:
 *
 *   FindSemaphore(AMI_HEALTH_NAME)   a running machine.  The semaphore is the
 *                                    first member, so the pointer is the mark.
 *   SysBase->SemaphoreList           a debugger with no OS left to call.
 *   a scan for hm_Magic              a debugger with no SysBase either.
 *
 * Every hm_ pointer is at the live counters rather than at a copy, so what is
 * read at the moment of the freeze is what the stack had at the moment of the
 * freeze, with no staleness to allow for.
 *
 * hm_Mem is the other fault this answers: a suspected leak.  AvailMem falls
 * for every program on the machine and cannot say whose; these are the stack's
 * own allocations, sockets and packets, with a high-water mark on each, so a
 * number that climbs and never comes back is visible without a debugger.
 *
 * Reading is a Forbid(), a magic check and a copy.  Nothing obtains the
 * semaphore: blocking on a machine that may already be wedged is the one thing
 * a diagnostic must not do.  The Forbid() is against a concurrent shutdown,
 * the mark is removed under Forbid() before the segment holding these counters
 * can go away.
 *
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
 * 3 since the mark grew hm_Mem.  Each hm_ pointer is at a live struct that a
 * reader copies whole, so a reader that disagrees about any of their shapes
 * must not read any of them: the version is what stops that.  A reader that
 * finds a version it does not know reports no stack rather than a wrong
 * number, and the commands ship with the library.
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
