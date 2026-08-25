/* AmiNetXDuo, public API of the ThreadX AmigaOS/Exec port; include after tx_api.h.
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

/* Adopt the calling Exec Task as a TX_THREAD.  Returns holding the ThreadX baton;
   until orphaned the Task must not block on anything but ThreadX.  thread_ptr is
   initialised here, not by tx_thread_create(), and must outlive the adoption.  */
UINT    tx_amiga_adopt_thread(TX_THREAD *thread_ptr, CHAR *name, UINT priority);

/* Release the baton, deregister the TX_THREAD and free the Exec signal.  Must be
   called by the same Task that adopted, and only while it holds the baton.
   Returns TX_SUCCESS, TX_THREAD_ERROR or TX_CALLER_ERROR.  */
UINT    tx_amiga_orphan_thread(TX_THREAD *thread_ptr);

/* Bracket for a cached adoption: resume takes the baton, suspend gives it back,
   and the "never block outside ThreadX" rule applies between them.  Both must be
   called by the Task that adopted; anything else gets TX_CALLER_ERROR.  */
UINT    tx_amiga_adopt_resume(TX_THREAD *thread_ptr);
UINT    tx_amiga_adopt_suspend(TX_THREAD *thread_ptr);

#ifdef AMINETXDUO_GREEN_REALM
/* Take-or-back-out resume: TX_SUCCESS took the baton, TX_NOT_DONE declined and
   left no trace (submit through the gate), TX_CALLER_ERROR as resume's.  */
UINT    tx_amiga_adopt_try_resume(TX_THREAD *thread_ptr);

/* Whether the baton looks immediately takeable.  A hint only: it can be stale by
   the time it is acted on, and both outcomes stay correct.  */
UINT    tx_amiga_baton_free(VOID);
#endif

/* Deregister a thread adopted by some other Task.  The Exec signal is NOT
   recovered -- only its owner may FreeSignal() it -- so prefer
   tx_amiga_orphan_thread() whenever the caller is the owner.  */
UINT    tx_amiga_discard_thread(TX_THREAD *thread_ptr);

/* The TX_THREAD the calling Exec Task was adopted as, or TX_NULL.  */
TX_THREAD *tx_amiga_adopted_thread(VOID);

/* TX_TRUE if the calling Exec Task is the ThreadX baton holder, which on a hosted
   port is not the same question as "is _tx_thread_system_state zero".  */
UINT    tx_amiga_caller_is_thread(VOID);

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

/* ------------------------------------------------------------------------ */
/* The green realm (AMINETXDUO_GREEN_REALM)                                  */
/* ------------------------------------------------------------------------ */

/* With -DAMINETXDUO_GREEN_REALM every thread the stack creates is a coroutine
   inside the realm Task.  These entry points exist in every build: in a baton
   build green_active() is FALSE, green_wait() is a plain Wait(), stats read 0. */

/* TX_TRUE while the caller runs in a green context (on the realm Task with a
   green thread holding the baton).  */
UINT    tx_amiga_green_active(VOID);

/* Sleep the calling green thread until one of sigmask's Exec signals is latched
   on the realm Task; returns the bits that arrived.  From a non-green context it
   is a plain Wait(sigmask).  Must not be called holding a ThreadX mutex.  */
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
    ULONG   gs_realm_sigbits;   /* Exec signal bits out on the realm Task    */
    ULONG   gs_gate_fast;       /* brackets that took a free baton directly  */
} TX_AMIGA_GREEN_STATS;

VOID    tx_amiga_green_stats(TX_AMIGA_GREEN_STATS *stats);

/* Count one intercepted Exec Wait() from green context (probe builds).  */
VOID    tx_amiga_green_stray_wait_note(VOID);

/* Count one gate decline (the caller fell back to the adopted-baton path).  */
VOID    tx_amiga_gate_fallback_note(VOID);

/* Count one free-baton fast-path take.  */
VOID    tx_amiga_gate_fast_note(VOID);

/* ------------------------------------------------------------------------ */
/* The request gate (AMINETXDUO_GREEN_REALM)                                 */
/* ------------------------------------------------------------------------ */

/* The adopted-caller request gate: the vector body between bracket entry and exit
   is captured as a continuation and run by a cached green proxy inside the realm
   while the owner Task parks.  One gate per owner Task; green builds only.  */
typedef struct TX_AMIGA_GATE_STRUCT
{
    TX_THREAD        ag_Thread;     /* green proxy: captured continuation    */
    VOID            *ag_ResumeSP;   /* leave-side context, resumed by owner  */
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
/* Submit the caller's continuation.  On success the call RETURNS ON THE REALM as
   the green proxy, with the owner parked, until tx_amiga_gate_return(); on any
   refusal it returns on the calling Task and nothing happened.  */
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

/* Heartbeat teardown for a dead owner.  TX_TRUE once fully reaped, TX_FALSE while
   deferred because the realm is executing the proxy -- the sweeper must call again
   next beat.  Safe at the tick task's level: no Wait(), no allocation.  */
UINT    tx_amiga_gate_orphan(TX_AMIGA_GATE *gate);

#ifdef __cplusplus
}
#endif

#endif /* TX_AMIGA_H */
