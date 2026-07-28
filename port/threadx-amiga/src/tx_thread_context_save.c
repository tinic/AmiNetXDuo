/***************************************************************************
 * Eclipse ThreadX -- AmigaOS/m68k port.
 *
 * Derived in structure from ports/linux/gnu/src/tx_thread_context_save.c
 *   Copyright (c) 2024 Microsoft Corporation
 *   Copyright (c) 2026-present Eclipse ThreadX contributors
 *
 * SPDX-License-Identifier: MIT
 **************************************************************************/

/**************************************************************************/
/*                                                                        */
/*    _tx_thread_context_save                          AmigaOS/m68k       */
/*                                                                        */
/*  DESCRIPTION                                                           */
/*                                                                        */
/*    Enters "interrupt" context.  In a hosted port there is no register   */
/*    context to save -- Exec already did that when it preempted the       */
/*    running task in favour of the tick task.  What this must do is       */
/*    (a) stop the baton holder and (b) raise _tx_thread_system_state so   */
/*    that ThreadX services invoked from the tick defer their context      */
/*    switches to _tx_thread_context_restore().                            */
/*                                                                        */
/*    (a) is free: Forbid() stops every other task in the machine, so the  */
/*    ThreadX thread that holds the baton is frozen for the whole tick.    */
/*    This is the Amiga substitute for the Linux port's                    */
/*    pthread_kill(SUSPEND_SIG) -- cheaper and, unlike signals, it cannot  */
/*    land in the middle of a libc call.  The constraint that comes with   */
/*    it: nothing between context_save and context_restore may Wait(),     */
/*    because that would break the Forbid().                               */
/*                                                                        */
/*    The Forbid() taken here is released by _tx_thread_context_restore(). */
/*                                                                        */
/**************************************************************************/

#define TX_SOURCE_CODE

#include "tx_amiga_internal.h"


VOID _tx_thread_context_save(VOID)
{

    /* Take the core lock and keep it for the duration of the "interrupt".  */
    Forbid();

    /* Increment the nested interrupt condition.  */
    _tx_thread_system_state++;
}
