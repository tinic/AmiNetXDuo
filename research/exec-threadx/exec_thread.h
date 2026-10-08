/* Research reserved-worker ThreadX services. SPDX-License-Identifier: MIT */
#ifndef ANX_RESEARCH_EXEC_THREAD_H
#define ANX_RESEARCH_EXEC_THREAD_H
#include "exec_task.h"
#define ANX_THREAD_EXEC_PRIORITY(p) ((BYTE)(TX_AMIGA_TASK_PRIORITY-((p)/16)))
enum { ANX_THREAD_EMPTY, ANX_THREAD_PREPARING, ANX_THREAD_PREPARED,
       ANX_THREAD_BOUND, ANX_THREAD_CANCEL, ANX_THREAD_FINISHED, ANX_THREAD_REAPED,
       ANX_THREAD_STOPPING };
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
 * pinned tx_api signatures, no vendor edits. Priorities 0..15 map to the pinned
 * port's Exec priority 1; 16..31 map to 0. Logical priorities remain in public
 * fields; ties are deliberate. Initial threshold must equal priority. Slices
 * are stored/advisory only; no tick accounting or global Exec quantum changes.
 * Entry runs in an automatic outer bridge context. Use matched
 * anx_tx_context_pause/resume for blocking Exec IO, retaining the record,
 * public control and all context frames through return. This is
 * creation/publication evidence, not strict scheduler conformance. */
int anx_exec_thread_prepare(AnxExecThread *, TX_THREAD *, CHAR *, APTR, ULONG);
/* Only unbound PREPARED reservation can be cancelled, outside the boundary.
 * Never touches public control storage; closes private IO and truly reaps Task. */
int anx_exec_thread_cancel(AnxExecThread *);
/* Creator-only outside boundary: wait for normal completion + native removal.
 * Does not release storage/ACK: public tx_thread_delete must then succeed. */
int anx_exec_thread_wait(AnxExecThread *);
/* Nonblocking creator command INSIDE a normal outer serialized boundary.
 * Only exact parked event/no mutex/wake/pin is eligible. Public ID remains
 * TERMINATED until actual native FINISHED then ordinary public delete. Wait
 * for ACK outside every boundary. Public tx_thread_terminate only acknowledges
 * an already native-FINISHED stopped target; it cannot stop a live worker. */
int anx_exec_thread_stop_event(AnxExecThread *, TX_EVENT_FLAGS_GROUP *);
/* Read-only current registered BOUND worker, normal serialized task context.
 * NULL for attached callers, marked callbacks or a mismatched native task.
 * Does not transfer record ownership; caller retains the backend reservation. */
const AnxExecThread *anx_exec_thread_owner_record(void);
/* Public services are defined by exec_thread.c, linked only by native research
 * target. Resume supports only initial DONT_START; general delayed suspend,
 * priority changes and forced terminate remain open. Current-owner threshold
 * changes require restore before outer boundary exit; no foreign changes.
 * Public delete only after normal completion + native removal, by creator. */
UINT anx_exec_thread_stack_in_use(const VOID *,ULONG);
#endif
