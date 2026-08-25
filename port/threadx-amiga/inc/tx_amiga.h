/*
 * AmiNetXDuo, public API of the ThreadX AmigaOS/Exec port.
 *
 * Include after tx_api.h.
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
 * wants, because tx_kernel_enter() never returns, the task that calls it
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
 * Bring the kernel down and return only once nothing the port created will
 * execute again, no tick, no scheduler, no ThreadX thread.
 *
 * This is what makes it safe for a program to exit.  tx_amiga_kernel_start()
 * leaves two Exec Tasks running with their entry points inside the caller's
 * code hunk, and the tick fires 50 times a second; AmigaDOS unloads that hunk
 * the moment main() returns, so without this call the next tick executes freed
 * memory and takes the machine with it.  A resident shared library that never
 * goes away does not need to call this; anything that can be unloaded does.
 *
 * Torn down:
 *   1. the periodic tick Task, including its timer.device request;
 *   2. ThreadX's own system timer thread, ThreadX creates it, on a stack in
 *      ThreadX's BSS, so it is neither the application's to delete nor safe to
 *      leave behind;
 *   3. the master Task that has been sitting in tx_kernel_enter() since start;
 *   4. everything tx_amiga_kernel_start() allocated: both Task stacks, and the
 *      kernel memory block if the port allocated it rather than the caller.
 *
 * If any application TX_THREAD still exists, stop refuses and changes nothing.
 * It does not terminate them for you.  tx_thread_delete() of a thread that is
 * blocked in Exec rather than in ThreadX cannot reclaim it and produces a
 * zombie (see tx_amiga_zombie_tasks() below), so a stop that tore down the
 * caller's threads would convert a clear refusal into an invisible hazard.
 * Delete your own threads, then call this.
 *
 * It also refuses while any zombie has not yet unblocked
 * (tx_amiga_zombie_tasks_live() != 0).  A zombie cannot be reclaimed on demand,
 * and it will run code in the program's hunk when it finally wakes.  Only the
 * caller knows how to unstick the device that is holding it.
 *
 * Callable from any Exec Task the port did not create, including one that is
 * currently adopted: the caller's own thread does not count against it, and is
 * orphaned on its behalf, but only once the stop is going ahead, so a refusal
 * leaves the caller adopted and the kernel as it found them.  Calling it from a
 * Task the port created (a ThreadX thread, the tick, the master) returns
 * TX_CALLER_ERROR: it would be waiting for itself.  Idempotent: calling it on a
 * kernel that is already down returns TX_SUCCESS.
 *
 * Only TX_SUCCESS means "safe to exit":
 *
 *   TX_SUCCESS       Down.  Nothing of the port's is running.  Safe to return
 *                    to AmigaDOS, or to expunge the library.
 *   TX_THREAD_ERROR  Refused; the kernel is untouched and still usable.
 *                    Application threads or live zombies remain.  Not safe to
 *                    exit.
 *   TX_NO_MEMORY     Refused for want of an Exec signal; kernel untouched and
 *                    still usable.  Not safe to exit.
 *   TX_CALLER_ERROR  Wrong caller; kernel untouched.  Not safe to exit.
 *   TX_NOT_DONE      Teardown began and did not finish, something the port
 *                    created could not be woken.  The kernel is now unusable
 *                    and something is still running in the program's hunk.  Do
 *                    not exit.  Every failure is logged through ami_log().
 *
 * start -> stop -> start works, and is covered by tools/smoke/kernelstop.c.
 * ThreadX's own initialisation is re-run from scratch, which re-clears the
 * thread and timer components but not the semaphore, queue, mutex, event flag
 * or pool created-lists (TX_INLINE_INITIALIZATION).  Objects that outlive a
 * stop therefore stay linked into lists whose threads have been wiped: delete
 * every ThreadX object you created before stopping, as if the program were
 * exiting.
 */
UINT    tx_amiga_kernel_stop(VOID);

/*
 * How many Exec Tasks have outlived the TX_THREAD they backed.
 *
 * tx_thread_delete() removes the backing Exec Task by asking it to destroy
 * itself, and waits a bounded time for it to do so.  A thread that is blocked
 * inside Exec rather than inside ThreadX, parked in WaitIO() on a device
 * that ignores AbortIO(), say, cannot be woken and cannot safely be removed
 * by anyone else, so the wait times out.  The thread is then detached: it can
 * no longer touch its TX_THREAD, the ThreadX baton is recovered if it held
 * one, and the task destroys itself whenever it finally unblocks.
 *
 * This counter is the only signal that it happened.  A caller that sees it move
 * across a tx_thread_delete() must not free that thread's stack: the zombie is
 * still running on it.
 */
