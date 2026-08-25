/***************************************************************************
 * Eclipse ThreadX, AmigaOS/m68k port.
 *
 * Derived in structure from ports/linux/gnu/src/tx_thread_interrupt_control.c
 *   Copyright (c) 2024 Microsoft Corporation
 *   Copyright (c) 2026-present Eclipse ThreadX contributors
 *
 * SPDX-License-Identifier: MIT
 **************************************************************************/

/* _tx_thread_interrupt_control, AmigaOS/m68k: ThreadX's interrupt lockout is
   Forbid()/Permit(), not Disable()/Enable() -- nothing here runs at Exec interrupt
   level, and a long Disable() drops serial characters and floppy transfers.  */

#define TX_SOURCE_CODE

#include "tx_amiga_internal.h"


/* TX_DISABLE / TX_RESTORE.  These are always strictly paired in the ThreadX
   core, so the saved posture is informational and the nesting is carried by
   Exec's TDNestCnt.  */

/* The nest counter is touched directly rather than through the library vectors;
   the bodies are inline in tx_port.h.  The offsets that header reaches Exec by are
   asserted below, so a moved field fails the build instead of corrupting it.  */

_Static_assert(__builtin_offsetof(struct ExecBase, TDNestCnt) == TX_AMIGA_OFF_TDNESTCNT,
               "ExecBase.TDNestCnt moved, fix TX_AMIGA_OFF_TDNESTCNT");
_Static_assert(__builtin_offsetof(struct ExecBase, AttnResched) == TX_AMIGA_OFF_ATTNRESCHED,
               "ExecBase.AttnResched moved, fix TX_AMIGA_OFF_ATTNRESCHED");
_Static_assert(sizeof(SysBase -> TDNestCnt) == 1, "TDNestCnt is not a byte");
_Static_assert(sizeof(SysBase -> AttnResched) == 2, "AttnResched is not a word");


/* The tail of Permit(), reached from TX_RESTORE only when AttnResched is set.  The
   decrement has already happened; when all three of Exec's conditions hold the
   nesting is put back and the real Permit() runs, so the reschedule is Exec's.  */
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


/* The out-of-line spellings.  Nothing on the data path reaches them -- TX_DISABLE
   and TX_RESTORE are the inline pair -- but they are part of the port's surface. */

UINT _tx_thread_interrupt_disable(void)
{

    return(_tx_amiga_int_disable());
}


VOID _tx_thread_interrupt_restore(UINT previous_posture)
{

    _tx_amiga_int_restore(previous_posture);
}


/* tx_interrupt_control(), the application-visible service.  It may be called
   unbalanced, so it changes the nesting by at most one level and reports the old
   posture: TX_INT_ENABLE inside N nested Forbid()s drops one level, not all N.  */
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
        /* Already enabled, nothing to do.  */
    }

    return(old_posture);
}
