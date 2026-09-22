/* AmiNetXDuo, public API of the ThreadX AmigaOS/Exec port; include after tx_api.h.
 * SPDX-License-Identifier: MIT
 */

#ifndef TX_AMIGA_H
#define TX_AMIGA_H

#include "tx_api.h"
#include "aminetxduo/exec_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Kernel start-up                                                           */
/* ------------------------------------------------------------------------ */

/* Give ThreadX the memory tx_application_define() receives as "first unused
   memory".  Must be called before tx_kernel_enter(); otherwise the port
   AllocMem()s TX_AMIGA_MEMORY_SIZE bytes itself.  */
VOID    tx_amiga_set_kernel_memory(VOID *memory, ULONG size);

/* Start ThreadX on a private Exec Task and return once the scheduler is live:
   the entry point a shared library wants, since tx_kernel_enter() never returns.
   Returns TX_SUCCESS, TX_NO_MEMORY or TX_NOT_DONE.  */
UINT    tx_amiga_kernel_start(VOID);

/* TX_TRUE once the scheduler is running.  */
UINT    tx_amiga_kernel_running(VOID);

/* Bring the kernel down; returns only once nothing the port created will run
   again.  Refuses while an application TX_THREAD or a live zombie remains, or if
   called from a Task the port created.  Delete your ThreadX objects first.  */
UINT    tx_amiga_kernel_stop(VOID);

/* Exec Tasks that outlived the TX_THREAD they backed; only ever goes up.  A
   caller that sees it move across a tx_thread_delete() must not free that
   thread's stack: the zombie is still running on it.  */
ULONG   tx_amiga_zombie_tasks(VOID);

/* How many of those have not unblocked yet.  Zero is a precondition of
   tx_amiga_kernel_stop(), and no program with a non-zero count can be unloaded. */
ULONG   tx_amiga_zombie_tasks_live(VOID);


/* ------------------------------------------------------------------------ */
/* The periodic tick                                                         */
/* ------------------------------------------------------------------------ */

/* Cumulative from kernel start, and allowed to wrap.  The wakeup source is not
   the time base, so wakeups and delivered ticks are different numbers.  */

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
    /* The rest of the current second, in E-Clock ticks.  */
    ULONG   tx_amiga_tick_uptime_rem;
    /* The longest gap between two wakeups, and what the wakeup before it spent. */
    ULONG   tx_amiga_tick_worst_stall_ms;
    ULONG   tx_amiga_tick_worst_service_us;
    /* Bursts that hit TX_AMIGA_TIMER_BUDGET_MS, and the ticks they deferred.  */
    ULONG   tx_amiga_tick_over_budget;
    ULONG   tx_amiga_tick_deferred;
    /* Ticks the wheel still owes plus everything clipped; the clock is not in it. */
    ULONG   tx_amiga_tick_skew;
    ULONG   tx_amiga_tick_skew_peak;
} TX_AMIGA_TICK_STATS;

VOID    tx_amiga_tick_stats(TX_AMIGA_TICK_STATS *stats);

/* The same counters where they live, for a debugger on a frozen machine or the
   published anchor.  Anything running should call tx_amiga_tick_stats().  */
TX_AMIGA_TICK_STATS *tx_amiga_tick_stats_live(VOID);


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


/* E-Clock ticks to milliseconds and to microseconds.  eclock_per_ms is the
   E-Clock RATE DIVIDED BY A THOUSAND; passing the rate itself wraps the
   millisecond conversion above six seconds.  */
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

    /* ec * 1000 wraps a ULONG above about six seconds; past that, divide first
       and lose the last three digits instead of the answer.  */
    if (ec >= 4000000UL)
    {
        return((ec / eclock_per_ms) * 1000UL);
    }
    return((ec * 1000UL) / eclock_per_ms);
}


/* ------------------------------------------------------------------------ */
/* Thread adoption                                                           */
/* ------------------------------------------------------------------------ */

/* Adopt the calling Exec Task as a TX_THREAD.  Returns holding the ThreadX
   baton; until orphaned the Task must not block on anything but ThreadX.

   THE TX_THREAD IS THE PORT'S, NOT THE CALLER'S.  It comes from a fixed pool
   (tx_amiga_pool.c), and what the caller keeps is a HANDLE: *thread_ptr plus
   the *generation that went with it.  Both must be given back to every other
   call below, and a handle whose slot has since been recycled is refused with
   TX_THREAD_ERROR rather than acted on.  Caller-owned storage is not an option:
   a Task RemTask()ed while adopted frees its own stack, and the recovery has to
   read the TX_THREAD after that.

   `reserved` is TX_TRUE only for a caller that MUST NOT PARK -- in this tree,
   one holding ami_ns_lock.  Only such a caller may have the pool's reserved
   tail; tx_amiga_pool.c says why one slot is provably enough.

   Waits, holding nothing, when every slot this caller may have is taken.
   TX_NO_MEMORY means no Exec signal was free, or more Tasks were already
   waiting than the port tracks; TX_NOT_DONE means the kernel is not running,
   including a kernel that stopped while the caller waited.  */
UINT    tx_amiga_adopt_thread(TX_THREAD **thread_ptr, ULONG *generation,
                              CHAR *name, UINT priority, UINT reserved);

/* Release the baton, deregister the TX_THREAD, free the Exec signal and give
   the pool slot back.  Must be called by the same Task that adopted, and only
   while it holds the baton.  TX_SUCCESS, TX_THREAD_ERROR (stale handle) or
   TX_CALLER_ERROR.  */
UINT    tx_amiga_orphan_thread(TX_THREAD *thread_ptr, ULONG generation);