ULONG   tx_amiga_zombie_tasks(VOID);

/*
 * How many of those have not unblocked yet.
 *
 * tx_amiga_zombie_tasks() only ever goes up: it records that this happened.
 * This one goes back down as each zombie finally unblocks and destroys itself,
 * so it reports whether one is still outstanding.  Zero is a precondition of
 * tx_amiga_kernel_stop(), and a program with a non-zero count here cannot
 * safely be unloaded whatever else it does.
 */
ULONG   tx_amiga_zombie_tasks_live(VOID);


/* ------------------------------------------------------------------------ */
/* The periodic tick                                                         */
/* ------------------------------------------------------------------------ */

/*
 * What the tick task has been doing.
 *
 * The tick wakes from a cheap source (timer.device UNIT_VBLANK by preference)
 * but takes its time from ReadEClock(), so wakeups and delivered ticks are two
 * different numbers:
 *
 *   * delivered / uptime is the real clock rate, and should be
 *     TX_TIMER_TICKS_PER_SECOND however badly the source misbehaves.  Both
 *     numbers come from the same E-Clock, so this is the drift measurement.
 *   * wakeups / (wall seconds) is what the tick costs the machine.
 *   * clipped > 0 means something stalled the tick task for longer than
 *     TX_AMIGA_TIMER_MAX_CATCHUP ticks and the wheel was not given the arrears.
 *     tx_time_get() still has them: the ThreadX clock comes from the E-Clock and
 *     not from the number of ticks delivered, so a dropped tick costs timer
 *     lateness and not timekeeping.  skew is how much lateness.
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
    ULONG   tx_amiga_tick_uptime_ms;        /* WHOLE SECONDS of it, in ms     */
    /* The rest of the current second, in E-Clock ticks.  The tick task keeps
       this by subtraction because dividing by the E-Clock rate is a
       32-iteration shift-subtract on a 68000 -- 709379 does not fit the 16-bit
       divisor its DIVU takes -- and at 50 Hz that was 1.1% of an A600, found
       by Profile WATCH over the divide helper.  tx_amiga_uptime_ms() below
       puts the two together for whoever actually asks. */
    ULONG   tx_amiga_tick_uptime_rem;
    /* The longest gap between two wakeups, and what the wakeup before it
       spent, kept because only the first few are logged and a machine that
       then freezes never flushes the log anyway. A priority 20 task not
       dispatched for this long, next to a service cost in the hundreds of
       microseconds, means something else held the machine.

       Every wakeup, not only a clipped one. The clip was once the only writer,
       which left these at 0 on every machine healthy enough never to clip
       while tx_amiga_tick_skew_peak below said the wheel had been late. A peak
       of N ticks and a stall of N tick periods is one late wakeup; a peak
       larger than the stall accounts for is accumulation across several. */
    ULONG   tx_amiga_tick_worst_stall_ms;
    ULONG   tx_amiga_tick_worst_service_us;
    /* Catch-up bursts that hit TX_AMIGA_TIMER_BUDGET_MS and gave the machine
       back, and the ticks they put off to a later wakeup.  Deferred, not lost:
       the wheel gets every one of them, late.  Non-zero means the tick was
       being asked for more than half of its own period. */
    ULONG   tx_amiga_tick_over_budget;
    ULONG   tx_amiga_tick_deferred;
    /* How far behind real time the timer wheel is, in ticks: what it has yet to
       be given, plus everything the clip above took off it for good.  The clock
       is not in this, it comes from the E-Clock and is true regardless, so
       this is a measure of how late timers are running and of nothing else.
       The peak is sampled before a backlog is worked off, so it moves on a
       machine where nothing was ever clipped or lost.

       The peak has a floor of one wakeup's worth of ticks, because a wakeup is
       sampled owing everything that elapsed since the last one: on the VBlank
       source that is TX_AMIGA_VBLANK_DIVIDER, 2 on a stock build, so a healthy
       PAL machine reads 2 having never been late at all. It is not subtracted:
       what one wakeup owes depends on the source that woke it, and on the
       timer.device fallback it is 1. tx_amiga_tick_worst_stall_ms above is the
       figure that says whether a peak is one wakeup or several. */
    ULONG   tx_amiga_tick_skew;
    ULONG   tx_amiga_tick_skew_peak;
} TX_AMIGA_TICK_STATS;

