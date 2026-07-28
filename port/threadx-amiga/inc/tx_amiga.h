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
 * Bring the kernel down and return only when Nothing the port owns will ever
 * EXECUTE AGAIN -- no tick, no scheduler, no ThreadX thread.
 *
 * This is what makes it safe for a PROGRAM to exit.  tx_amiga_kernel_start()
 * leaves two Exec Tasks running with their entry points inside the caller's
 * code hunk, and the tick fires 50 times a second; AmigaDOS unloads that hunk
 * the moment main() returns, so without this call the next tick executes freed
 * memory and takes the machine with it.  A resident shared library that never
 * goes away does not need to call this; anything that can be unloaded does.
 *
 *   1. the periodic tick Task, including its timer.device request;
 *   2. ThreadX's own system timer thread -- ThreadX creates it, on a stack in
 *      ThreadX's BSS, so it is neither the application's to delete nor safe to
 *      leave behind;
 *   3. the master Task that has been sitting in tx_kernel_enter() since start;
 *   4. everything tx_amiga_kernel_start() allocated: both Task stacks, and the
 *      kernel memory block if the port allocated it rather than the caller.
 *
 * The contract on outstanding threads -- IT REFUSES, It does not reap
 *
 * If any application TX_THREAD still exists, stop REFUSES and changes nothing.
 * It does not terminate them for you.  tx_thread_delete() of a thread that is
 * blocked in Exec rather than in ThreadX cannot reclaim it and produces a
 * zombie (see tx_amiga_zombie_tasks() below), so a stop that "helpfully" tore
 * down the caller's threads would convert a clear refusal into a hazard the
 * caller cannot even see.  Delete your own threads, then call this.
 *
 * Likewise it refuses while any zombie has not yet unblocked
 * (tx_amiga_zombie_tasks_live() != 0).  A zombie cannot be reclaimed on demand
 * -- that is what makes it one -- and it will run code in the program's hunk
 * when it finally wakes.  Only the caller knows how to unstick the device that
 * is holding it.
 *
 * Callable from any Exec Task the port did not create, including one that is
 * currently ADOPTED: the caller's own thread does not count against it, and is
 * orphaned on its behalf -- but only once the stop is going ahead, so a refusal
 * leaves the caller adopted and the kernel exactly as it found them.
 * Calling it from a Task the port created (a ThreadX thread, the tick, the
 * master) returns TX_CALLER_ERROR -- it would be waiting for itself.
 * Idempotent: calling it on a kernel that is already down returns TX_SUCCESS.
 *
 * RETURNS -- ONLY TX_SUCCESS MEANS "SAFE TO EXIT"
 *
 *   TX_SUCCESS       Down.  Nothing of the port's is running.  Safe to return
 *                    to AmigaDOS, or to expunge the library.
 *   TX_THREAD_ERROR  Refused; the kernel is untouched And still usable.
 *                    Application threads or live zombies remain.  NOT safe to
 *                    exit.
 *   TX_NO_MEMORY     Refused for want of an Exec signal; kernel untouched and
 *                    still usable.  NOT safe to exit.
 *   TX_CALLER_ERROR  Wrong caller; kernel untouched.  NOT safe to exit.
 *   TX_NOT_DONE      Teardown began and did not finish -- something the port
 *                    owns could not be woken.  The kernel is now unusable AND
 *                    something is still running in the program's hunk.  DO NOT
 *                    EXIT.  Every failure is logged through ami_log().
 *
 * start -> stop -> start works, and is covered by tools/smoke/kernelstop.c.
 * ThreadX's own initialisation is re-run from scratch, which re-clears the
 * thread and timer components -- but NOT the semaphore, queue, mutex, event
 * flag or pool created-lists (TX_INLINE_INITIALIZATION).  Objects that outlive
 * a stop therefore stay linked into lists whose threads have been wiped, so:
 * delete every ThreadX object you created before stopping, exactly as if the
 * program were exiting.  That is the same precondition stop already imposes on
 * threads, extended to the rest.
 */