/* Bracket for a cached adoption: resume takes the baton, suspend gives it back,
   and the "never block outside ThreadX" rule applies between them.  The slot is
   kept across the pair.  Both must be called by the Task that adopted; anything
   else gets TX_CALLER_ERROR, and a stale handle TX_THREAD_ERROR.  */
UINT    tx_amiga_adopt_resume(TX_THREAD *thread_ptr, ULONG generation);
UINT    tx_amiga_adopt_suspend(TX_THREAD *thread_ptr, ULONG generation);


/* Deregister a thread adopted by some other Task and give its slot back.  The
   Exec signal is NOT recovered -- only its owner may FreeSignal() it -- so
   prefer tx_amiga_orphan_thread() whenever the caller is the owner.  */
UINT    tx_amiga_discard_thread(TX_THREAD *thread_ptr, ULONG generation);

/* The pool.  tx_amiga_adopt_generation() answers 0 for anything that is not a
   live slot, including a pointer that is not the port's at all, and reads no
   memory outside the pool to decide; call it under Forbid(), which is also what
   keeps the answer true for the length of the caller's next call.  */
ULONG   tx_amiga_adopt_slots(VOID);
ULONG   tx_amiga_adopt_reserve(VOID);
ULONG   tx_amiga_adopt_slots_free(VOID);

/* Give back every slot whose claimer was removed between taking it and
   creating the TX_THREAD in it -- the one dead-adopter residue the baton
   reclaim cannot see, because there is no thread to ask about.  Called from
   the stack's second tick; takes its own Forbid().  How many it has ever freed
   is _tx_amiga_adopt_unpublished_freed.  */
VOID    tx_amiga_adopt_sweep_unpublished(VOID);

/* FOR THE PORT'S OWN HARNESS: leave exactly that residue.  */
UINT    tx_amiga_adopt_claim_orphan(VOID);
ULONG   tx_amiga_adopt_generation(TX_THREAD *thread_ptr);
UINT    tx_amiga_adopt_handle_valid(TX_THREAD *thread_ptr, ULONG generation);

/* Adoptions that had to wait for a slot, the high-water slot count, and the
   times a caller was refused because the waiter table was full.  */
extern ULONG _tx_amiga_adopt_parks;
extern ULONG _tx_amiga_adopt_waiting;   /* waiting right now                  */
extern ULONG _tx_amiga_adopt_peak;
extern ULONG _tx_amiga_adopt_waiter_full;
extern ULONG _tx_amiga_adopt_unpublished_freed;

/* Times tx_amiga_discard_thread() found a discarded baton holder had left the
   core's preemption lockout raised and cleared it.  */
extern ULONG _tx_amiga_discard_preempt_resets;

/* The TX_THREAD the calling Exec Task was adopted as, or TX_NULL.  */
TX_THREAD *tx_amiga_adopted_thread(VOID);

/* TX_TRUE if the calling Exec Task is the ThreadX baton holder, which on a hosted
   port is not the same question as "is _tx_thread_system_state zero".  */
UINT    tx_amiga_caller_is_thread(VOID);

/* A ThreadX thread sometimes has to block in Exec (Wait/WaitIO) without
   keeping the single hosted scheduler baton.  These calls are the port
   boundary for that transaction; application code must use the higher-level
   netstack bracket instead.

   The *_locked calls require one surrounding Forbid().  They remove/resume
   `thread_ptr` on the ThreadX ready list without dispatching through Exec and
   return the ordinary ThreadX status.  *wake is TX_TRUE when the caller must
   invoke tx_amiga_exec_wait_wake() after Permit(); *moved is TX_TRUE when the
   thread being released was no longer the current baton holder.  Park is
   called after the resume-side Permit() and returns TX_FALSE only when an
   adopted task was orphaned while it slept. */
ULONG   tx_amiga_exec_wait_system_state_locked(VOID);
TX_THREAD *tx_amiga_exec_wait_current_locked(VOID);
TX_THREAD *tx_amiga_exec_wait_owner_locked(VOID);
UINT    tx_amiga_exec_wait_release_locked(TX_THREAD *thread_ptr,
                                           UINT *wake, UINT *moved);
UINT    tx_amiga_exec_wait_resume_locked(TX_THREAD *thread_ptr, UINT *wake);
VOID    tx_amiga_exec_wait_wake(VOID);
UINT    tx_amiga_exec_wait_park(TX_THREAD *thread_ptr);

/* TX_TRUE if [start, start+size) overlaps the stack of a thread ThreadX still has
   on its created list, which tx_thread_create() refuses with TX_PTR_ERROR.
   Ranges that merely meet at an endpoint count.  */
UINT    tx_amiga_stack_in_use(const VOID *start, ULONG size);


/* ------------------------------------------------------------------------ */
/* Scheduling call counts                                                    */
/* ------------------------------------------------------------------------ */

#ifdef AMINETXDUO_SCHEDCOUNT

/* How often the scheduling primitives were entered; built only under
   -DAMINETXDUO_SCHEDCOUNT.  sc_exec_* are Exec's own, read out of SysBase.  */
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

#ifdef __cplusplus
}
#endif


/* ThreadX's timer ISR entry.  Upstream declares it in no header -- it is
   reached from the port's assembly -- so the port declares it, and the
   definition in tx_timer_interrupt.c is checked against this.

   Declaring it adds one -fanalyzer entry to tools/analyzer-baseline.txt, and
   that entry is a GCC bug, not a finding.  `typedef void VOID; VOID f(VOID);
   VOID f(VOID) {}' in three lines on its own reports "use of uninitialized
   value '<return-value>'"; the same three lines written with plain `void'
   report nothing.  Six sibling files in this directory are in the baseline
   for exactly that reason already. */
VOID    _tx_timer_interrupt(VOID);

#endif /* TX_AMIGA_H */
