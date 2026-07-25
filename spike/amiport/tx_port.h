/* Minimal experimental AmigaOS/m68k tx_port.h -- compile-feasibility spike only.
   Not a working port: TX_DISABLE/TX_RESTORE here are placeholders. */

#ifndef TX_PORT_H
#define TX_PORT_H

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef void            VOID;
typedef char            CHAR;
typedef unsigned char   UCHAR;
typedef int             INT;
typedef unsigned int    UINT;
typedef long            LONG;
typedef unsigned long   ULONG;
typedef short           SHORT;
typedef unsigned short  USHORT;
typedef uint64_t        ULONG64;
#define ULONG64_DEFINED

#define TX_MAX_PRIORITIES               32
#define TX_MINIMUM_STACK                1024
#define TX_TIMER_THREAD_STACK_SIZE      2048
#define TX_TIMER_THREAD_PRIORITY        0

#define TX_INT_DISABLE                  1
#define TX_INT_ENABLE                   0

#define TX_TRACE_TIME_SOURCE            0
#define TX_TRACE_TIME_MASK              0xFFFFFFFFUL

#define TX_PORT_SPECIFIC_BUILD_OPTIONS  0

/* Placeholder critical section: real port would use exec Disable()/Enable()
   or Forbid()/Permit() depending on the scheduling model. */
UINT _tx_amiga_int_disable(void);
VOID _tx_amiga_int_restore(UINT);
#define TX_INTERRUPT_SAVE_AREA          UINT tx_saved_posture;
#define TX_DISABLE                      tx_saved_posture = _tx_amiga_int_disable();
#define TX_RESTORE                      _tx_amiga_int_restore(tx_saved_posture);

/* Exec-task backing store for a ThreadX thread. */
#define TX_THREAD_EXTENSION_0           VOID *tx_thread_amiga_task; \
                                        ULONG tx_thread_amiga_run_sigmask; \
                                        UINT  tx_thread_amiga_suspension_type;
#define TX_THREAD_EXTENSION_1           VOID *tx_thread_extension_ptr;
#define TX_THREAD_EXTENSION_2
#define TX_THREAD_EXTENSION_3

#define TX_BLOCK_POOL_EXTENSION
#define TX_BYTE_POOL_EXTENSION
#define TX_EVENT_FLAGS_GROUP_EXTENSION
#define TX_MUTEX_EXTENSION
#define TX_QUEUE_EXTENSION
#define TX_SEMAPHORE_EXTENSION
#define TX_TIMER_EXTENSION

#define TX_INLINE_INITIALIZATION

#ifdef TX_THREAD_INIT
CHAR _tx_version_id[] = "Eclipse ThreadX AmigaOS/m68k experimental port";
#else
extern CHAR _tx_version_id[];
#endif


#define TX_THREAD_USER_EXTENSION
#define TX_BLOCK_POOL_CREATE_EXTENSION(a)
#define TX_BLOCK_POOL_DELETE_EXTENSION(a)
#define TX_BYTE_POOL_CREATE_EXTENSION(a)
#define TX_BYTE_POOL_DELETE_EXTENSION(a)
#define TX_EVENT_FLAGS_GROUP_CREATE_EXTENSION(a)
#define TX_EVENT_FLAGS_GROUP_DELETE_EXTENSION(a)
#define TX_MUTEX_CREATE_EXTENSION(a)
#define TX_MUTEX_DELETE_EXTENSION(a)
#define TX_QUEUE_CREATE_EXTENSION(a)
#define TX_QUEUE_DELETE_EXTENSION(a)
#define TX_SEMAPHORE_CREATE_EXTENSION(a)
#define TX_SEMAPHORE_DELETE_EXTENSION(a)
#define TX_THREAD_CREATE_EXTENSION(a)
#define TX_THREAD_DELETE_EXTENSION(a)
#define TX_TIMER_CREATE_EXTENSION(a)
#define TX_TIMER_DELETE_EXTENSION(a)
#define TX_THREAD_COMPLETED_EXTENSION(a)
#define TX_THREAD_TERMINATED_EXTENSION(a)
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

#endif
