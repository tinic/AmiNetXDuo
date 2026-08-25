/***************************************************************************
 * Eclipse ThreadX, AmigaOS/m68k port.
 *
 * Derived in structure from ports/linux/gnu/src/tx_thread_context_save.c
 *   Copyright (c) 2024 Microsoft Corporation
 *   Copyright (c) 2026-present Eclipse ThreadX contributors
 *
 * SPDX-License-Identifier: MIT
 **************************************************************************/

/* _tx_thread_context_save, AmigaOS/m68k: enters "interrupt" context.  The Forbid()
   freezes the baton holder for the whole tick and is released by
   _tx_thread_context_restore(); nothing between the two may Wait().  */

#define TX_SOURCE_CODE

#include "tx_amiga_internal.h"


VOID _tx_thread_context_save(VOID)
{

    /* Take the core lock and keep it for the duration of the "interrupt".  */
    Forbid();

    /* Increment the nested interrupt condition.  */
    _tx_thread_system_state++;
}
