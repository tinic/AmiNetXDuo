/***************************************************************************
 * Eclipse ThreadX, AmigaOS/m68k port.
 *
 * Derived from ports/linux/gnu/src/tx_timer_interrupt.c
 *   Copyright (c) 2024 Microsoft Corporation
 *   Copyright (c) 2026-present Eclipse ThreadX contributors
 *
 * SPDX-License-Identifier: MIT
 **************************************************************************/

/* _tx_timer_interrupt, AmigaOS/m68k: walks the timer wheel one slot, called from
   the tick task between _tx_thread_context_save() and _tx_thread_context_restore().
   It does NOT touch the system clock; _tx_amiga_timer_clock_advance() owns that. */

#define TX_SOURCE_CODE

#include "tx_amiga_internal.h"


VOID _tx_timer_interrupt(VOID)
{

    Forbid();

    /* Test for time-slice expiration.  */
    if (_tx_timer_time_slice != ((ULONG) 0))
    {

        _tx_timer_time_slice--;

        if (_tx_timer_time_slice == ((ULONG) 0))
        {
            _tx_timer_expired_time_slice =  TX_TRUE;
        }
    }

    /* Test for timer expiration.  */
    if (*_tx_timer_current_ptr != TX_NULL)
    {

        _tx_timer_expired =  TX_TRUE;
    }
    else
    {

        /* No timer expired, increment the timer pointer.  */
        _tx_timer_current_ptr++;

        /* Check for wrap-around.  */
        if (_tx_timer_current_ptr == _tx_timer_list_end)
        {
            _tx_timer_current_ptr =  _tx_timer_list_start;
        }
    }

    /* See if anything has expired.  */
    if ((_tx_timer_expired_time_slice == TX_TRUE) || (_tx_timer_expired == TX_TRUE))
    {

        if (_tx_timer_expired == TX_TRUE)
        {
            _tx_timer_expiration_process();
        }

        if (_tx_timer_expired_time_slice == TX_TRUE)
        {
            _tx_thread_time_slice();
        }
    }

    Permit();
}


/* The ThreadX system clock, which _tx_timer_interrupt() above no longer keeps.
   `ticks` is always an elapsed count from the caller's accumulated E-Clock anchor,
   never recomputed from a raw reading, so the clock can only move forward.  */
VOID _tx_amiga_timer_clock_advance(ULONG ticks)
{

    Forbid();
    _tx_timer_system_clock +=  ticks;
    Permit();
}
