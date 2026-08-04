/***************************************************************************
 * Eclipse ThreadX, AmigaOS/m68k port.
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
/*    Walks the timer wheel one slot and processes time-slice and timer    */
/*    expirations.  Called up to TX_TIMER_TICKS_PER_SECOND times a second  */
/*    from the tick task in tx_initialize_low_level.c, between             */
/*    _tx_thread_context_save() and _tx_thread_context_restore().          */
/*                                                                        */
/*    It does NOT touch the system clock, which the stock ports increment  */
/*    here.  _tx_amiga_timer_clock_advance() below owns it, from the       */
/*    E-Clock, so that a wheel running behind does not also lose time.     */
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


/*
 * The ThreadX system clock, which _tx_timer_interrupt() above no longer keeps.
 *
 * The stock port increments it once per call, which makes tx_time_get() the
 * count of calls made rather than a measure of time: every tick the tick task
 * cannot deliver is time gone for good and never reconciled.  Here the tick
 * task reads ReadEClock() at each wakeup, works out how many whole tick periods
 * have really elapsed, and passes that, so the clock is true against real
 * time however many or few calls were made, and the wheel is free to run behind
 * it without taking timekeeping with it.
 *
 * `ticks` is always an elapsed count from the caller's accumulated E-Clock
 * anchor, never a value recomputed from a raw reading, so the clock can only
 * move forward.  It is ULONG and wraps at ~2.7 years at 50 Hz, which is what
 * ThreadX already assumes of it.
 *
 * An add, not tx_time_set(): the value is an increment, and the absolute call
 * would need a tx_time_get()/tx_time_set() pair with the clock unguarded in
 * between.  The direct write under Forbid() is what the port already does to
 * _tx_thread_current_ptr and _tx_thread_system_state.
 */
VOID _tx_amiga_timer_clock_advance(ULONG ticks)
{

    Forbid();
    _tx_timer_system_clock +=  ticks;
    Permit();
}
