/***************************************************************************
 * Eclipse ThreadX, AmigaOS/m68k port.
 *
 * Derived in structure from ports/linux/gnu/src/tx_thread_context_restore.c
 *   Copyright (c) 2024 Microsoft Corporation
 *   Copyright (c) 2026-present Eclipse ThreadX contributors
 *
 * SPDX-License-Identifier: MIT
 **************************************************************************/

/* _tx_thread_context_restore, AmigaOS/m68k: leaves "interrupt" context and releases
   the core lock _tx_thread_context_save() took.  Preemption is deferred, not
   asynchronous: the baton holder keeps it until its next ThreadX service call.  */

#define TX_SOURCE_CODE

#include "tx_amiga_internal.h"


VOID _tx_thread_context_restore(VOID)
{

    /* Decrement the nested interrupt count.  */
    _tx_thread_system_state--;

    if (_tx_thread_system_state == ((ULONG) 0))
    {

        if ((_tx_thread_current_ptr == TX_NULL) && (_tx_thread_execute_ptr != TX_NULL))
        {

            /* Idle system, and the tick made something runnable.  Dispatch it
               here; the scheduler Task would run the same lines a switch later. */
            if (_tx_amiga_dispatch_inline() == ((UINT) TX_FALSE))
            {
                _tx_amiga_wake_scheduler();
            }
        }
    }

    /* Release the core lock taken in _tx_thread_context_save().  */
    Permit();
}
