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

UINT _tx_thread_interrupt_disable(void)
{

UINT    previous_posture;


    previous_posture =  _tx_amiga_forbidden();
    Forbid();
    return(previous_posture);
}


VOID _tx_thread_interrupt_restore(UINT previous_posture)
{

    (VOID) previous_posture;
    Permit();
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
