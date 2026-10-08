/* Backend-owned caller registry, research only. SPDX-License-Identifier: MIT */
#ifndef ANX_EXEC_CALLER_H
#define ANX_EXEC_CALLER_H
#include "exec_clock.h"
#include "tx_amiga.h"
#define ANX_CALLER_SLOTS 16
#define ANX_CALLER_LEASES 64
#define ANX_CALLER_WAITERS 32
/* One retained backend clock; one signal per admitted Task, no caller timer
 * request/reply port or new stack. Public adoption API in tx_amiga.h binds to
 * backend-owned controls/frames. No vendor/public structure changes.
 * Dormant cached slots may be evicted only after their signal was handed to
 * the caller; a separate lease pins that signal until owner free or proven
 * task removal. Reserved callers never park. Normal full-pool callers park
 * outside every boundary, with retained backend waiter records.
 * Dead dormant controls and object-free TX_SLEEP controls are reclaimed.
 * Other removed ACTIVE/blocked/paused callers fail closed at admission;
 * general dead-owner queue/mutex cleanup is OPEN.
 * Task-list membership precedes every owner dereference. Shape stamps are
 * conservative guards, not a unique Task ID on KS3.1: exact address/shape/bit
 * recycling is not proven distinguishable. No general ABA/ISR claim.
 * Start/stop in the same creator outside boundaries. Stop refuses records,
 * evicted signal leases or pending adopters. Runtime reset is pinned. */
int anx_exec_callers_start(AnxExecClock *);
int anx_exec_callers_stop(void);
typedef struct {ULONG live,leases,waiting,admitted,resumed,evicted,reclaimed,dead_sleep,timeouts,notifications;} AnxCallerStats;
void anx_exec_callers_stats(AnxCallerStats *);
/* Research-only observable exhaustion counter. Monotonic; never reset by
 * start/stop. Fixture may inject ULONG_MAX only at final empty-registry test. */
extern ULONG anx_exec_caller_generation;
/* Quiescent manager-removal recovery only; clock creator transferred first. */
int anx_exec_callers_recover_creator(struct Task *dead);
int anx_exec_callers_claim_recovery(struct Task *dead,int claim);
#endif