VOID    tx_amiga_tick_stats(TX_AMIGA_TICK_STATS *stats);

/*
 * The same counters where they live, for a reader that cannot make the call
 * above: a debugger on a frozen machine, or the published anchor that points
 * at them (include/aminetxduo/health.h).  Anything running should still call
 * tx_amiga_tick_stats(), which returns one consistent snapshot.
 */
TX_AMIGA_TICK_STATS *tx_amiga_tick_stats_live(VOID);


/*
 * Uptime in milliseconds: whole seconds the tick task counted, plus the part
 * second it did not convert.
 *
 * THE DIVIDE IS HERE ON PURPOSE.  Callers are NetStat, the netstatus block and
 * the tickclock smoke test, all of which ask when a human or a test does; the
 * tick task runs fifty times a second at priority 20 and is the wrong place to
 * spend a 32-iteration divide on a number nobody reads at that rate.
 */
static __inline ULONG tx_amiga_uptime_ms(const TX_AMIGA_TICK_STATS *t)
{
    ULONG hz = t -> tx_amiga_tick_eclock_hz;

    if (hz == 0UL)
    {
        return(t -> tx_amiga_tick_uptime_ms);
    }
    return(t -> tx_amiga_tick_uptime_ms +
           ((t -> tx_amiga_tick_uptime_rem * 1000UL) / hz));
}


/*
 * E-Clock ticks to milliseconds and to microseconds.
 *
 * eclock_per_ms is the E-Clock rate divided by a thousand, which every caller
 * already holds; taking the rate instead would put a multiply by 1000 in front
 * of the millisecond conversion and wrap it above six seconds.
 */
static __inline ULONG tx_amiga_eclock_ms(ULONG ec, ULONG eclock_per_ms)
{
    if (eclock_per_ms == 0UL)
    {
        return(0UL);
    }
    return(ec / eclock_per_ms);
}

static __inline ULONG tx_amiga_eclock_us(ULONG ec, ULONG eclock_per_ms)
{
    if (eclock_per_ms == 0UL)
    {
        return(0UL);
    }

    /* ec * 1000 wraps a ULONG above about six seconds' worth, which is longer
       than anything measured in microseconds is worth reading; past that,
       divide first and lose the last three digits instead of the answer. */
    if (ec >= 4000000UL)
    {
        return((ec / eclock_per_ms) * 1000UL);
    }
    return((ec * 1000UL) / eclock_per_ms);
}


/* ------------------------------------------------------------------------ */
/* Thread adoption                                                           */
/* ------------------------------------------------------------------------ */

/*
 * Register the calling Exec Task with ThreadX as a TX_THREAD, so NetX Duo may
 * suspend and resume it (nx_tcp_socket_receive(), nx_packet_allocate(),
 * tx_mutex_get(), ... all suspend "the calling thread").
 *
 * thread_ptr must point at storage that stays valid until orphaned; it is
 * initialised here, do not pass it to tx_thread_create() as well.  No stack
 * is allocated: the Task already owns one, and _tx_thread_stack_build() binds
 * to it instead of building a frame.
 *
 * On return the calling Task holds the ThreadX baton: it is
 * _tx_thread_current_ptr and no other ThreadX thread is running.  The call
 * blocks until that is true.
 *
 * While adopted, the Task must not block on anything except ThreadX.  An
 * adopted Task that Wait()s on an Intuition port, a DOS packet or a device
 * IORequest holds the baton while unrunnable, and the whole stack, including
 * the NetX Duo IP thread and the periodic timer, stalls behind it.  Intended
 * usage is therefore: adopt on entry to a stack call, orphan on exit.  See
 * docs/RESEARCH.md 6.3 and the notes in tx_amiga_adopt.c.
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
 * A caller that owns persistent storage may therefore adopt once and use this
 * pair as its bracket.  Between them the thread is TX_SUSPENDED, not on any
 * ready list, never dispatched by the scheduler, and the baton is free, so
 * the "never block outside ThreadX while adopted" rule is unchanged: it applies
 * between resume and suspend as it applies between adopt and orphan.
 *
 *   tx_amiga_adopt_thread(&t, ...);           once
 *       tx_amiga_adopt_resume(&t);  ... work ...  tx_amiga_adopt_suspend(&t);
 *       tx_amiga_adopt_resume(&t);  ... work ...  tx_amiga_adopt_suspend(&t);
 *   tx_amiga_orphan_thread(&t);               once, on the same Task
 *
 * Both must be called by the Task that adopted, and both refuse anything else
 * with TX_CALLER_ERROR; a caller that gets that should fall back to a fresh
 * tx_amiga_adopt_thread(), which is always correct and merely slower.
 *
 * One lifetime this does not close: a Task that exits without orphaning leaves
 * a TX_SUSPENDED TX_THREAD in the created list whose tx_thread_amiga_task
 * points at freed memory.  Nothing dispatches a suspended thread and nothing
 * else resumes one, so it is inert only for as long as that stays true.
 * Storage for the TX_THREAD must outlive the Task or be freed by it.
 */
