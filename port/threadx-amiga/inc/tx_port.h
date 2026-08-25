/***************************************************************************
 * Eclipse ThreadX, AmigaOS / m68k port.
 *
 * Derived from ports/linux/gnu/inc/tx_port.h
 *   Copyright (c) 2024 Microsoft Corporation
 *   Copyright (c) 2026-present Eclipse ThreadX contributors
 * and from the AmiNetXDuo build spike (spike/amiport/tx_port.h).
 *
 * This program and the accompanying materials are made available under the
 * terms of the MIT License which is available at
 * https://opensource.org/licenses/MIT.
 *
 * SPDX-License-Identifier: MIT
 **************************************************************************/

/* tx_port.h, AmigaOS/m68k.  Must not include any AmigaOS header: it is pulled
   into every ThreadX and NetX Duo core file, and <exec/types.h> redefines
   VOID/ULONG/SHORT/USHORT unless ThreadX's typedefs come first.  */

#ifndef TX_PORT_H
#define TX_PORT_H


/* Determine if the optional ThreadX user define file should be used.  */

#ifdef TX_INCLUDE_USER_DEFINE_FILE
#include "tx_user.h"
#endif


/* Define compiler library include files.  No <stdio.h>: a shared library build
   must not drag newlib's stdio in.  */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>


/* Define ThreadX basic types for this port.  m68k is ILP32, so LONG/ULONG are
   32 bits and match both NetX Duo's assumption and Exec's own typedefs.  */

typedef void                                    VOID;
typedef char                                    CHAR;
typedef unsigned char                           UCHAR;
typedef int                                     INT;
typedef unsigned int                            UINT;
typedef long                                    LONG;
typedef unsigned long                           ULONG;
typedef short                                   SHORT;
typedef unsigned short                          USHORT;
typedef uint64_t                                ULONG64;
#define ULONG64_DEFINED


/* Define the priority levels for ThreadX.  */

#ifndef TX_MAX_PRIORITIES
#define TX_MAX_PRIORITIES                       32
#endif


/* Minimum stack for a ThreadX thread.  On AmigaOS the ThreadX stack is the
   Exec Task stack (see tx_thread_stack_build.c), so this is a real limit and
   68k frames are not small.  */

#ifndef TX_MINIMUM_STACK
#define TX_MINIMUM_STACK                        1024
#endif


/* Timer expiration runs where the tick is delivered.  With the tick and the
   timer thread both Exec Tasks, deferring it stalls the wheel; the trade is that
   a callback runs under the tick's Forbid() and may not block or take a mutex. */
#define TX_TIMER_PROCESS_IN_ISR


/* Do not pre-fill thread stacks: an adopted thread's stack is a live Exec Task
   stack and filling it would destroy the caller's own frames.  Stack checking
   (TX_ENABLE_STACK_CHECKING) is consequently unavailable.  */

#define TX_DISABLE_STACK_FILLING


/* No notify callbacks.  _tx_thread_terminate() calls them inside its TX_DISABLE
   (a Forbid()), and the adopt/discard paths add a Forbid() of their own with
   _tx_thread_system_state raised; a callback that blocked would break both.  */

#define TX_DISABLE_NOTIFY_CALLBACKS


/* Define various constants for the port.  */

#define TX_INT_DISABLE                          1           /* Forbid()  */
#define TX_INT_ENABLE                           0           /* Permit()  */


/* Trace time source.  No free-running counter is assumed.  */

#ifndef TX_TRACE_TIME_SOURCE
#define TX_TRACE_TIME_SOURCE                    0
#endif
#ifndef TX_TRACE_TIME_MASK
#define TX_TRACE_TIME_MASK                      0xFFFFFFFFUL
#endif


/* Port specific options word for _tx_build_options.  */

#define TX_PORT_SPECIFIC_BUILD_OPTIONS          0


/* In-line initialization for the modules that support it.  */

#define TX_INLINE_INITIALIZATION


/* ------------------------------------------------------------------------ */
/* Scheduling call counters.                                                 */
/* ------------------------------------------------------------------------ */

/* Entry counts for the scheduling primitives (-DAMINETXDUO_SCHEDCOUNT; free when
   off).  An array with an index enum, not a struct, because this header must stay
   includable before <exec/types.h>.  Every increment is inside a Forbid().  */

