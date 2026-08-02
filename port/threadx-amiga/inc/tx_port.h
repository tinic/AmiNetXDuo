/***************************************************************************
 * Eclipse ThreadX -- AmigaOS / m68k port.
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

/**************************************************************************/
/*                                                                        */
/*  PORT SPECIFIC C INFORMATION                                           */
/*                                                                        */
/*    tx_port.h                                        AmigaOS/m68k       */
/*                                                                        */
/*  DESCRIPTION                                                           */
/*                                                                        */
/*    ThreadX runs on top of Exec as a "hosted" port, in the same shape    */
/*    as the Linux and Win32 ports: every TX_THREAD is backed by a real    */
/*    Exec Task, and a single baton ("core lock") means exactly one        */
/*    ThreadX thread executes at a time.  See docs/RESEARCH.md 6.2.        */
/*                                                                        */
/*    Critical sections map to Forbid()/Permit(), not Disable()/Enable().  */
/*    Nothing in ThreadX or NetX Duo runs at Exec interrupt level here --  */
/*    the SANA-II reader is a Task, packet arrival is an IORequest         */
/*    completion, and the periodic tick is a Task.  Forbid() nests, which  */
/*    matches the save/restore posture idiom, and Exec preserves the       */
/*    per-task nesting count across Wait(), which is what makes it legal   */
/*    for a thread to block inside a TX_DISABLE region (see                */
/*    tx_thread_system_return.c).                                          */
/*                                                                        */
/*    This header must not include any AmigaOS header.  It is pulled into  */
/*    all 185 ThreadX core files and all 511 NetX Duo core files, and      */
/*    <exec/types.h> redefines VOID/ULONG/SHORT/USHORT in ways that only   */
/*    work when ThreadX's typedefs come first.  Port sources include       */
/*    "tx_api.h" and only then the Exec headers.                           */
/*                                                                        */
/**************************************************************************/

#ifndef TX_PORT_H
#define TX_PORT_H


/* Determine if the optional ThreadX user define file should be used.  */

#ifdef TX_INCLUDE_USER_DEFINE_FILE
#include "tx_user.h"
#endif


/* Define compiler library include files.  No <stdio.h>: a shared library build
   must not drag newlib's stdio in.  The core needs memset/memcpy/memcmp
   only.  */

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


/* The system timer thread.  Timer expiration is processed on a real ThreadX
   thread (TX_TIMER_PROCESS_IN_ISR is not defined), so application timer
   callbacks -- including NetX Duo's periodic handlers -- run at thread level
   where they may take mutexes.  */

#ifndef TX_TIMER_THREAD_STACK_SIZE
#define TX_TIMER_THREAD_STACK_SIZE              4096
#endif

#ifndef TX_TIMER_THREAD_PRIORITY
#define TX_TIMER_THREAD_PRIORITY                0
#endif


/* Do not pre-fill thread stacks.  (1) Adopted threads (tx_amiga_adopt_thread)
   give ThreadX the bounds of an Exec Task stack that is already live, so
   filling it would destroy the caller's own frames; (2) a byte-wise fill of
   every stack is expensive on a 14 MHz 68020.  Stack checking
   (TX_ENABLE_STACK_CHECKING) is consequently unavailable.  */

#define TX_DISABLE_STACK_FILLING


/* No notify callbacks.  _tx_thread_terminate() calls tx_thread_entry_exit_notify
   from inside its TX_DISABLE, which here is a Forbid(), and
   tx_amiga_discard_thread()/tx_amiga_orphan_thread() hold a Forbid() of their
   own across the terminate with _tx_thread_system_state raised.  A registered
   callback that blocked would break both at once.  Nothing in NetX Duo or this
   stack registers one, so the call sites go rather than the hazard staying
   latent.  docs/FORBID.md.  */

#define TX_DISABLE_NOTIFY_CALLBACKS


/* Define various constants for the port.  */

#define TX_INT_DISABLE                          1           /* Forbid()  */
#define TX_INT_ENABLE                           0           /* Permit()  */