UINT    tx_amiga_kernel_stop(VOID);

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
 * This counter is the only signal that it happened.  A Caller that sees it
 * MOVE ACROSS A tx_thread_delete() MUST NOT FREE THAT THREAD'S STACK: the
 * zombie is still running on it.
 */
ULONG   tx_amiga_zombie_tasks(VOID);

/*
 * How many of those have not unblocked yet.
 *
 * tx_amiga_zombie_tasks() only ever goes up: it answers "did this happen?".
 * This one goes back down as each zombie finally unblocks and destroys itself,
 * so it answers the question that matters at exit -- "is one still out there?".
 * Zero is a precondition of tx_amiga_kernel_stop(), and a program with a
 * non-zero count here cannot safely be unloaded whatever else it does.
 */
ULONG   tx_amiga_zombie_tasks_live(VOID);


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
 * On return the calling Task Holds the THREADX baton, i.e. it is
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

/*
 * A task that makes one stack call makes thousands.  Measured on a 14 MHz
 * 68020 (tests/perf/bracket_test.c), one adopt/orphan pair costs ~800 us, of
 * which the AllocSignal()/FreeSignal() is 17 us and everything else is
 * _tx_thread_create(), _tx_thread_terminate(), _tx_thread_delete() and the
 * scheduler poke.  That work is repeatable rather than per-call: the same task
 * gets the same TX_THREAD every time.
 *
 * So a caller that owns persistent storage may adopt ONCE and then use this
 * pair as its bracket.  Between them the thread is TX_SUSPENDED -- it is not
 * on any ready list, the scheduler will never dispatch it, and the baton is
 * free -- so the "never block outside ThreadX while adopted" contract is
 * unchanged: it applies between resume and suspend, exactly as it applies
 * between adopt and orphan.
 *
 *   tx_amiga_adopt_thread(&t, ...);           once
 *       tx_amiga_adopt_resume(&t);  ... work ...  tx_amiga_adopt_suspend(&t);
 *       tx_amiga_adopt_resume(&t);  ... work ...  tx_amiga_adopt_suspend(&t);
 *   tx_amiga_orphan_thread(&t);               once, on the same Task
 *
 * Both must be called by the Task that adopted, and both refuse anything else
 * with TX_CALLER_ERROR -- a caller that gets that should fall back to a fresh
 * tx_amiga_adopt_thread(), which is always correct and merely slower.
 *
 * The lifetime this does not close, stated because it is the reason to think
 * twice: a Task that exits without orphaning leaves a TX_SUSPENDED TX_THREAD
 * in the created list whose tx_thread_amiga_task points at freed memory.
 * Nothing dispatches a suspended thread and nothing else resumes one, so it is
 * inert -- but it is inert only for as long as that stays true.  Storage for
 * the TX_THREAD must outlive the Task or be freed by it.
 */
UINT    tx_amiga_adopt_resume(TX_THREAD *thread_ptr);
UINT    tx_amiga_adopt_suspend(TX_THREAD *thread_ptr);

/*
 * Deregister a thread adopted by SOME OTHER Task -- the teardown path for a
 * cached adoption whose owner is gone or is not the one closing.  The Exec
 * signal is NOT recovered, because only its owner may FreeSignal() it; if the
 * owner is alive it keeps a bit it will never use again, and if it is dead the
 * bit died with it.  Prefer tx_amiga_orphan_thread() whenever the caller is
 * the owner: it is the same work plus the signal.
 *
 * Returns TX_SUCCESS, TX_PTR_ERROR or TX_THREAD_ERROR.
 */
UINT    tx_amiga_discard_thread(TX_THREAD *thread_ptr);

/* The TX_THREAD the calling Exec Task was adopted as, or TX_NULL.  */
TX_THREAD *tx_amiga_adopted_thread(VOID);

#ifdef __cplusplus
}
#endif

#endif /* TX_AMIGA_H */
