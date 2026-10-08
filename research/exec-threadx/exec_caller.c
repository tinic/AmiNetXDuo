/* Cached Exec admission; all controls/frames outlive caller stacks.
 * SPDX-License-Identifier: MIT */
#include "exec_caller.h"
#include "tx_thread.h"
#include <exec/execbase.h>
#include <proto/exec.h>
#include <string.h>
/* Same guarded alternate-Exec extension as tx_thread_interrupt_control.c.
 * Commodore ROMs do not set TF_ETASK. The pointer overlays tc_TrapAlloc;
 * et_UniqueID follows Message and parent APTR in the extension. */
#define ANX_TASK_ETASK_OFF 34
#define ANX_ETASK_ID_OFF (sizeof(struct Message)+sizeof(APTR))
_Static_assert(__builtin_offsetof(struct Task,tc_TrapAlloc)==ANX_TASK_ETASK_OFF,"Task extension ABI");
_Static_assert(ANX_ETASK_ID_OFF==24,"ETask identity ABI");
enum {EMPTY,DORMANT,ACTIVE,REMOVING};
typedef struct {
    struct Task *owner;
    ULONG mask,stamp;
    BYTE signal;
    unsigned debt;
} Lease;
typedef struct {
    TX_THREAD thread;
    AnxTxThread bridge;
    AnxTxContext frame;
    AnxWait wait;
    AnxWaitOps ops;
    Lease *lease;
    ULONG generation;
    unsigned state,handed;
} Caller;
typedef struct {struct Task *owner;ULONG mask,stamp;} Pending;
static Caller callers[ANX_CALLER_SLOTS];
static Lease leases[ANX_CALLER_LEASES];
static Pending pending[ANX_CALLER_WAITERS];
static AnxExecClock *clock_owner;
static struct Task *creator;
static AnxCallerStats stats;
static unsigned detaching;
ULONG anx_exec_caller_generation;
static void enter(void *arg) {(void)arg;Forbid();}
static void leave(void *arg) {(void)arg;Permit();}