UINT    tx_amiga_adopt_resume(TX_THREAD *thread_ptr);
UINT    tx_amiga_adopt_suspend(TX_THREAD *thread_ptr);

#ifdef AMINETXDUO_GREEN_REALM
/*
 * The take-or-back-out form of tx_amiga_adopt_resume(), for the request
 * gate's free-baton fast path: resume the cached thread ONLY if that takes
 * the baton without a round trip (baton free, nothing ready that outranks),
 * and otherwise undo the resume entirely -- both under one Forbid(), so the
 * takeability check and the take are one atom and a decline leaves no trace.
 * TX_SUCCESS took the baton; TX_NOT_DONE declined (submit through the gate);
 * TX_CALLER_ERROR means what resume's does (fall back to a fresh adoption).
 */
UINT    tx_amiga_adopt_try_resume(TX_THREAD *thread_ptr);

/* Whether the baton looks immediately takeable.  A policy hint for the
   first-ever adoption on a task (no cached thread to try-resume yet); it can
   be stale by the time it is acted on, and both outcomes stay correct.  */
UINT    tx_amiga_baton_free(VOID);
#endif

/*
 * Deregister a thread adopted by some other Task, the teardown path for a
 * cached adoption whose owner is gone or is not the one closing.  The Exec
 * signal is not recovered, because only its owner may FreeSignal() it; if the
 * owner is alive it keeps a bit it will never use again, and if it is dead the
 * bit went with it.  Prefer tx_amiga_orphan_thread() whenever the caller is the
 * owner: it is the same work plus the signal.
 *
 * Returns TX_SUCCESS, TX_PTR_ERROR or TX_THREAD_ERROR.
 */
UINT    tx_amiga_discard_thread(TX_THREAD *thread_ptr);

/* The TX_THREAD the calling Exec Task was adopted as, or TX_NULL.  */
TX_THREAD *tx_amiga_adopted_thread(VOID);

/*
 * TX_TRUE if the calling Exec Task is the ThreadX baton holder, the port's
 * answer to "is a thread calling me", which on a hosted port is not the same
 * question as "is _tx_thread_system_state zero".  NetX Duo's caller-checking
 * macros use it; port/netxduo-amiga/inc/nx_port.h says why.
 */
UINT    tx_amiga_caller_is_thread(VOID);

/*
 * TX_TRUE if [start, start+size) overlaps the stack of a thread ThreadX still
 * has on its created list, which tx_thread_create() refuses with TX_PTR_ERROR.
 * This includes either range wholly containing the other and ranges that meet
 * at an endpoint.  A caller allocating a stack at run time asks first and
 * takes another block rather than failing.
 *
 * The case that makes this necessary is the one tx_amiga_adopt_resume() warns
 * about: an adopted thread's stack is its Exec Task's own, so a Task that
 * exits while still registered leaves a range that the allocator will hand out
 * again.
 */
UINT    tx_amiga_stack_in_use(const VOID *start, ULONG size);


/* ------------------------------------------------------------------------ */
/* Scheduling call counts                                                    */
/* ------------------------------------------------------------------------ */

#ifdef AMINETXDUO_SCHEDCOUNT

/*
 * How often the scheduling primitives were entered, so that a sampled share
 * can be turned into a cost per call.  Built only under
 * -DAMINETXDUO_SCHEDCOUNT=ON; see tx_port.h for what each one counts.
 *
 * sc_exec_dispatch and sc_exec_idle are Exec's own, read straight out of
 * SysBase: they are what Reschedule/Switch/Dispatch cost is charged against,
 * and no instrumentation of ours could produce them.
 */
