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
    uint32_t token;
    unsigned expiry_dispatched, resumes, parks;
} AnxTxThread;

typedef struct AnxTxContext {
    struct AnxTxContext *previous;
    TX_THREAD *saved_thread;
    ULONG saved_state;
    uintptr_t owner;
} AnxTxContext;

/* One domain, serialized task-level call boundaries. No real interrupt calls.
 * Bind actual pinned control blocks; retain them and quiesce all producers
 * before detach. Blocking is permitted only in the outer thread context. */
void anx_tx_runtime_init(const AnxTxPlatform *);
int anx_tx_attach(AnxTxThread *, TX_THREAD *, AnxWait *, uintptr_t);
int anx_tx_detach(AnxTxThread *);
void anx_tx_context_begin(AnxTxContext *, TX_THREAD *, ULONG system_state);
void anx_tx_context_end(AnxTxContext *);
int anx_tx_expire(TX_THREAD *, uint32_t token); /* marked timer context required */
UINT anx_tx_host_disable(void);
void anx_tx_host_restore(UINT);
/* Deterministic host schedule seam, invoked after a real mutex release.
 * NULL in the native experiment. It must not block. */
extern void (*anx_tx_after_mutex_put)(TX_MUTEX *);
#endif
