/***************************************************************************
 * Eclipse ThreadX, AmigaOS/m68k port.
 *
 * Derived in structure from ports/linux/gnu/src/tx_thread_context_restore.c
 *   Copyright (c) 2024 Microsoft Corporation
 *   Copyright (c) 2026-present Eclipse ThreadX contributors
 *
 * SPDX-License-Identifier: MIT
 **************************************************************************/

/**************************************************************************/
/*                                                                        */
/*    _tx_thread_context_restore                       AmigaOS/m68k       */
/*                                                                        */
/*  DESCRIPTION                                                           */
/*                                                                        */
/*    Leaves "interrupt" context and releases the core lock taken by       */
/*    _tx_thread_context_save().                                           */
/*                                                                        */
/*    Preemption is deferred rather than asynchronous.  If the tick made a */
/*    higher priority thread ready while another thread held the baton,    */
/*    this function does not take the baton away.  It cannot: the baton    */
/*    holder is an Exec Task stopped at an arbitrary instruction, and      */
/*    AmigaOS has no equivalent of pthread_kill()/sigsuspend() with which  */
/*    to park it safely.  Instead the running thread keeps the baton until */
/*    its next _tx_thread_system_preempt_check(), which every blocking     */
/*    ThreadX service performs, and passes it on there.                    */
/*                                                                        */
/*    So ThreadX priorities are honoured at ThreadX API boundaries rather  */
/*    than at tick granularity.  For a network stack whose threads block   */
/*    constantly this costs latency, not correctness.  It does mean a      */
/*    ThreadX thread that neither blocks nor calls a ThreadX service will  */
/*    not be preempted, see the adoption notes in tx_amiga_adopt.c.      */
/*                                                                        */
/*    When the system was idle (no baton holder) the scheduler task is     */
/*    poked so it dispatches whatever the tick made ready.                 */
/*                                                                        */
/**************************************************************************/

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
               here; the scheduler Task would only run the same ten lines one
               Exec context switch later.  */
            if (_tx_amiga_dispatch_inline() == ((UINT) TX_FALSE))
            {
                _tx_amiga_wake_scheduler();
            }
        }
    }

    /* Release the core lock taken in _tx_thread_context_save().  */
    Permit();
}
