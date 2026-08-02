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
 * loopback; tests/perf/prof counted 38,853 over 1 MB).  A Forbid()/Permit()
 * pair through the library vectors costs 9,923 ns on the A1200 profile -- two
 * indirect jumps into a jump table that is not in the same memory as the
 * caller -- so the pair alone was 8.5% of that transfer.
 *
 * The bodies are now inline at every call site: tx_port.h has them, and the
 * reasoning for the AttnResched test is there too.  What is left here is the
 * rare tail, and the out-of-line entry points the port's own API still names.
 *
 * The offsets tx_port.h reaches Exec by are asserted against the NDK below.
 * A header that ever moved one of these fields fails the build instead of
 * silently corrupting the nest count.
 */

_Static_assert(__builtin_offsetof(struct ExecBase, TDNestCnt) == TX_AMIGA_OFF_TDNESTCNT,
               "ExecBase.TDNestCnt moved -- fix TX_AMIGA_OFF_TDNESTCNT");
_Static_assert(__builtin_offsetof(struct ExecBase, AttnResched) == TX_AMIGA_OFF_ATTNRESCHED,
               "ExecBase.AttnResched moved -- fix TX_AMIGA_OFF_ATTNRESCHED");
_Static_assert(sizeof(SysBase -> TDNestCnt) == 1, "TDNestCnt is not a byte");
_Static_assert(sizeof(SysBase -> AttnResched) == 2, "AttnResched is not a word");


/*
 * The tail of Permit(), reached from TX_RESTORE only when AttnResched is set.
 *
 * The decrement has already happened.  Exec reschedules when the count went
 * below zero, no interrupt is in progress and the attention word is set; when
 * all three hold the nesting is put back and the real Permit() is called, so
 * the reschedule is Exec's own and not a copy of it.  When any of them fails
 * the library call would have done nothing beyond the decrement, so returning
 * reproduces it exactly.
 */
VOID _tx_amiga_permit_finish(void)
{

    if ((SysBase -> TDNestCnt >= 0) || (SysBase -> IDNestCnt >= 0) ||
        (SysBase -> AttnResched == 0))
    {
        return;
    }

    __asm volatile ("addq.b #1,%0" : "+m" (SysBase -> TDNestCnt) : : "cc");
    TX_AMIGA_COUNT(TX_AMIGA_SC_PERMIT_SLOW);
    Permit();
}


/* The out-of-line spellings.  Nothing on the data path reaches them any more
   -- TX_DISABLE/TX_RESTORE are the inline pair -- but they are part of the
   port's surface and cost nothing unreferenced.  */

UINT _tx_thread_interrupt_disable(void)
{

    return(_tx_amiga_int_disable());
}


VOID _tx_thread_interrupt_restore(UINT previous_posture)
{

    _tx_amiga_int_restore(previous_posture);
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
