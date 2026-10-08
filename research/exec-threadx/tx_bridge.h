/* Research suspension bridge, not a complete scheduler. SPDX-License-Identifier: MIT */
#ifndef ANX_RESEARCH_TX_BRIDGE_H
#define ANX_RESEARCH_TX_BRIDGE_H
#include "tx_api.h"
#include "wait.h"
#include <stdint.h>

typedef struct {
    void (*enter)(void *);
    void (*leave)(void *);
    uintptr_t (*caller)(void *);
    void (*panic)(void *, const char *); /* must not return */
    void *context;
    /* Called with a temporary enter held. Verify these are precisely all
     * task protection levels and interrupts are enabled before an Exec wait. */
    int (*can_pause)(void *, unsigned levels);
} AnxTxPlatform;

typedef struct AnxTxThread {
    TX_THREAD *thread;
    AnxWait *wait;
    uintptr_t owner;
    struct AnxTxThread *next;
    uint32_t token, pending_token;
    unsigned expiry_dispatched, pending_resume, resumes, parks;
    VOID (*cleanup_at_suspend)(TX_THREAD *, ULONG);
    VOID *control_at_suspend;
    ULONG sequence_at_suspend;
    int (*resume_cleanup)(struct AnxTxThread *);
    uint32_t operation;
    unsigned abort_pins;
    TX_SEMAPHORE *semaphore_call; /* retained until an actual blocking get returns */
    int (*abort_policy)(struct AnxTxThread *, UINT *);
    void (*terminal_owner)(void *); /* private terminal path, must not return */
    void *terminal_context;
    unsigned terminal_pending;
    struct AnxTxContext *paused_frame;
    unsigned paused_depth, exec_wait_nesting;
} AnxTxThread;

typedef struct AnxTxContext {
    struct AnxTxContext *previous;
    TX_THREAD *saved_thread;
    ULONG saved_state;
    uintptr_t owner;
} AnxTxContext;

/* Research-only cleanup grace: failure is fatal, never a fabricated status. */
#define ANX_TX_CLEANUP_GRACE_US UINT64_C(1000000)

/* One domain, serialized task-level call boundaries. No real interrupt calls.
 * Bind actual pinned control blocks; retain them and quiesce all producers
 * before detach. Blocking is permitted only in the outer thread context. */
void anx_tx_runtime_init(const AnxTxPlatform *);
/* Mutex/event/semaphore lifetimes are tracked in real created rings. Creation/deletion
 * requires a normal serialized context, never a timer/resume callback. Delete
 * is quiescent only: busy objects return TX_FEATURE_NOT_ENABLED unchanged.
 * Reset with live objects is fatal. Caller must quiesce external producers and
 * retain control-block storage through native owner ACK before reclaiming it.
 * This is not general ThreadX delete-with-waiters or full NetX IP deletion.
 * Semaphore put refuses ULONG_MAX count with TX_CEILING_EXCEEDED (no wrap);
 * notification callbacks are unsupported. The FIFO/count/cleanup bodies are
 * pinned upstream code, with membership and context guards in this bridge. */
/* Native lifecycle calls must block outside every bridge call boundary. */
int anx_tx_runtime_idle(void);
/* Trusted native backend reservations pin the domain even before a binding.
 * Balance each hold/drop; reinitialization with any reservation is fatal. */
void anx_tx_runtime_hold(void);
void anx_tx_runtime_drop(void);
/* Trusted admission preflight runs under protection BEFORE a new context.
 * NULL by default. Installed service must not block or enter a context.
 * Retain its storage until uninstalled; reset while installed is forbidden. */
extern void (*anx_tx_admission_check)(void);
int anx_tx_attach(AnxTxThread *, TX_THREAD *, AnxWait *, uintptr_t);
/* Trusted creator publication: fully prepared owner/wait retained, public
 * control block initialized and SUSPENDED. Requires a serialized boundary;
 * preserves public fields. Owner must remain parked until publication ends. */
int anx_tx_bind_created(AnxTxThread *, TX_THREAD *, AnxWait *, uintptr_t);
int anx_tx_detach(AnxTxThread *);
/* Trusted registry retirement, with external producers already quiesced.
 * Checks membership and concrete wait/object references; permits foreign
 * retirement of a dormant READY binding. Does not dereference owner Task or
 * context frames, free signals, or reclaim an active/paused/blocked binding. */