/* Trace time source.  No free-running counter is assumed; the port could use
   the E-Clock here, but reading it costs a timer.device call.  */

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
/* Interrupt (critical section) control.                                     */
/* ------------------------------------------------------------------------ */

/* TX_DISABLE/TX_RESTORE are strictly balanced throughout the ThreadX core,
   so they map onto the Forbid()/Permit() nest counter directly and the saved
   posture is informational only.  These are out-of-line calls rather than
   macros so that this header stays free of AmigaOS includes.  */

UINT   _tx_thread_interrupt_disable(void);
VOID   _tx_thread_interrupt_restore(UINT previous_posture);

#define TX_INTERRUPT_SAVE_AREA                  UINT tx_saved_posture;
#define TX_DISABLE                              tx_saved_posture = _tx_thread_interrupt_disable();
#define TX_RESTORE                              _tx_thread_interrupt_restore(tx_saved_posture);


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

/* tx_thread_amiga_task            struct Task * backing this TX_THREAD
   tx_thread_amiga_run_signal      Exec signal mask the scheduler pokes to give
                                   this thread the baton
   tx_thread_amiga_suspension_type 0 = solicited (parked on the run signal).
                                   Reserved for a future asynchronous
                                   suspension path; this port only ever
                                   suspends threads at their own request.
   tx_thread_amiga_flags           TX_AMIGA_THREAD_* bits below
   tx_thread_amiga_signal_owner    struct Task * that AllocSignal()d the run
                                   signal, for adopted threads only

   tx_thread_amiga_signal_owner is not a duplicate of tx_thread_amiga_task.
   _tx_amiga_reap() clears tx_thread_amiga_task on teardown, deliberately, so
   the scheduler cannot poke a Task that is no longer a thread -- but the run
   signal outlives the TX_THREAD and only the Task that allocated it may call
   FreeSignal() on it.  Losing that record is losing the bit: there are 32 per
   Task and no way to recover one.  Cleared by whoever frees the signal.

   The teardown handshake does not live here: it is in the Exec Task's own
   control block (struct _tx_amiga_ctrl, tx_amiga_internal.h), so a task the
   reaper had to give up on can still destroy itself after the TX_THREAD has
   been deleted and its storage reused.                                      */

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


/* Object create/delete extensions -- all white space for this port.  */

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
#define TX_THREAD_TERMINATED_EXTENSION(thread_ptr)
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


/* Start the periodic tick task once the kernel is initialised but before the
   scheduler runs -- the exact hook the Linux port uses.  */

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

/* Exec priority for Tasks that ThreadX creates.  ThreadX -- not Exec -- picks
   which of them runs, so they all share one priority; making it depend on the
   ThreadX priority would put Exec's scheduler in competition with ours.  */

#ifndef TX_AMIGA_TASK_PRIORITY
#define TX_AMIGA_TASK_PRIORITY                  1
#endif

/* Exec priority of the periodic tick Task.  It must out-prioritise every
   ThreadX task so that it really does preempt the baton holder.  */

#ifndef TX_AMIGA_TIMER_PRIORITY
#define TX_AMIGA_TIMER_PRIORITY                 20
#endif

/* ----------------------------------------------------------- the tick ---- */

/*
 * 50 Hz.  The wakeup source is not the time base.
 *
 * The AmiTCP-derived stacks have run a 50 Hz computational clock since 1993
 * (`#define hz (50)`), and 4.4BSD's protocol timers -- what a TCP stack
 * consumes ticks for -- are hz/2 (500 ms) and hz/5 (200 ms).  20 ms of
 * granularity is an order of magnitude finer than anything above us asks for,
 * at half the wakeups the previous 100 Hz cost.
 *
 * NX_IP_PERIODIC_RATE in port/netxduo-amiga/inc/nx_user.h must equal this.
 * NetX Duo derives every one of its own timeouts from it, so a disagreement
 * scales every TCP timer by the ratio rather than failing loudly.
 *
 * The tick task wakes on timer.device UNIT_VBLANK but does not count those
 * wakeups: it reads ReadEClock() and works out how many 20 ms periods have
 * elapsed.  VBlank is 50 Hz PAL and 60 Hz NTSC, and under RTG
 * (Picasso96/CyberGraphX), on PiStorm/Emu68 and inside emulators its
 * relationship to real time cannot be relied on.  The E-Clock is CIA-derived,
 * independent of the display, and reports its own frequency, so it is correct
 * on all of them.  A wakeup that is late or coalesced delivers the arrears; one
 * that is early delivers nothing.
 */

