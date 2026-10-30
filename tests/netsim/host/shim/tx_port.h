/*
 * AmiNetXDuo, host network simulator: a ThreadX port on a virtual clock.
 *
 * One CPU, infinitely fast.  Every ThreadX thread is a host pthread, but only
 * the one the scheduler hands the CPU to runs; the rest wait on their own
 * condition variable.  Time does not move while any thread is ready.  When
 * none is, the scheduler advances the clock to the next event -- a frame
 * arriving, or the next tick -- and runs it as an interrupt would.  So a run
 * is a pure function of its inputs, takes as long as the host needs to
 * execute the code, and never waits for a wall clock.
 *
 * The type widths are the m68k's, as tests/perf/host/shim's are: LONG and
 * ULONG are 32 bits, pointers are the host's, ALIGN_TYPE holds one.  The
 * options are the shipping port's (port/threadx-amiga/inc/tx_port.h): timers
 * expire in the tick interrupt, notify callbacks off, no stack filling, 50
 * ticks a second.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef TX_PORT_H
#define TX_PORT_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#ifdef TX_INCLUDE_USER_DEFINE_FILE
#include "tx_user.h"
#endif

typedef void                                    VOID;
typedef char                                    CHAR;
typedef unsigned char                           UCHAR;
typedef int                                     INT;
typedef unsigned int                            UINT;
typedef int                                     LONG;
typedef unsigned int                            ULONG;
typedef short                                   SHORT;
typedef unsigned short                          USHORT;
typedef uint64_t                                ULONG64;
#define ULONG64_DEFINED

#define ALIGN_TYPE_DEFINED
typedef uintptr_t                               ALIGN_TYPE;
#if UINTPTR_MAX > 0xFFFFFFFFUL
#define TX_BYTE_BLOCK_FREE                      ((ALIGN_TYPE) 0xFFFFEEEEFFFFEEEEULL)
#endif

#define TX_MAX_PRIORITIES                       32
#define TX_MINIMUM_STACK                        200
#define TX_TIMER_TICKS_PER_SECOND               (50UL)

/* As the shipping port.  */
#define TX_TIMER_PROCESS_IN_ISR
#define TX_DISABLE_STACK_FILLING
#define TX_DISABLE_NOTIFY_CALLBACKS

#ifndef TX_TIMER_THREAD_STACK_SIZE
#define TX_TIMER_THREAD_STACK_SIZE              1024
#endif
#ifndef TX_TIMER_THREAD_PRIORITY
#define TX_TIMER_THREAD_PRIORITY                0
#endif

#define TX_INT_DISABLE                          1
#define TX_INT_ENABLE                           0

#define TX_TRACE_TIME_SOURCE                    0
#define TX_TRACE_TIME_MASK                      0xFFFFFFFFUL
#define TX_PORT_SPECIFIC_BUILD_OPTIONS          0
#define TX_INLINE_INITIALIZATION

/* Nothing can interrupt a thread: interrupts run only from the scheduler,
   while every thread is parked.  So the lockout is a posture and no more.  */
UINT   _tx_thread_interrupt_control(UINT new_posture);

#define TX_INTERRUPT_SAVE_AREA                  UINT tx_saved_posture;
#define TX_DISABLE                              tx_saved_posture = TX_INT_DISABLE;
#define TX_RESTORE                              (VOID) tx_saved_posture;

#define TX_BLOCK_POOL_DISABLE                   TX_DISABLE
#define TX_BYTE_POOL_DISABLE                    TX_DISABLE
#define TX_EVENT_FLAGS_GROUP_DISABLE            TX_DISABLE
#define TX_MUTEX_DISABLE                        TX_DISABLE
#define TX_QUEUE_DISABLE                        TX_DISABLE
#define TX_SEMAPHORE_DISABLE                    TX_DISABLE

/* The host thread behind each ThreadX thread, and what it waits on.  */
#define TX_THREAD_EXTENSION_0                   pthread_t       tx_thread_sim_host; \
                                                pthread_cond_t  tx_thread_sim_run;  \
                                                UINT            tx_thread_sim_live;
#define TX_THREAD_EXTENSION_1                   VOID           *tx_thread_extension_ptr;
#define TX_THREAD_EXTENSION_2
#define TX_THREAD_EXTENSION_3

/* A pointer does not fit the ULONG a thread entry or a timer is handed, so
   on a 64-bit host the object carries it, as ThreadX's 64-bit ports do. */
#if UINTPTR_MAX > 0xFFFFFFFFUL
#define TX_64_BIT
#define TX_TIMER_INTERNAL_EXTENSION             VOID           *tx_timer_internal_extension_ptr;
#define TX_THREAD_CREATE_TIMEOUT_SETUP(t)       (t) -> tx_thread_timer.tx_timer_internal_timeout_function = &(_tx_thread_timeout); \
                                                (t) -> tx_thread_timer.tx_timer_internal_timeout_param    = 0;                    \
                                                (t) -> tx_thread_timer.tx_timer_internal_extension_ptr    = (VOID *) (t);
#define TX_THREAD_TIMEOUT_POINTER_SETUP(t)      (t) = (TX_THREAD *) _tx_timer_expired_timer_ptr -> tx_timer_internal_extension_ptr;

/* NetX Duo takes the same from its user header (test/cmake/netxduo64);
   the shipping nx_user.h has no reason to carry them. */
#define NX_THREAD_EXTENSION_PTR_SET(a, b)       TX_THREAD_EXTENSION_PTR_SET(a, b)
#define NX_THREAD_EXTENSION_PTR_GET(a, b, c)    TX_THREAD_EXTENSION_PTR_GET(a, b, c)
#define NX_TIMER_EXTENSION_PTR_SET(a, b)        TX_TIMER_EXTENSION_PTR_SET(a, b)
#define NX_TIMER_EXTENSION_PTR_GET(a, b, c)     TX_TIMER_EXTENSION_PTR_GET(a, b, c)
#endif

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
#define TX_THREAD_COMPLETED_EXTENSION(thread_ptr)
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
#define TX_PORT_SPECIFIC_PRE_SCHEDULER_INITIALIZATION
#define TX_TRACE_PORT_EXTENSION
#define TX_SAFETY_CRITICAL_EXCEPTION_HANDLER

/* A deleted or reset thread's host thread is told to exit and joined, so
   the control block can be created again. */
struct TX_THREAD_STRUCT;
VOID   _tx_sim_thread_retire(struct TX_THREAD_STRUCT *thread_ptr);
#define TX_THREAD_DELETE_PORT_COMPLETION(thread_ptr)    _tx_sim_thread_retire(thread_ptr);
#define TX_THREAD_RESET_PORT_COMPLETION(thread_ptr)     _tx_sim_thread_retire(thread_ptr);

#define TX_THREAD_STACK_BUILD_STATUS(thread_ptr) \
    (((thread_ptr) -> tx_thread_sim_live != 0) ? TX_SUCCESS : TX_NO_MEMORY)

#ifdef TX_THREAD_INIT
CHAR                            _tx_version_id[] =
                                    "ThreadX host simulator, virtual clock";
#else
extern  CHAR                    _tx_version_id[];
#endif

#endif /* TX_PORT_H */
