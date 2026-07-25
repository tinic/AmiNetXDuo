/*
 * AmiNetXDuo -- public API of the ThreadX AmigaOS/Exec port.
 *
 * Include AFTER tx_api.h.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef TX_AMIGA_H
#define TX_AMIGA_H

#include "tx_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Kernel start-up                                                           */
/* ------------------------------------------------------------------------ */

/*
 * Give ThreadX the memory region that tx_application_define() receives as
 * "first unused memory".  Must be called before tx_kernel_enter(); otherwise
 * the port AllocMem()s TX_AMIGA_MEMORY_SIZE bytes itself.
 */
VOID    tx_amiga_set_kernel_memory(VOID *memory, ULONG size);

/*
 * Start ThreadX on a private Exec Task and return once tx_application_define()
 * has run and the scheduler is live.  This is the entry point a shared library
 * wants, because tx_kernel_enter() never returns -- the task that calls it
 * becomes the ThreadX scheduler for the lifetime of the kernel.
 *
 * A standalone program may equally well just call tx_kernel_enter() from
 * main() in the usual ThreadX shape.
 *
 * Returns TX_SUCCESS, TX_NO_MEMORY or TX_NOT_DONE.
 */
UINT    tx_amiga_kernel_start(VOID);

/* TX_TRUE once the scheduler is running.  */
UINT    tx_amiga_kernel_running(VOID);

/*
 * How many Exec Tasks have outlived the TX_THREAD they backed.
 *
 * tx_thread_delete() removes the backing Exec Task by asking it to destroy
 * itself, and waits a bounded time for it to do so.  A thread that is blocked
 * inside Exec rather than inside ThreadX -- parked in WaitIO() on a device
 * that ignores AbortIO(), say -- cannot be woken and cannot safely be removed
 * by anyone else, so the wait times out.  The thread is then detached: it can
 * no longer touch its TX_THREAD, the ThreadX baton is recovered if it held
 * one, and the task destroys itself whenever it finally unblocks.
 *
 * This counter is the only signal that it happened.  A CALLER THAT SEES IT
 * MOVE ACROSS A tx_thread_delete() MUST NOT FREE THAT THREAD'S STACK: the
 * zombie is still running on it.
 */
ULONG   tx_amiga_zombie_tasks(VOID);


/* ------------------------------------------------------------------------ */
/* The periodic tick                                                         */
/* ------------------------------------------------------------------------ */

/*
 * What the tick task has actually been doing.
 *
 * The tick wakes from a cheap source (timer.device UNIT_VBLANK by preference)
 * but takes its TIME from ReadEClock(), so wakeups and delivered ticks are two
 * different numbers and both are worth seeing:
 *
 *   * delivered / uptime is the real clock rate, and it should be
 *     TX_TIMER_TICKS_PER_SECOND however badly the source misbehaves.  Both
 *     numbers come from the same E-Clock, so this is the drift measurement
 *     with nothing else in the way.
 *   * wakeups / (wall seconds) is what the tick costs the machine.
 *   * clipped > 0 means something stalled the tick task for longer than
 *     TX_AMIGA_TIMER_MAX_CATCHUP ticks and the arrears were dropped.
 *
 * Every field is a snapshot; the counters are cumulative from kernel start and
 * are allowed to wrap.  Safe to call from any Task.
 */

typedef struct TX_AMIGA_TICK_STATS_STRUCT
{
    ULONG   tx_amiga_tick_unit;             /* timer.device unit in use       */
    ULONG   tx_amiga_tick_fallback;         /* TX_TRUE if VBlank was rejected */
    ULONG   tx_amiga_tick_eclock_hz;        /* E-Clock rate ReadEClock gave   */
    ULONG   tx_amiga_tick_source_chz;       /* measured source rate, Hz * 100 */
    ULONG   tx_amiga_tick_wakeups;          /* times the task ran             */
    ULONG   tx_amiga_tick_delivered;        /* _tx_timer_interrupt() calls    */
    ULONG   tx_amiga_tick_empty;            /* wakeups that delivered nothing */
    ULONG   tx_amiga_tick_catchups;         /* wakeups that delivered >1      */
    ULONG   tx_amiga_tick_clipped;          /* catch-ups that hit the cap     */
    ULONG   tx_amiga_tick_lost;             /* ticks dropped by those clips   */
    ULONG   tx_amiga_tick_service_us;       /* total time IN the task, us     */
    ULONG   tx_amiga_tick_uptime_ms;        /* E-Clock ms since the tick began*/
} TX_AMIGA_TICK_STATS;

VOID    tx_amiga_tick_stats(TX_AMIGA_TICK_STATS *stats);


/* ------------------------------------------------------------------------ */
/* Thread adoption                                                           */
/* ------------------------------------------------------------------------ */

/*
 * Register the CALLING Exec Task with ThreadX as a TX_THREAD, so that NetX Duo
 * may suspend and resume it (nx_tcp_socket_receive(), nx_packet_allocate(),
 * tx_mutex_get(), ... all suspend "the calling thread").
 *
 * thread_ptr must point at storage that stays valid until orphaned; it is
 * initialised here -- do not pass it to tx_thread_create() as well.  No stack
 * is allocated: the Task already owns one, and _tx_thread_stack_build() binds
 * to it instead of building a frame.
 *
 * On return the calling Task HOLDS THE THREADX BATON, i.e. it is
 * _tx_thread_current_ptr and no other ThreadX thread is running.  The call
 * blocks until that is true.
 *
 *   *** While adopted, the Task must not block on anything except ThreadX. ***
 *
 * An adopted Task that Wait()s on an Intuition port, a DOS packet or a device
 * IORequest is holding the baton while unrunnable, and the whole stack --
 * including the NetX Duo IP thread and the periodic timer -- stalls behind it.
 * The intended usage is therefore: adopt on entry to a stack call, orphan on
 * exit.  See docs/RESEARCH.md 6.3 and the notes in tx_amiga_adopt.c.
 *
 * priority is a ThreadX priority (0..TX_MAX_PRIORITIES-1); the Task's Exec
 * priority is untouched.
 *
 * Returns TX_SUCCESS, TX_NO_MEMORY (no free Exec signal), TX_PRIORITY_ERROR,
 * TX_PTR_ERROR or TX_NOT_DONE (kernel not running).
 */
UINT    tx_amiga_adopt_thread(TX_THREAD *thread_ptr, CHAR *name, UINT priority);

/*
 * Release the baton, deregister the TX_THREAD and free the Exec signal.  Must
 * be called by the same Task that adopted, and only while it holds the baton.
 * After this the Task is an ordinary Exec Task again and may block on anything.
 *
 * Returns TX_SUCCESS, TX_THREAD_ERROR or TX_CALLER_ERROR.
 */
UINT    tx_amiga_orphan_thread(TX_THREAD *thread_ptr);

/* The TX_THREAD the calling Exec Task was adopted as, or TX_NULL.  */
TX_THREAD *tx_amiga_adopted_thread(VOID);

#ifdef __cplusplus
}
#endif

#endif /* TX_AMIGA_H */