#ifndef TX_TIMER_TICKS_PER_SECOND
#define TX_TIMER_TICKS_PER_SECOND               (50UL)
#endif

/* timer.device unit for the wakeup source.  UNIT_VBLANK (1) rides the vertical
   blank interrupt the machine is already taking, so a request costs a list
   insertion rather than a CIA timer reload; UNIT_MICROHZ (0) is the fallback
   the port selects at run time if VBlank wakeups turn out not to arrive.  */

#ifndef TX_AMIGA_TIMER_UNIT
#define TX_AMIGA_TIMER_UNIT                     1
#endif

/* Wakeup rate of a UNIT_MICROHZ source, in Hz.  Only the tick rate by default,
   which is one request per tick period.  Raising it above the tick rate wakes
   more often than there are ticks to deliver, so a tick lands nearer its true
   time instead of at the next frame; the extra wakeups measure zero elapsed
   periods and count in tx_amiga_tick_empty.  No effect on UNIT_VBLANK, which
   takes the frame it is given.  */

#ifndef TX_AMIGA_TIMER_WAKEUP_HZ
#define TX_AMIGA_TIMER_WAKEUP_HZ                TX_TIMER_TICKS_PER_SECOND
#endif

/* Ceiling on the wheel's backlog, in ticks, and the only place a tick is thrown
   away.  A Forbid()-heavy section, a disk access or an emulator host hiccup can
   stall the tick task for a long time; without a cap the catch-up would fire
   thousands of timer callbacks back to back with the core lock held, which is
   worse than the lateness.  8 ticks is 160 ms -- more than any stall the port
   causes itself, and still below BSD's 200 ms fast timer.

   Ticks past the cap are dropped, which is the one thing that makes the wheel
   skip a slot, hiding the timers in it until the pointer comes round again (up
   to TX_TIMER_ENTRIES ticks).  It exists because the budget below defers rather
   than drops, and deferral alone would let a machine that never catches up grow
   an unbounded backlog.  This is the pathological case; the budget is the
   ordinary one.  Counted in tx_amiga_tick_lost.  */

#ifndef TX_AMIGA_TIMER_MAX_CATCHUP
#define TX_AMIGA_TIMER_MAX_CATCHUP              8UL
#endif

/* Milliseconds of a tick period the tick task may spend delivering, before it
   gives the machine back and finishes the backlog at the next wakeup.  The
   burst runs under Forbid(), so this is the ceiling on how long one wakeup can
   stop every other task in the machine.  Four fifths of 20 ms for the tick
   leaves 4 ms for everyone else, and keeps the hold inside the ~27 ms an
   A2065's 32 KB ring holds at 10 Mbit -- the real deadline, since a hold longer
   than that costs packets on hardware whatever the tick does.

   Nothing is lost here: the wheel is walked a slot at a time and gets every one
   of the deferred ticks, late.  tx_amiga_tick_over_budget counts the yields and
   tx_amiga_tick_deferred the ticks put off.  */

#ifndef TX_AMIGA_TIMER_BUDGET_MS
#define TX_AMIGA_TIMER_BUDGET_MS                16UL
#endif

/* Startup validation window for the wakeup source, in milliseconds, and the
   rate band a usable source must fall in.  50 Hz PAL and 60 Hz NTSC both sit
   inside 30..120; a source that answers instantly (a spin) or not at all
   fails and gets us onto UNIT_MICROHZ instead.  */

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
   created to remove itself before declaring the shutdown failed.  Both are
   parked on a signal and answer within milliseconds; the only way to spend
   this budget is a Task that cannot be woken at all, and then the answer is
   "not safe to exit" however long we wait.  */

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
