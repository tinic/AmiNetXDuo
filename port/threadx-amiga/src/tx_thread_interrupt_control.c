/***************************************************************************
 * Eclipse ThreadX -- AmigaOS/m68k port.
 *
 * Derived in structure from ports/linux/gnu/src/tx_thread_interrupt_control.c
 *   Copyright (c) 2024 Microsoft Corporation
 *   Copyright (c) 2026-present Eclipse ThreadX contributors
 *
 * SPDX-License-Identifier: MIT
 **************************************************************************/

/**************************************************************************/
/*                                                                        */
/*    _tx_thread_interrupt_control                     AmigaOS/m68k       */
/*                                                                        */
/*  DESCRIPTION                                                           */
/*                                                                        */
/*    ThreadX's "interrupt lockout" is Forbid()/Permit() here, not         */
/*    Disable()/Enable() (docs/RESEARCH.md 6.2).  Nothing in ThreadX or    */
/*    NetX Duo runs at Exec interrupt level in this design, and a long     */
/*    Disable() region is harmful on an Amiga: it drops serial characters, */
/*    breaks floppy transfers and audibly glitches audio.  Forbid() gives  */
/*    the same mutual exclusion against every other task at a fraction of  */
/*    the system cost, and it nests, matching the save/restore posture     */
/*    idiom.                                                               */
/*                                                                        */
/*    Interrupt handlers really must not touch ThreadX state.  Data        */
/*    arriving from a SANA-II device reaches the stack as an IORequest     */
/*    completion picked up by a task, never from the device's own server.  */
/*                                                                        */
/**************************************************************************/

#define TX_SOURCE_CODE

#include "tx_amiga_internal.h"


/* TX_DISABLE / TX_RESTORE.  These are always strictly paired in the ThreadX
   core, so the saved posture is informational and the nesting is carried by
   Exec's TDNestCnt.  */

/*
 * Why the nest counter is touched directly rather than through Forbid() and
 * Permit().
 *
 * These two run on every critical section in ThreadX and NetX Duo, which is
 * about 26 pairs per received TCP segment (tests/perf/perf_test.c counted
 * 10,042 over a 256 KB transfer across the simulated wire, and 2,537 over
 * loopback).  A Forbid()/Permit() pair through the library vectors costs
 * 9,923 ns on the A1200 profile -- two indirect jumps into a jump table that
 * is not in the same memory as the caller -- so the pair alone was 8.5% of
 * that transfer.
 *
 * Forbid() is one instruction: ADDQ.B #1,TDNestCnt(A6).  The inline asm is
 * there to guarantee it stays one instruction; a read-modify-write the
 * compiler split into three could lose a nesting level against an interrupt.
 * A single ADDQ.B cannot, because a 68k takes interrupts only between
 * instructions.
 *
 * Permit() is one instruction plus a test.  Exec's own Permit decrements and
 * then reschedules only when all three of these hold: the count went below
 * zero (this was the outermost Forbid), no interrupt is in progress, and the
 * attention word has something in it.  When any of them fails the library
 * call does nothing beyond the decrement, so the sequence below reproduces it
 * exactly.  When all three hold, the nesting is put back and the real
 * Permit() is called, which reschedules the way it always did -- the
 * reschedule path is Exec's, not a copy of it.
 *
 * The third test is `AttnResched != 0` rather than the ARF_AttnSwitch bit.
 * That bit number is in exec/execbase.i and in no C header, and the whole
 * word being clear is enough to prove Exec's Permit would not have
 * rescheduled.  A word that is set for some other reason costs one library
 * call that was going to happen anyway.
 */

static __inline VOID _tx_amiga_forbid_inline(VOID)
{
    __asm volatile ("addq.b #1,%0" : "+m" (SysBase -> TDNestCnt) : : "cc");
}

static __inline VOID _tx_amiga_permit_inline(VOID)
{
    __asm volatile ("subq.b #1,%0" : "+m" (SysBase -> TDNestCnt) : : "cc");

    if ((SysBase -> TDNestCnt >= 0) || (SysBase -> IDNestCnt >= 0) ||
        (SysBase -> AttnResched == 0))
    {
        return;
    }

    __asm volatile ("addq.b #1,%0" : "+m" (SysBase -> TDNestCnt) : : "cc");
    Permit();
}

UINT _tx_thread_interrupt_disable(void)
{

UINT    previous_posture;

    previous_posture =  _tx_amiga_forbidden();
    _tx_amiga_forbid_inline();
    return(previous_posture);
}


VOID _tx_thread_interrupt_restore(UINT previous_posture)
{

    (VOID) previous_posture;
    _tx_amiga_permit_inline();
}


/*
 * tx_interrupt_control() -- the application-visible service.
 *
 * Unlike the pair above, this one may be called unbalanced, so it changes the
 * nesting by at most one level and reports what the posture was.  Requesting
 * TX_INT_ENABLE from inside N nested Forbid()s drops one level, not all N.
 * ThreadX's own core never calls this.
 */
UINT _tx_thread_interrupt_control(UINT new_posture)
{

UINT    old_posture;


    old_posture =  _tx_amiga_forbidden();

    if (new_posture == ((UINT) TX_INT_DISABLE))
    {
        Forbid();
    }
    else if (old_posture == ((UINT) TX_INT_DISABLE))
    {
        Permit();
    }
    else
    {
        /* Already enabled -- nothing to do.  */
    }

    return(old_posture);
}