#define TX_AMIGA_SC_DISABLE         0       /* _tx_thread_interrupt_disable  */
#define TX_AMIGA_SC_RESTORE         1       /* _tx_thread_interrupt_restore  */
#define TX_AMIGA_SC_PERMIT_SLOW     2       /* restores that reached Permit()*/
#define TX_AMIGA_SC_MUTEX_GET       3       /* tx_mutex_get(), via nx_port.h */
#define TX_AMIGA_SC_MUTEX_PUT       4
#define TX_AMIGA_SC_SYS_RETURN      5       /* _tx_thread_system_return()    */
#define TX_AMIGA_SC_WAKE            6       /* _tx_amiga_wake_scheduler()    */
#define TX_AMIGA_SC_SCHED_DISPATCH  7       /* batons handed out by the task */
#define TX_AMIGA_SC_SCHED_WAIT      8       /* Wait()s in _tx_thread_schedule*/
#define TX_AMIGA_SC_PARK_WAIT       9       /* Wait()s in the park loop      */
#define TX_AMIGA_SC_PARK_SPURIOUS   10      /* wakes that had no baton       */
#define TX_AMIGA_SC_DIRECT          11      /* batons passed peer to peer    */
#define TX_AMIGA_SC_MAX             12

#ifdef AMINETXDUO_SCHEDCOUNT
extern ULONG    _tx_amiga_sched_count[TX_AMIGA_SC_MAX];
#define TX_AMIGA_COUNT(which)                   (_tx_amiga_sched_count[(which)]++)
#else
#define TX_AMIGA_COUNT(which)                   ((VOID) 0)
#endif


/* ------------------------------------------------------------------------ */
/* Interrupt (critical section) control.                                     */
/* ------------------------------------------------------------------------ */

/* TX_DISABLE/TX_RESTORE are strictly balanced throughout the ThreadX core,
   so they map onto the Forbid()/Permit() nest counter directly and the saved
   posture is informational only.  */

UINT   _tx_thread_interrupt_disable(void);
VOID   _tx_thread_interrupt_restore(UINT previous_posture);

/* Expanded at the call site, as the ThreadX porting model expects.  Exec is reached
   by offset, not by <exec/execbase.h>, which would drag exec/types.h in ahead of
   the typedefs above; the offsets are asserted in tx_thread_interrupt_control.c. */

#define TX_AMIGA_OFF_TDNESTCNT      0x0127                  /* BYTE  */
#define TX_AMIGA_OFF_ATTNRESCHED    0x012A                  /* UWORD */

/* SysBase, loaded with asm because -Warray-bounds rejects a deref of absolute
   location 4.  Not volatile: the value never changes, so GCC may hoist it.  */
static __inline__ __attribute__((always_inline)) char *_tx_amiga_execbase(void)
{

char   *base;


    __asm__ ("move.l 4,%0" : "=a" (base));

    return(base);
}

/* Both fields off one base register: GCC will not hold a volatile address across
   a volatile access, and reloads location 4 for each.  */
#define TX_AMIGA_TDNESTCNT_AT(b)    (*((volatile signed char *) \
                                       ((b) + TX_AMIGA_OFF_TDNESTCNT)))
#define TX_AMIGA_ATTNRESCHED_AT(b)  (*((volatile unsigned short *) \
                                       ((b) + TX_AMIGA_OFF_ATTNRESCHED)))

/* The rare tail of Permit(): defined in tx_thread_interrupt_control.c, which
   can see Exec.  Reached only when a reschedule may be owed.  */
VOID   _tx_amiga_permit_finish(void);

/* Forbid() is ADDQ.B #1,TDNestCnt.  The asm is there to keep it ONE instruction:
   a read-modify-write the compiler split into three could lose a nesting level
   against an interrupt, and a 68k takes interrupts between instructions.  */
static __inline__ __attribute__((always_inline)) UINT _tx_amiga_int_disable(void)
{

UINT    previous_posture;
char   *execbase;


    execbase =  _tx_amiga_execbase();

    previous_posture =  (TX_AMIGA_TDNESTCNT_AT(execbase) >= 0)
                        ? ((UINT) TX_INT_DISABLE) : ((UINT) TX_INT_ENABLE);

    __asm__ __volatile__ ("addq.b #1,%0"
                          : "+m" (TX_AMIGA_TDNESTCNT_AT(execbase)) : : "cc");

    TX_AMIGA_COUNT(TX_AMIGA_SC_DISABLE);

    return(previous_posture);
}

/* Permit() is the decrement plus a decision.  A clear AttnResched proves Exec's
   Permit would not have rescheduled, so returning here is exactly what the library
   call would have done; when set, _tx_amiga_permit_finish() applies the full test. */
