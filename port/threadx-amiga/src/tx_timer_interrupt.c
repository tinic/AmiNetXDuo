/***************************************************************************
 * Eclipse ThreadX -- AmigaOS/m68k port.
 *
 * Derived from ports/linux/gnu/src/tx_timer_interrupt.c
 *   Copyright (c) 2024 Microsoft Corporation
 *   Copyright (c) 2026-present Eclipse ThreadX contributors
 *
 * SPDX-License-Identifier: MIT
 **************************************************************************/

/**************************************************************************/
/*                                                                        */
/*    _tx_timer_interrupt                              AmigaOS/m68k       */
/*                                                                        */
/*  DESCRIPTION                                                           */
/*                                                                        */
/*    Advances the ThreadX system clock and processes time-slice and       */
/*    timer expirations.  Called TX_TIMER_TICKS_PER_SECOND times a second  */
/*    from the tick task in tx_initialize_low_level.c, between             */
/*    _tx_thread_context_save() and _tx_thread_context_restore().          */
/*                                                                        */
/*    The Forbid()/Permit() here is redundant with the one the tick task   */
/*    already holds; it is kept so the function is safe to call from any   */
/*    other deferred-interrupt source we add later (e.g. a SANA-II reader  */
/*    that wants to drive the clock forward after a long DMA stall).       */
/*                                                                        */
/**************************************************************************/

#define TX_SOURCE_CODE

#include "tx_amiga_internal.h"


VOID _tx_timer_interrupt(VOID)
{

    Forbid();

    /* Increment the system clock.  */
    _tx_timer_system_clock++;

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