typedef struct TX_AMIGA_SCHED_STATS_STRUCT
{
    ULONG   sc_disable;
    ULONG   sc_restore;
    ULONG   sc_permit_slow;
    ULONG   sc_mutex_get;
    ULONG   sc_mutex_put;
    ULONG   sc_sys_return;
    ULONG   sc_wake;
    ULONG   sc_sched_dispatch;
    ULONG   sc_sched_wait;
    ULONG   sc_park_wait;
    ULONG   sc_park_spurious;
    ULONG   sc_direct;
    ULONG   sc_exec_dispatch;               /* SysBase -> DispCount          */
    ULONG   sc_exec_idle;                   /* SysBase -> IdleCount          */
} TX_AMIGA_SCHED_STATS;

VOID    tx_amiga_sched_stats(TX_AMIGA_SCHED_STATS *stats);

#endif /* AMINETXDUO_SCHEDCOUNT */

/* ------------------------------------------------------------------------ */
/* The green realm (AMINETXDUO_GREEN_REALM)                                  */
/* ------------------------------------------------------------------------ */

/*
 * Built with -DAMINETXDUO_GREEN_REALM=ON, every thread the stack creates is
 * a coroutine inside the realm Task (real m68k stack switching) instead of
 * an Exec Task of its own.  These entry points exist in every build so
 * callers need no conditional code: in a baton build tx_amiga_green_active()
 * is always FALSE, tx_amiga_green_wait() is a plain Wait(), and the stats
 * read zero.
 */

/* TX_TRUE while the caller runs in a green context (on the realm Task with a
   green thread holding the baton).  */
UINT    tx_amiga_green_active(VOID);

/*
 * Sleep the calling green thread until one of sigmask's Exec signals is
 * latched on the realm Task; returns the bits that arrived.  This is the
 * green replacement for "release the baton, Wait(), reacquire": only the
 * calling thread sleeps, the realm keeps running everything else.  From a
 * non-green context it degrades to Wait(sigmask).  Must not be called
 * holding a ThreadX mutex.
 */
ULONG   tx_amiga_green_wait(ULONG sigmask);

typedef struct TX_AMIGA_GREEN_STATS_STRUCT
{
    ULONG   gs_switches;        /* green contexts entered (stack switches)   */
    ULONG   gs_external;        /* baton handoffs to adopted Exec Tasks      */
    ULONG   gs_idle_waits;      /* times the realm slept in its one Wait()   */
    ULONG   gs_wait_fast;       /* green waits satisfied by a latched signal */
    ULONG   gs_wait_slow;       /* green waits that suspended the thread     */
    ULONG   gs_stray_wait;      /* Exec Wait()s caught arriving from green
                                   context (probe builds; must stay zero)    */
    ULONG   gs_gate_calls;      /* bracket calls carried through the gate    */
    ULONG   gs_gate_fallback;   /* bracket calls the gate declined (adopted) */
    ULONG   gs_realm_sigbits;   /* Exec signal bits allocated on the realm
                                   Task, of the 16 allocatable: every green
                                   thread's MsgPort and AllocSignal draws
                                   from this one budget (the signal-bit
                                   audit's live figure)                      */
    ULONG   gs_gate_fast;       /* bracket calls that took a free baton
                                   directly (the fast path) instead of
                                   submitting through the gate; fast+gated
                                   +fallback partitions the brackets, and
                                   the fast share is what attributes the
                                   physical baton-leg numbers               */
} TX_AMIGA_GREEN_STATS;

VOID    tx_amiga_green_stats(TX_AMIGA_GREEN_STATS *stats);

/* Count one intercepted Exec Wait() from green context (the probe build's
   assert path in src/netstack/netstack_baton.c).  */
VOID    tx_amiga_green_stray_wait_note(VOID);

/* Count one gate decline (the caller fell back to the adopted-baton path;
   the counter lives in the port so it sits beside the calls it divides).  */
VOID    tx_amiga_gate_fallback_note(VOID);

/* Count one free-baton fast-path take (the caller entered without the gate
   because the realm was idle; same home as the other two for the same
   reason).  */
VOID    tx_amiga_gate_fast_note(VOID);

/* ------------------------------------------------------------------------ */
/* The request gate (AMINETXDUO_GREEN_REALM)                                 */
/* ------------------------------------------------------------------------ */