static __inline__ __attribute__((always_inline)) void _tx_amiga_int_restore(UINT previous_posture)
{

char   *execbase;


    (void) previous_posture;

    TX_AMIGA_COUNT(TX_AMIGA_SC_RESTORE);

    execbase =  _tx_amiga_execbase();

    __asm__ __volatile__ ("subq.b #1,%0"
                          : "+m" (TX_AMIGA_TDNESTCNT_AT(execbase)) : : "cc");

    if (TX_AMIGA_ATTNRESCHED_AT(execbase) != 0)
    {
        _tx_amiga_permit_finish();
    }
}

#define TX_INTERRUPT_SAVE_AREA                  UINT tx_saved_posture;
#define TX_DISABLE                              tx_saved_posture = _tx_amiga_int_disable();
#define TX_RESTORE                              _tx_amiga_int_restore(tx_saved_posture);


/* Per-object lockout macros.  */

#define TX_BLOCK_POOL_DISABLE                   TX_DISABLE
#define TX_BYTE_POOL_DISABLE                    TX_DISABLE
#define TX_EVENT_FLAGS_GROUP_DISABLE            TX_DISABLE
#define TX_MUTEX_DISABLE                        TX_DISABLE
#define TX_QUEUE_DISABLE                        TX_DISABLE
#define TX_SEMAPHORE_DISABLE                    TX_DISABLE


/* ------------------------------------------------------------------------ */
/* Control block extensions.                                                 */
/* ------------------------------------------------------------------------ */

/* tx_thread_amiga_signal_owner is not a duplicate of tx_thread_amiga_task:
   _tx_amiga_reap() clears the task on teardown, but only the Task that
   AllocSignal()d the run signal may FreeSignal() it, one of that Task's 32.  */

#define TX_THREAD_EXTENSION_0                   VOID  *tx_thread_amiga_task; \
                                                VOID  *tx_thread_amiga_signal_owner; \
                                                ULONG  tx_thread_amiga_run_signal; \
                                                UINT   tx_thread_amiga_suspension_type; \
                                                UINT   tx_thread_amiga_flags;

#define TX_THREAD_EXTENSION_1                   VOID  *tx_thread_extension_ptr;
#define TX_THREAD_EXTENSION_2
#define TX_THREAD_EXTENSION_3

/* tx_thread_amiga_flags bits.  */
#define TX_AMIGA_THREAD_ADOPTED                 0x0001U   /* pre-existing Exec Task  */
#define TX_AMIGA_THREAD_DIE                     0x0002U   /* teardown requested      */
#define TX_AMIGA_THREAD_ORPHANED                0x0004U   /* woken but no longer ours */
#define TX_AMIGA_THREAD_GREEN                   0x0008U   /* green: no Exec Task     */


#define TX_BLOCK_POOL_EXTENSION
#define TX_BYTE_POOL_EXTENSION
#define TX_EVENT_FLAGS_GROUP_EXTENSION
#define TX_MUTEX_EXTENSION
#define TX_QUEUE_EXTENSION
#define TX_SEMAPHORE_EXTENSION
#define TX_TIMER_EXTENSION

#ifndef TX_THREAD_USER_EXTENSION
#define TX_THREAD_USER_EXTENSION
#endif


/* Object create/delete extensions, all white space for this port.  */

#define TX_BLOCK_POOL_CREATE_EXTENSION(pool_ptr)
#define TX_BLOCK_POOL_DELETE_EXTENSION(pool_ptr)
#define TX_BYTE_POOL_CREATE_EXTENSION(pool_ptr)
#define TX_BYTE_POOL_DELETE_EXTENSION(pool_ptr)
#define TX_EVENT_FLAGS_GROUP_CREATE_EXTENSION(group_ptr)
#define TX_EVENT_FLAGS_GROUP_DELETE_EXTENSION(group_ptr)
#define TX_MUTEX_CREATE_EXTENSION(mutex_ptr)
#define TX_MUTEX_DELETE_EXTENSION(mutex_ptr)
#define TX_QUEUE_CREATE_EXTENSION(queue_ptr)
#define TX_QUEUE_DELETE_EXTENSION(queue_ptr)
#define TX_SEMAPHORE_CREATE_EXTENSION(semaphore_ptr)
#define TX_SEMAPHORE_DELETE_EXTENSION(semaphore_ptr)
#define TX_TIMER_CREATE_EXTENSION(timer_ptr)
#define TX_TIMER_DELETE_EXTENSION(timer_ptr)
#define TX_THREAD_CREATE_EXTENSION(thread_ptr)
#define TX_THREAD_DELETE_EXTENSION(thread_ptr)

