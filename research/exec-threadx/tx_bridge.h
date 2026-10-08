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
    int (*abort_policy)(struct AnxTxThread *, UINT *);
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
int anx_tx_attach(AnxTxThread *, TX_THREAD *, AnxWait *, uintptr_t);
int anx_tx_detach(AnxTxThread *);
/* Owner-only, while READY and quiescent. Optional integration must perform
 * real cleanup before resume, or return zero to fail closed. */
int anx_tx_set_resume_cleanup(AnxTxThread *, int (*)(AnxTxThread *));
int anx_tx_set_abort_policy(AnxTxThread *, int (*)(AnxTxThread *, UINT *));
/* Actual pinned body, renamed by this research project's compiler only. */
UINT anx_tx_original_wait_abort(TX_THREAD *);
void anx_tx_context_begin(AnxTxContext *, TX_THREAD *, ULONG system_state);
void anx_tx_context_end(AnxTxContext *);
int anx_tx_expire(TX_THREAD *, uint32_t token); /* marked timer context required */
/* One explicit application-clock tick from an outer marked task timer context.
 * Native driver must wait outside the boundary. Does not tick private waits.
 * Callback storage retained throughout dispatch; only event set/deactivation
 * supported inside callbacks. No automatic timer task or catch-up policy. */
void anx_tx_timer_tick(void);
UINT anx_tx_host_disable(void);
void anx_tx_host_restore(UINT);
/* Deterministic host schedule seam, invoked after a real mutex release.
 * NULL in the native experiment. It must not block. */
extern void (*anx_tx_after_mutex_put)(TX_MUTEX *);
#endif