int anx_tx_forget_dormant(AnxTxThread *);
int anx_tx_quiescent(AnxTxThread *);
int anx_tx_context_is_outer(AnxTxContext *);
/* Trusted registry has positively proved native owner removal. Only actual
 * TX_SLEEP with no queue/object/mutex/abort/Exec-pause references is eligible.
 * Remove its timer and binding, publish TERMINATED/ID0 and complete a pending
 * private wait DELETED. Adapter must suppress notification to the dead owner.
 * No owner stack/frame dereference; no fabricated returned ThreadX status.
 * General removed-owner cleanup remains unsupported. */
int anx_tx_forget_dead_sleep(AnxTxThread *);
/* Owner-only normal completion; same quiescence checks as detach. Removes the
 * runtime binding but retains public ID and publishes TX_COMPLETED for delete.
 * Caller retains storage and closes IO before its native finished publication. */
int anx_tx_complete(AnxTxThread *);
/* Trusted owner installs a private terminal path while READY/quiescent.
 * stop_event requires a normal outer producer boundary and exact blocked event
 * object; no owned mutex, pending wake, abort pin or stale wait generation.
 * It removes real event cleanup/timer/runtime references, retains public ID as
 * TERMINATED, completes the private wait DELETED. Terminal callback is reached
 * outside the boundary before any further public-control dereference. Caller
 * must retain all storage and quiesce all external producers through native ACK.
 * Native integration separately enforces creator authority. Not tx_terminate. */
int anx_tx_set_terminal_owner(AnxTxThread *, void (*)(void *), void *);
int anx_tx_stop_event(AnxTxThread *, TX_EVENT_FLAGS_GROUP *);
/* Owner-only, while READY and quiescent. Optional integration must perform
 * real cleanup before resume, or return zero to fail closed. */
int anx_tx_set_resume_cleanup(AnxTxThread *, int (*)(AnxTxThread *));
int anx_tx_set_abort_policy(AnxTxThread *, int (*)(AnxTxThread *, UINT *));
/* Actual pinned body, renamed by this research project's compiler only. */
UINT anx_tx_original_wait_abort(TX_THREAD *);
void anx_tx_context_begin(AnxTxContext *, TX_THREAD *, ULONG system_state);
void anx_tx_context_end(AnxTxContext *);
/* Owner-only Exec wait bracket, including nested release/acquire pairs.
 * Pause drops the entire NORMAL same-thread context chain and all its task
 * protection levels. Resume restores the exact chain after real Exec IO.
 * Storage/owner must remain live throughout; detach/reset while paused is
 * prohibited. No ThreadX queue, timer, baton or scheduler transition occurs.
 * Outside an admitted context or unmatched resume returns zero unchanged;
 * callers must not perform blocking IO when pause rejects. Platform preflight
 * refuses external Forbid/Disable levels. Paused owners cannot begin/end a
 * bridge context or invoke ThreadX services until the final resume. This is
 * not dead-task reclamation: retained frames may still live on the owner stack. */
int anx_tx_context_pause(void);
int anx_tx_context_resume(void);
/* Research scheduling policy: running owner only threshold changes, restore
 * before outer context_end. Real blocking may drop/reenter a raised-threshold
 * boundary. Slices are stored/advisory, not a ThreadX tick/dispatch guarantee.
 * No foreign threshold changes, priority inheritance or yield implementation. */
int anx_tx_expire(TX_THREAD *, uint32_t token); /* marked timer context required */
/* One explicit application-clock tick from an outer marked task timer context.
 * Native driver must wait outside the boundary. Does not tick private waits.
 * Callback storage retained throughout dispatch; only event set/deactivation
 * supported inside callbacks. No automatic timer task or catch-up policy. */
void anx_tx_timer_tick(void);
/* Research integration guards: fail closed before a raw consumer publishes. */
void anx_tx_require_context(UINT blocking);
void anx_tx_unsupported(const char *reason); /* terminal, never returns */
UINT anx_tx_host_disable(void);
void anx_tx_host_restore(UINT);
/* Deterministic host schedule seam, invoked after a real mutex release.
 * NULL in the native experiment. It must not block. */
extern void (*anx_tx_after_mutex_put)(TX_MUTEX *);
#endif