/* A terminated thread may be sitting in a green signal wait; its waiter slot must
   go with it, or the realm keeps consuming its mask's bits for a thread that can
   never collect them.  Runs under the core lock, which is the Forbid() it wants. */
struct TX_THREAD_STRUCT;
void    _tx_amiga_thread_terminated(struct TX_THREAD_STRUCT *thread_ptr);
#define TX_THREAD_TERMINATED_EXTENSION(thread_ptr)    _tx_amiga_thread_terminated(thread_ptr);
#define TX_THREAD_STACK_BUILD_STATUS(thread_ptr)      \
    (((thread_ptr) -> tx_thread_amiga_task != (VOID *) 0) ? TX_SUCCESS : TX_NO_MEMORY)
#define TX_TIMER_INITIALIZE_EXTENSION(a)
#define TX_BYTE_ALLOCATE_EXTENSION
#define TX_BYTE_RELEASE_EXTENSION
#define TX_MUTEX_PUT_EXTENSION_1
#define TX_MUTEX_PUT_EXTENSION_2
#define TX_MUTEX_PRIORITY_CHANGE_EXTENSION
#define TX_THREAD_STACK_ANALYZE_EXTENSION
#define TX_INITIALIZE_KERNEL_ENTER_EXTENSION
#define TX_PORT_SPECIFIC_PRE_INITIALIZATION
#define TX_PORT_SPECIFIC_POST_INITIALIZATION
#define TX_TRACE_PORT_EXTENSION
#define TX_SAFETY_CRITICAL_EXCEPTION_HANDLER


struct TX_THREAD_STRUCT;

/* Post-completion hooks so that the backing Exec Task is removed when a
   thread is deleted or reset.  */

void _tx_thread_delete_port_completion(struct TX_THREAD_STRUCT *thread_ptr, UINT tx_saved_posture);
#define TX_THREAD_DELETE_PORT_COMPLETION(thread_ptr)  _tx_thread_delete_port_completion(thread_ptr, tx_saved_posture);

void _tx_thread_reset_port_completion(struct TX_THREAD_STRUCT *thread_ptr, UINT tx_saved_posture);
#define TX_THREAD_RESET_PORT_COMPLETION(thread_ptr)   _tx_thread_reset_port_completion(thread_ptr, tx_saved_posture);


/* A thread whose entry function has just returned gets one look at whether its
   Exec Task was abandoned by the reaper, before the generic code touches the
   ready lists on its behalf.  See _tx_amiga_thread_completed().  */

void    _tx_amiga_thread_completed(void);

#define TX_THREAD_COMPLETED_EXTENSION(thread_ptr)     _tx_amiga_thread_completed();


/* In a green build wakeups are delivered at the realm's scheduling points, so a
   relinquishing green thread must deliver latched signals and owed ticks BEFORE
   the generic code inspects the ready lists.  Baton builds keep the stock hook. */

#ifdef AMINETXDUO_GREEN_REALM
void    _tx_amiga_relinquish_prepare(void);
#define TX_THREAD_RELINQUISH_PORT_PREPARE             _tx_amiga_relinquish_prepare();
#endif


/* Start the periodic tick task once the kernel is initialised but before the
   scheduler runs, the exact hook the Linux port uses.  */

void    _tx_amiga_start_interrupts(void);

#define TX_PORT_SPECIFIC_PRE_SCHEDULER_INITIALIZATION   _tx_amiga_start_interrupts();


/* ------------------------------------------------------------------------ */
/* Port tunables and internals.                                              */
/* ------------------------------------------------------------------------ */

/* Bytes handed to tx_application_define() as "first unused memory".  ThreadX
   sub-allocates its own pools out of this.  Override with
   tx_amiga_set_kernel_memory() before tx_kernel_enter().  */

#ifndef TX_AMIGA_MEMORY_SIZE
#define TX_AMIGA_MEMORY_SIZE                    (32UL * 1024UL)
#endif

/* Exec priority for Tasks that ThreadX creates.  ThreadX, not Exec, picks
   which of them runs, so they all share one priority; making it depend on the
   ThreadX priority would put Exec's scheduler in competition with ours.  */

#ifndef TX_AMIGA_TASK_PRIORITY
#define TX_AMIGA_TASK_PRIORITY                  1
#endif

/* Exec priority of a Task parked in _tx_amiga_thread_park(), and only while it is
   parked.  One above the band, because Exec reschedules on a Signal() only for a
   STRICTLY higher priority; still far below TX_AMIGA_TIMER_PRIORITY.  */