static ULONG stamp(struct Task *task)
{
    ULONG value=(ULONG)task->tc_SPLower ^ ((ULONG)task->tc_SPUpper<<1) ^ ((ULONG)task->tc_Node.ln_Name<<2);
    if (task->tc_Flags&TF_ETASK) {
        UBYTE *etask=*(UBYTE **)((UBYTE *)task+ANX_TASK_ETASK_OFF);
        if (etask) value^=*(ULONG *)(etask+ANX_ETASK_ID_OFF);
    }
    return value ? value : 1;
}
static int removed(struct Task *owner,ULONG mask)
{
    return !tx_amiga_exec_task_alive(owner) || !(owner->tc_SigAlloc&mask);
}
static int identity(Lease *lease)
{
    return lease && !removed(lease->owner,lease->mask) && stamp(lease->owner)==lease->stamp;
}
static Caller *lookup(TX_THREAD *thread,ULONG generation)
{
    if (!generation) return 0;
    for (unsigned i=0;i<ANX_CALLER_SLOTS;i++)
        if (&callers[i].thread==thread && callers[i].state!=EMPTY && callers[i].generation==generation) return &callers[i];
    return 0;
}
static void release_record(Caller *r,int debt)
{
    if (debt) r->lease->debt=1;
    else {memset(r->lease,0,sizeof(*r->lease));stats.leases--;}
    r->state=EMPTY;r->lease=0;r->handed=0;stats.live--;
}
static void retire(Caller *r,int debt)
{
    if (r->state!=DORMANT || !anx_tx_forget_dormant(&r->bridge))
        anx_tx_unsupported("caller retirement retained active references");
    release_record(r,debt);
}
static void sweep(void)
{
    for (unsigned i=0;i<ANX_CALLER_SLOTS;i++) {
        Caller *r=&callers[i];
        if (r->state!=EMPTY && removed(r->lease->owner,r->lease->mask)) {
            /* Never run a producer that could write into a removed caller's
             * stack while it retains an active cleanup/output pointer. */
            if (r->state==DORMANT) retire(r,0);
            else {
                r->state=REMOVING;
                if (!anx_tx_forget_dead_sleep(&r->bridge)) anx_tx_unsupported("removed active caller cleanup not implemented");
                release_record(r,0);stats.dead_sleep++;
            }
            stats.reclaimed++;
        }
    }
    for (unsigned i=0;i<ANX_CALLER_LEASES;i++)
        if (leases[i].owner && leases[i].debt && removed(leases[i].owner,leases[i].mask)) {
            memset(&leases[i],0,sizeof(leases[i]));stats.leases--;stats.reclaimed++;
        }
    for (unsigned i=0;i<ANX_CALLER_WAITERS;i++)
        if (pending[i].owner && removed(pending[i].owner,pending[i].mask)) {
            memset(&pending[i],0,sizeof(pending[i]));stats.waiting--;stats.reclaimed++;
        }
}
static uint64_t now(void *arg)
{
    uint64_t value;
    (void)arg;
    if (!anx_exec_clock_now(clock_owner,&value)) anx_tx_unsupported("caller deadline clock unavailable");
    return value;
}
static void notify(void *arg)
{
    Caller *r=arg;
    if (r->state==REMOVING && r->wait.result==ANX_WAIT_DELETED && removed(r->lease->owner,r->lease->mask)) return;
    if (r->state!=ACTIVE || !identity(r->lease)) anx_tx_unsupported("caller wake lost owner identity");
    Signal(r->lease->owner,r->lease->mask);stats.notifications++;
}
static int park(void *arg,uint64_t deadline)
{
    Caller *r=arg;
    if (!tx_amiga_exec_task_context() || !anx_tx_runtime_idle() || r->state!=ACTIVE ||
        !r->lease || r->lease->owner!=FindTask(0)) anx_tx_unsupported("caller wait outside owner/idle context");
    (void)Wait(r->lease->mask);
    return deadline==ANX_WAIT_FOREVER || now(r)<deadline;
}
static int slot_available(void)
{
    for (unsigned i=0;i<ANX_CALLER_SLOTS-1;i++) {
        Caller *r=&callers[i];
        if (r->wait.generation!=UINT32_MAX && (r->state==EMPTY ||
            (r->state==DORMANT && r->handed && identity(r->lease) && anx_tx_quiescent(&r->bridge)))) return 1;
    }
    return 0;
}
static void poll(void *arg,uint64_t time)
{
    (void)arg;sweep();
    for (unsigned i=0;i<ANX_CALLER_SLOTS;i++) {
        Caller *r=&callers[i];
        if (r->state==ACTIVE && r->wait.result==ANX_WAIT_PENDING &&
            r->wait.deadline!=ANX_WAIT_FOREVER && time>=r->wait.deadline) {
            if (!anx_wait_complete(&r->wait,r->wait.generation,ANX_WAIT_TIMEOUT))
                anx_tx_unsupported("caller deadline generation changed");
            stats.timeouts++;
        }
    }
    /* Notifications are retained by Exec even before the owner starts Wait.
     * A contender rechecks/claims under Forbid; a wake is not a reservation. */
    if (!slot_available()) return;
    for (unsigned i=0;i<ANX_CALLER_WAITERS;i++) {
        Pending *p=&pending[i];
        if (p->owner && !removed(p->owner,p->mask) && stamp(p->owner)==p->stamp) Signal(p->owner,p->mask);
    }
}
static int can_detach(void *arg) {(void)arg;return detaching && !stats.live && !stats.leases && !stats.waiting;}
int anx_exec_callers_start(AnxExecClock *clock)
{
    if (!clock || clock_owner || !tx_amiga_exec_task_context() || !anx_tx_runtime_idle() || anx_tx_admission_check) return 0;
    if (!anx_exec_clock_service(clock,poll,can_detach,0)) return 0;
    Forbid();clock_owner=clock;creator=FindTask(0);anx_tx_admission_check=sweep;anx_tx_runtime_hold();Permit();return 1;
}
int anx_exec_callers_stop(void)
{
    if (!clock_owner || creator!=FindTask(0) || !tx_amiga_exec_task_context() || !anx_tx_runtime_idle()) return 0;
    Forbid();sweep();
    if (stats.live || stats.leases || stats.waiting) {Permit();return 0;}
    detaching=1;Permit();
    if (!anx_exec_clock_service(clock_owner,0,0,0)) {Forbid();detaching=0;Permit();return 0;}
    Forbid();anx_tx_admission_check=0;clock_owner=0;creator=0;detaching=0;anx_tx_runtime_drop();Permit();return 1;
}
void anx_exec_callers_stats(AnxCallerStats *out) {if (out) {Forbid();*out=stats;Permit();}}
UINT tx_amiga_adopt_handle_valid(TX_THREAD *thread,ULONG generation)
{
    UINT valid;Forbid();valid=lookup(thread,generation)!=0;Permit();return valid;
}
ULONG tx_amiga_adopt_generation(TX_THREAD *thread)
{
    ULONG generation=0;Forbid();
    for (unsigned i=0;i<ANX_CALLER_SLOTS;i++) if (&callers[i].thread==thread && callers[i].state!=EMPTY) generation=callers[i].generation;
    Permit();return generation;
}
static Caller *free_slot(UINT reserved)
{
    unsigned limit=reserved ? ANX_CALLER_SLOTS : ANX_CALLER_SLOTS-1;
    for (unsigned i=0;i<limit;i++) if (callers[i].state==EMPTY && callers[i].wait.generation!=UINT32_MAX) return &callers[i];
    for (unsigned i=0;i<limit;i++) {
        Caller *r=&callers[i];
        if (r->state==DORMANT && r->handed && identity(r->lease) && r->wait.generation!=UINT32_MAX && anx_tx_quiescent(&r->bridge)) {
            retire(r,1);stats.evicted++;return r;
        }
    }
    return 0;
}
UINT tx_amiga_adopt_thread(TX_THREAD **thread,ULONG *generation,CHAR *name,UINT priority,UINT reserved)
{
    Lease *lease=0;Caller *r;Pending *waiter=0;
    BYTE signal;ULONG mask;
    if (!thread || !generation || !name || priority>=TX_MAX_PRIORITIES || reserved>TX_TRUE) return TX_PTR_ERROR;
    if (!tx_amiga_exec_task_context() || !anx_tx_runtime_idle()) return TX_CALLER_ERROR;
    Forbid();
    if (!clock_owner || detaching || anx_exec_caller_generation==(ULONG)-1) {Permit();return TX_NOT_DONE;}
    sweep();
    for (unsigned i=0;i<ANX_CALLER_SLOTS;i++) if (callers[i].state!=EMPTY && callers[i].lease->owner==FindTask(0)) {Permit();return TX_CALLER_ERROR;}
    for (unsigned i=0;i<ANX_CALLER_LEASES;i++) if (!leases[i].owner) {lease=&leases[i];break;}
    signal=lease ? AllocSignal(-1) : -1;
    if (signal<0) {Permit();return TX_NO_MEMORY;}
    mask=1UL<<signal;(void)SetSignal(0,mask);
    while (!(r=free_slot(reserved))) {
        if (reserved) {FreeSignal(signal);Permit();return TX_NO_MEMORY;}
        if (!waiter) {
            for (unsigned i=0;i<ANX_CALLER_WAITERS;i++) if (!pending[i].owner) {waiter=&pending[i];break;}
            if (!waiter) {FreeSignal(signal);Permit();return TX_NO_MEMORY;}
            *waiter=(Pending){FindTask(0),mask,stamp(FindTask(0))};stats.waiting++;
        }
        Permit();(void)Wait(mask);Forbid();sweep();
        if (!clock_owner || detaching || anx_exec_caller_generation==(ULONG)-1) {
            memset(waiter,0,sizeof(*waiter));stats.waiting--;FreeSignal(signal);Permit();return TX_NOT_DONE;
        }
    }
    if (waiter) {memset(waiter,0,sizeof(*waiter));stats.waiting--;}
    /* Reserve the lease NOW, after a park: another task may have consumed the
     * initially observed free lease while this caller was not protected. */
    lease=0;for (unsigned i=0;i<ANX_CALLER_LEASES;i++) if (!leases[i].owner) {lease=&leases[i];break;}
    if (!lease) {FreeSignal(signal);Permit();return TX_NO_MEMORY;}
    *lease=(Lease){FindTask(0),mask,stamp(FindTask(0)),signal,0};
    uint32_t wait_generation=r->wait.generation;
    r->ops=(AnxWaitOps){enter,leave,now,park,notify,r};anx_wait_init(&r->wait,&r->ops);r->wait.generation=wait_generation;
    if (!anx_tx_attach(&r->bridge,&r->thread,&r->wait,(uintptr_t)FindTask(0))) {
        memset(lease,0,sizeof(*lease));FreeSignal(signal);Permit();return TX_CALLER_ERROR;
    }
    r->lease=lease;r->state=ACTIVE;r->handed=0;r->generation=++anx_exec_caller_generation;
    r->thread.tx_thread_name=name;r->thread.tx_thread_priority=priority;r->thread.tx_thread_user_priority=priority;
    r->thread.tx_thread_preempt_threshold=priority;r->thread.tx_thread_user_preempt_threshold=priority;
    r->thread.tx_thread_stack_start=FindTask(0)->tc_SPLower;
    r->thread.tx_thread_stack_end=(UBYTE *)FindTask(0)->tc_SPUpper-1;
    r->thread.tx_thread_stack_size=(ULONG)((UBYTE *)FindTask(0)->tc_SPUpper-(UBYTE *)FindTask(0)->tc_SPLower);
    r->thread.tx_thread_amiga_flags=TX_AMIGA_THREAD_ADOPTED;
    *thread=&r->thread;*generation=r->generation;stats.live++;stats.leases++;stats.admitted++;
    anx_tx_context_begin(&r->frame,&r->thread,0);Permit();return TX_SUCCESS;
}
UINT tx_amiga_adopt_suspend(TX_THREAD *thread,ULONG generation)
{
    Caller *r;Forbid();r=lookup(thread,generation);
    if (!r) {Permit();return TX_THREAD_ERROR;}
    if (r->lease->owner!=FindTask(0) || r->state!=ACTIVE || !anx_tx_context_is_outer(&r->frame)) {Permit();return TX_CALLER_ERROR;}
    if (!anx_tx_quiescent(&r->bridge) || thread->tx_thread_preempt_threshold!=thread->tx_thread_priority) {Permit();return TX_FEATURE_NOT_ENABLED;}
    r->state=DORMANT;anx_tx_context_end(&r->frame);Permit();return TX_SUCCESS;
}
UINT tx_amiga_adopt_resume(TX_THREAD *thread,ULONG generation)
{
    Caller *r;
    if (!tx_amiga_exec_task_context() || !anx_tx_runtime_idle()) return TX_CALLER_ERROR;
    Forbid();r=lookup(thread,generation);
    if (!r) {Permit();return TX_THREAD_ERROR;}
    if (r->lease->owner!=FindTask(0) || r->state!=DORMANT || !anx_tx_quiescent(&r->bridge)) {Permit();return TX_CALLER_ERROR;}
    r->lease->stamp=stamp(FindTask(0));r->state=ACTIVE;stats.resumed++;
    thread->tx_thread_stack_start=FindTask(0)->tc_SPLower;
    thread->tx_thread_stack_end=(UBYTE *)FindTask(0)->tc_SPUpper-1;
    thread->tx_thread_stack_size=(ULONG)((UBYTE *)FindTask(0)->tc_SPUpper-(UBYTE *)FindTask(0)->tc_SPLower);
    anx_tx_context_begin(&r->frame,thread,0);Permit();return TX_SUCCESS;
}
UINT tx_amiga_orphan_thread(TX_THREAD *thread,ULONG generation)
{
    Caller *r;UINT status;
    Forbid();r=lookup(thread,generation);
    if (!r) {Permit();return TX_THREAD_ERROR;}
    if (r->lease->owner!=FindTask(0) || r->state!=ACTIVE) {Permit();return TX_CALLER_ERROR;}
    status=tx_amiga_adopt_suspend(thread,generation);
    if (status!=TX_SUCCESS) {Permit();return status;}
    BYTE signal=r->lease->signal;ULONG mask=r->lease->mask;retire(r,0);
    (void)SetSignal(0,mask);FreeSignal(signal);Permit();return TX_SUCCESS;
}
UINT tx_amiga_discard_thread(TX_THREAD *thread,ULONG generation)
{
    Caller *r;Forbid();r=lookup(thread,generation);
    if (!r) {Permit();return TX_THREAD_ERROR;}
    if (r->state!=DORMANT || !removed(r->lease->owner,r->lease->mask)) {Permit();return TX_FEATURE_NOT_ENABLED;}
    retire(r,0);stats.reclaimed++;Permit();return TX_SUCCESS;
}
ULONG tx_amiga_adopt_signal(TX_THREAD *thread)
{
    ULONG mask=0;Forbid();
    Caller *r=lookup(thread,tx_amiga_adopt_generation(thread));
    if (r && r->state==ACTIVE && r->lease->owner==FindTask(0)) {r->handed=1;mask=r->lease->mask;}
    Permit();return mask;
}
VOID tx_amiga_adopt_signal_free(ULONG mask)
{
    Forbid();
    for (unsigned i=0;i<ANX_CALLER_LEASES;i++) {
        Lease *lease=&leases[i];
        if (lease->owner==FindTask(0) && lease->mask==mask && lease->debt && identity(lease)) {
            (void)SetSignal(0,mask);FreeSignal(lease->signal);memset(lease,0,sizeof(*lease));stats.leases--;break;
        }
    }
    Permit();
}
VOID tx_amiga_adopt_sweep_unpublished(VOID) {Forbid();if (clock_owner) sweep();Permit();}

int anx_exec_callers_recover_creator(struct Task *dead)
{
    if (!clock_owner || creator!=dead || !tx_amiga_exec_task_context() || !anx_tx_runtime_idle()) return 0;
    Forbid();
    if (tx_amiga_exec_task_alive(dead) || clock_owner->creator!=FindTask(0) ||
        stats.live || stats.leases || stats.waiting || !anx_tx_runtime_retains_only(2)) {Permit();return 0;}
    creator=FindTask(0);Permit();return 1;
}

int anx_exec_callers_claim_recovery(struct Task *dead,int claim)
{
    Forbid();
    if (!clock_owner || creator!=dead || tx_amiga_exec_task_alive(dead) ||
        stats.live || stats.leases || stats.waiting || !anx_tx_runtime_retains_only(2)) {Permit();return 0;}
    detaching=claim!=0;Permit();return 1;
}
