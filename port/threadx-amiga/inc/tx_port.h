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
/*    Exec Task, and a single baton ("core lock") guarantees that exactly  */
/*    one ThreadX thread executes at a time.  See docs/RESEARCH.md 6.2.    */
/*                                                                        */
/*    Critical sections map to Forbid()/Permit(), NOT Disable()/Enable().  */
/*    Nothing in ThreadX or NetX Duo runs at Exec interrupt level in this  */
/*    design -- the SANA-II reader is a Task, packet arrival is an         */
/*    IORequest completion, and the periodic tick is a Task.  Forbid()     */
/*    nests, which matches the save/restore posture idiom exactly, and     */
/*    Exec preserves the per-task nesting count across Wait(), which is    */
/*    what makes it legal for a thread to block inside a TX_DISABLE        */
/*    region (see tx_thread_system_return.c).                              */
/*                                                                        */
/*    IMPORTANT: this header must not include any AmigaOS header.  It is   */
/*    pulled into all 185 ThreadX core files and all 511 NetX Duo core     */
/*    files, and <exec/types.h> redefines VOID/ULONG/SHORT/USHORT in ways  */
/*    that only work when ThreadX's typedefs come first.  Port sources     */
/*    include "tx_api.h" and only then the Exec headers.                   */
/*                                                                        */
/**************************************************************************/

#ifndef TX_PORT_H
#define TX_PORT_H


/* Determine if the optional ThreadX user define file should be used.  */

#ifdef TX_INCLUDE_USER_DEFINE_FILE
#include "tx_user.h"
#endif


/* Define compiler library include files.  Deliberately minimal: no <stdio.h>,
   because a shared library build must not drag newlib's stdio in.  The core
   needs memset/memcpy/memcmp only.  */

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


/* Minimum stack for a ThreadX thread.  On AmigaOS the ThreadX stack IS the
   Exec Task stack (see tx_thread_stack_build.c), so this is a real limit and
   68k frames are not small.  */

#ifndef TX_MINIMUM_STACK
#define TX_MINIMUM_STACK                        1024
#endif


/* The system timer thread.  Timer expiration is processed on a real ThreadX
   thread (TX_TIMER_PROCESS_IN_ISR is NOT defined), so application timer
   callbacks -- including NetX Duo's periodic handlers -- run at thread level
   where they may take mutexes.  */

#ifndef TX_TIMER_THREAD_STACK_SIZE
#define TX_TIMER_THREAD_STACK_SIZE              4096
#endif

#ifndef TX_TIMER_THREAD_PRIORITY
#define TX_TIMER_THREAD_PRIORITY                0
#endif


/* Do not pre-fill thread stacks.  Two reasons: (1) adopted threads
   (tx_amiga_adopt_thread) hand ThreadX the bounds of an Exec Task stack that
   is already live -- filling it would destroy the caller's own frames; and
   (2) a byte-wise fill of every stack is expensive on a 14 MHz 68020.
   Stack checking (TX_ENABLE_STACK_CHECKING) is consequently unavailable.  */

#define TX_DISABLE_STACK_FILLING


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
   tx_thread_amiga_run_signal      Exec signal mask the scheduler pokes to
                                   hand this thread the baton
   tx_thread_amiga_suspension_type 0 = solicited (parked on the run signal).
                                   Reserved for a future asynchronous
                                   suspension path; this port only ever
                                   suspends threads at their own request.
   tx_thread_amiga_flags           TX_AMIGA_THREAD_* bits below

   The teardown handshake deliberately does NOT live here: it is in the Exec
   Task's own control block (struct _tx_amiga_ctrl, tx_amiga_internal.h), so
   that a task the reaper had to give up on can still destroy itself after the
   TX_THREAD has been deleted and its storage reused.                        */

#define TX_THREAD_EXTENSION_0                   VOID  *tx_thread_amiga_task; \
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

/* timer.device unit.  UNIT_MICROHZ (0) is required for the 100 Hz default;
   UNIT_VBLANK (1) cannot resolve finer than a display frame.  */

#ifndef TX_AMIGA_TIMER_UNIT
#define TX_AMIGA_TIMER_UNIT                     0
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

/* Hand a thread the baton, or park the caller.  Internal to the port.  */
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