/*
 * The adopted-caller request gate, the green realm's client boundary.  An
 * application Task entering a bsdsocket vector no longer becomes a ThreadX
 * thread itself: its continuation -- the rest of
 * the vector body, captured at the bracket -- is handed to a cached GREEN
 * proxy thread and executes inside the realm, where a NetX suspension is a
 * stack switch instead of an Exec round trip.  The Task itself parks on a
 * small side stack in one plain Wait() until the vector completes: request
 * submission in, one boundary Signal back.
 *
 * The capture trick is that the request IS the continuation.  The vector
 * body between bsd_nx_enter() and bsd_nx_leave() runs unchanged, on the
 * caller's own stack memory, but under the realm Task's feet; the bracket
 * discipline the tree already enforces ("nothing inside a bracket blocks on
 * anything except ThreadX", include/aminetxduo/netstack.h) is exactly the
 * property that makes the region migratable.  Anything the body must know
 * about the parked owner's Exec state -- the Ctrl-C break bits -- is
 * collected by the parked Task and read through tx_amiga_gate_breaks().
 *
 * One gate per owner Task, bound on first use, torn down by the owner
 * (tx_amiga_gate_release) or by the heartbeat when the owner died
 * (tx_amiga_gate_orphan).  Green builds only; the baton build never
 * references these.
 */
typedef struct TX_AMIGA_GATE_STRUCT
{
    TX_THREAD        ag_Thread;     /* the green proxy: the caller's
                                       captured continuation                 */
    VOID            *ag_ResumeSP;   /* leave-side context, resumed by the
                                       parked owner                          */
    VOID            *ag_Side;       /* the parked owner's side stack         */
    VOID            *ag_Task;       /* owner (struct Task *); only it calls  */
    ULONG            ag_DoneMask;   /* completion signal, owner's bit        */
    ULONG            ag_BreakMask;  /* watched while parked, per call        */
    volatile ULONG   ag_Breaks;     /* break bits the parker collected       */
    volatile UINT    ag_Done;       /* completion flag; the signal's truth   */
    UINT             ag_Live;       /* the proxy exists                      */
    UINT             ag_Active;     /* between gate_call and gate_return     */
    volatile UINT    ag_OwnerDead;  /* heartbeat: owner exited uncleanly     */
} TX_AMIGA_GATE;

/* Bind the gate to the calling Task: allocate the side stack and the
   completion signal, create the dormant green proxy.  TX_SUCCESS or why not
   (TX_NO_MEMORY, TX_NOT_DONE with no kernel).  Owner's context only.  */
UINT    tx_amiga_gate_bind(TX_AMIGA_GATE *gate, CHAR *name, UINT priority);

/*
 * Submit the caller's continuation.  On success the call RETURNS ON THE
 * REALM, as the green proxy, and everything until tx_amiga_gate_return()
 * runs there; the owning Task is parked collecting break bits.  On any
 * refusal (foreign task, dead proxy, kernel down, proxy mid-teardown) it
 * returns the error on the calling Task and nothing happened: fall back to
 * the adopted-baton bracket.
 */
UINT    tx_amiga_gate_call(TX_AMIGA_GATE *gate, ULONG break_mask);

/* Complete the request: suspend the proxy, post the boundary Signal, and
   resume the parked owner at this exact point.  Returns on the OWNER.  */
VOID    tx_amiga_gate_return(TX_AMIGA_GATE *gate);

/* Break bits the parked owner has collected so far this call (observed, not
   consumed; they are re-posted to the owner at gate_return).  The gated
   replacement for SetSignal(0,0) & breakmask inside a bracket.  */
ULONG   tx_amiga_gate_breaks(const TX_AMIGA_GATE *gate);

/* Owner-context teardown (bsd_nx_release): destroy the dormant proxy, free
   the side stack and the signal bit.  */
VOID    tx_amiga_gate_release(TX_AMIGA_GATE *gate);

/*
 * Heartbeat teardown for a dead owner.  Marks the gate ownerless, and reaps
 * the proxy when that is safe: dormant, or suspended mid-flight (the realm
 * is not inside its context).  A proxy the realm is EXECUTING right now is
 * only marked; gate_return() then completes without signalling the corpse
 * and the next heartbeat reaps.  TX_TRUE once fully reaped, TX_FALSE while
 * deferred -- the sweeper must call again next beat.  Safe at the tick
 * task's interrupt level: no Wait(), no allocation.
 */
UINT    tx_amiga_gate_orphan(TX_AMIGA_GATE *gate);

#ifdef __cplusplus
}
#endif

#endif /* TX_AMIGA_H */