#ifndef TX_AMIGA_HANDOFF_PRIORITY
#define TX_AMIGA_HANDOFF_PRIORITY               (TX_AMIGA_TASK_PRIORITY + 1)
#endif

/* Exec priority of the periodic tick Task.  It must out-prioritise every
   ThreadX task so that it really does preempt the baton holder.  */

#ifndef TX_AMIGA_TIMER_PRIORITY
#define TX_AMIGA_TIMER_PRIORITY                 20
#endif

/* ----------------------------------------------------------- the tick ---- */

/* 50 Hz.  NX_IP_PERIODIC_RATE in port/netxduo-amiga/inc/nx_user.h MUST equal this:
   NetX Duo derives every timeout from it, so a disagreement scales every TCP timer
   by the ratio rather than failing.  The wakeup source is not the time base.  */

#ifndef TX_TIMER_TICKS_PER_SECOND
#define TX_TIMER_TICKS_PER_SECOND               (50UL)
#endif

/* timer.device unit for the wakeup source.  UNIT_VBLANK (1) rides an interrupt
   the machine already takes; UNIT_MICROHZ (0) is the fallback the port selects
   at run time if VBlank wakeups turn out not to arrive.  */

#ifndef TX_AMIGA_TIMER_UNIT
#define TX_AMIGA_TIMER_UNIT                     1
#endif

/* Ceiling on the wheel's backlog, in ticks, and the only place a tick is thrown
   away.  A dropped tick is the one thing that makes the wheel skip a slot, hiding
   its timers for up to TX_TIMER_ENTRIES ticks; counted in tx_amiga_tick_lost.  */

#ifndef TX_AMIGA_TIMER_MAX_CATCHUP
#define TX_AMIGA_TIMER_MAX_CATCHUP              8UL
#endif

/* Milliseconds of a tick period the tick task may spend delivering before it gives
   the machine back.  The burst runs under Forbid(), so this bounds how long one
   wakeup stops every other task.  Nothing is lost here, only deferred.  */

#ifndef TX_AMIGA_TIMER_BUDGET_MS
#define TX_AMIGA_TIMER_BUDGET_MS                16UL
#endif

/* Startup validation window for the wakeup source, in milliseconds, and the rate
   band a usable source must fall in.  50 Hz PAL and 60 Hz NTSC both sit inside
   30..120; anything outside it gets us onto UNIT_MICROHZ instead.  */

#ifndef TX_AMIGA_TIMER_PROBE_MS
#define TX_AMIGA_TIMER_PROBE_MS                 250UL
#endif
#ifndef TX_AMIGA_TIMER_PROBE_MIN_HZ
#define TX_AMIGA_TIMER_PROBE_MIN_HZ             30UL
#endif
#ifndef TX_AMIGA_TIMER_PROBE_MAX_HZ
#define TX_AMIGA_TIMER_PROBE_MAX_HZ             120UL
#endif


/* How long tx_amiga_kernel_stop() waits for each of the two Tasks the port
   created to remove itself before declaring the shutdown failed.  */

#ifndef TX_AMIGA_STOP_TIMEOUT_SECS
#define TX_AMIGA_STOP_TIMEOUT_SECS              5UL
#endif


/* Port globals.  Declared with VOID * rather than struct Task * so that this
   header stays independent of the Exec headers.  */

extern VOID    *_tx_amiga_scheduler_task;       /* struct Task *            */
extern ULONG    _tx_amiga_scheduler_signal;
extern VOID    *_tx_amiga_timer_task;           /* struct Task *            */
extern VOID    *_tx_amiga_adopt_task;           /* struct Task *, handshake */
extern ULONG    _tx_amiga_adopt_signal;
extern VOID    *_tx_amiga_kernel_memory;
extern ULONG    _tx_amiga_kernel_memory_size;

/* Poke the scheduler task so it re-evaluates _tx_thread_execute_ptr.  */
VOID    _tx_amiga_wake_scheduler(VOID);

/* Give a thread the baton, or park the caller.  Internal to the port.  */
VOID    _tx_amiga_signal_task(VOID *task, ULONG sigmask);


/* Define the version ID of ThreadX.  */

#ifdef TX_THREAD_INIT
CHAR    _tx_version_id[] =
            "Copyright (c) Microsoft Corporation * Eclipse ThreadX contributors"
            " * ThreadX AmigaOS/m68k (AmiNetXDuo) *";
#else
extern CHAR    _tx_version_id[];
#endif

#endif /* TX_PORT_H */
