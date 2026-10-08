/* Research reserved-worker ThreadX services. SPDX-License-Identifier: MIT */
#ifndef ANX_RESEARCH_EXEC_THREAD_H
#define ANX_RESEARCH_EXEC_THREAD_H
#include "exec_task.h"
enum { ANX_THREAD_EMPTY, ANX_THREAD_PREPARING, ANX_THREAD_PREPARED,
       ANX_THREAD_BOUND, ANX_THREAD_CANCEL, ANX_THREAD_FINISHED, ANX_THREAD_REAPED };
typedef struct AnxExecThread {
    struct Task task;
    AnxTxThread bridge;
    AnxExecWait wait;
    TX_THREAD *thread;
    struct Task *creator;
    struct AnxExecThread *next;
    APTR stack;
    ULONG stack_size,ack;
    BYTE signal;
    volatile unsigned state,prepared,entered;
} AnxExecThread;
/* Zero-init record once. Prepare outside bridge/Forbid/Disable in creator task.
 * Retain record, name, control block and aligned >=8192 stack through cancellation
 * or successful public delete. Live record/control/stack regions cannot overlap.
 * Opens worker-owned IO before public creation;
 * does not read/modify target control block. No implicit unreserved allocation.
 * Public create requires this exact creator/target/stack reservation. Same public
 * pinned tx_api signatures, no vendor edits. All priorities map 31-priority to
 * Exec task priorities; threshold must equal priority and slice must be zero.
 * This is creation/publication evidence, not strict scheduler conformance. */
int anx_exec_thread_prepare(AnxExecThread *, TX_THREAD *, CHAR *, APTR, ULONG);
/* Only unbound PREPARED reservation can be cancelled, outside the boundary.
 * Never touches public control storage; closes private IO and truly reaps Task. */
int anx_exec_thread_cancel(AnxExecThread *);
/* Creator-only outside boundary: wait for normal completion + native removal.
 * Does not release storage/ACK: public tx_thread_delete must then succeed. */
int anx_exec_thread_wait(AnxExecThread *);
/* Public services are defined by exec_thread.c, linked only by native research
 * target. Resume supports only initial DONT_START; general delayed suspend,
 * priorities/threshold changes, nonzero slices and forced terminate remain open.
 * Public delete only after normal completion + native removal, by creator. */
#endif
