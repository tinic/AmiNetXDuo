/* Research reserved-worker ThreadX services. SPDX-License-Identifier: MIT */
#ifndef ANX_RESEARCH_EXEC_THREAD_H
#define ANX_RESEARCH_EXEC_THREAD_H
#include "exec_task.h"
#define ANX_THREAD_EXEC_PRIORITY(p) ((BYTE)(TX_AMIGA_TASK_PRIORITY-((p)/16)))
enum { ANX_THREAD_EMPTY, ANX_THREAD_PREPARING, ANX_THREAD_PREPARED,
       ANX_THREAD_BOUND, ANX_THREAD_CANCEL, ANX_THREAD_FINISHED, ANX_THREAD_REAPED,
       ANX_THREAD_STOPPING, ANX_THREAD_RETIRE_PENDING };
typedef struct AnxExecThread {
    struct Task task;
    AnxTxThread bridge;
    AnxExecWait wait;
    TX_THREAD *thread;
    struct Task *creator;
    struct AnxExecThread *next;
    APTR stack;
    ULONG stack_size,ack;
    APTR native_stack,native_allocation;
    ULONG native_size,native_allocation_size,client_stamp;
    struct Task *client;
    unsigned managed;
    BYTE signal;
    volatile unsigned state,prepared,entered;
} AnxExecThread;
/* Zero-init record once. Prepare outside bridge/Forbid/Disable in creator task.
 * Retain record, name, control block and aligned >=8192 stack through cancellation
 * or successful public delete. Live record/control/stack regions cannot overlap.
 * Opens worker-owned IO before public creation;
 * does not read/modify target control block. This legacy prepare API performs no
 * allocation of a record or stack. Public create consumes the exact client/
 * target/stack reservation; with a running kernel an ordinary unreserved create
 * obtains a manager-owned reservation first. Same public
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
 * target. Resume supports initial DONT_START and retained explicit self-suspend;
 * foreign READY/blocked suspension and general delayed suspend,
 * priority changes and forced terminate remain open. Current-owner threshold
 * changes require restore before outer boundary exit; no foreign changes.
 * Public delete only after completion + native removal. Managed bound handles
 * belong to the application/library storage lifetime; any registered normal
 * ThreadX caller except the target can stop/delete them. PREPARED reservations
 * and legacy records retain their original client/ACK authority.
 * Managed finished deletion is nonblocking; the manager later frees only private
 * storage, while the public control and supplied stack are immediately reusable. */
UINT anx_exec_thread_stack_in_use(const VOID *,ULONG);
/* Native yield preflight; minimum Exec priority refuses before frame release.
 * Public void relinquish treats a refused preflight as a fatal unsupported call. */
UINT anx_exec_thread_relinquish(VOID);
/* Research manager-owned reservations. Normal context, exact caller/target/
 * name/public stack. Reserve BEFORE an upstream constructor raises threshold;
 * create consumes it without waiting. Unreserve cancels only an unused record.
 * Inputs remain caller-owned through public deletion/cancellation. */
UINT anx_exec_thread_reserve(TX_THREAD *,CHAR *,APTR,ULONG);
UINT anx_exec_thread_unreserve(TX_THREAD *);
typedef struct {
    struct Task *task,*manager,*client;
    APTR public_stack,native_stack;
    ULONG public_size,native_size,owned_bytes;
    unsigned state,entered,io_opened;
} AnxManagedSnapshot;
int anx_exec_thread_managed_snapshot(TX_THREAD *,AnxManagedSnapshot *);
void anx_exec_thread_managed_resources(ULONG *records,ULONG *bytes);
/* Private stable-manager hooks; NULL without a running kernel. */
extern UINT (*anx_exec_managed_prepare)(TX_THREAD *,CHAR *,APTR,ULONG);
extern UINT (*anx_exec_managed_retire)(TX_THREAD *,unsigned cancel);
extern void (*anx_exec_managed_notify)(void);
UINT anx_exec_thread_manage_prepare(struct Task *,TX_THREAD *,CHAR *,APTR,ULONG);
UINT anx_exec_thread_manage_retire(struct Task *,TX_THREAD *,unsigned);
void anx_exec_thread_manage_drain(void);
int anx_exec_thread_managed_stop_event(TX_THREAD *,TX_EVENT_FLAGS_GROUP *);
#endif
