/* Public creation boundary backed by already prepared Exec workers.
 * SPDX-License-Identifier: MIT */
#define TX_SOURCE_CODE
#include "exec_thread.h"
#include "tx_thread.h"
#include <exec/execbase.h>
#include <proto/exec.h>
#include <string.h>
#ifndef TX_DISABLE_STACK_FILLING
#error "prepared workers require the pinned port's disabled stack filling"
#endif
_Static_assert(TX_MAX_PRIORITIES==32,"review Exec priority mapping after a profile change");
_Static_assert(TX_AMIGA_TASK_PRIORITY==1,"review safe Exec priority band after a port change");

TX_THREAD *_tx_thread_created_ptr;
ULONG _tx_thread_created_count;
static AnxExecThread *records;

static int idle(void)
{
    return SysBase->TDNestCnt<0 && SysBase->IDNestCnt<0 && anx_tx_runtime_idle();
}
static AnxExecThread *lookup(TX_THREAD *t)
{
    AnxExecThread *r;
    for (r=records;r;r=r->next) if (r->thread==t) return r;
    return 0;
}
static void finish(AnxExecThread *r)
{
    Forbid(); r->state=ANX_THREAD_FINISHED;
    Signal(r->creator,r->ack); RemTask(0);
    for (;;) {}
}
static void terminal_owner(void *arg)
{
    AnxExecThread *r=arg;
    if (!idle() || FindTask(0)!=&r->task || r->state!=ANX_THREAD_STOPPING ||
        r->bridge.thread || !r->bridge.terminal_pending)
        anx_tx_unsupported("invalid native terminal owner");
    if (!anx_exec_wait_close(&r->wait)) anx_tx_unsupported("terminal owner IO close failed");
    finish(r);
}
static VOID worker(VOID)
{
    AnxExecThread *r=FindTask(0)->tc_UserData;
    AnxTxContext frame;
    if (!anx_exec_wait_open(&r->wait)) finish(r);
    Forbid(); r->prepared=1; r->state=ANX_THREAD_PREPARED;
    Signal(r->creator,r->ack); Permit();
    for (;;) {
        unsigned state,run;
        Forbid(); state=r->state;
        run=state==ANX_THREAD_BOUND && r->thread->tx_thread_state==TX_READY;
        Permit();
        if (state==ANX_THREAD_CANCEL) {
            if (!anx_exec_wait_close(&r->wait)) anx_tx_unsupported("reservation IO close failed");
            finish(r);
        }
        if (run) break;
        (void)Wait(SIGF_SINGLE);
    }
    anx_tx_context_begin(&frame,r->thread,0);
    if (!anx_tx_set_terminal_owner(&r->bridge,terminal_owner,r))
        anx_tx_unsupported("created terminal owner registration failed");
    r->entered=1;
    r->thread->tx_thread_entry(r->thread->tx_thread_entry_parameter);
    anx_tx_context_end(&frame);
    if (!anx_tx_complete(&r->bridge)) anx_tx_unsupported("created entry returned before quiescence");
    if (!anx_exec_wait_close(&r->wait)) anx_tx_unsupported("created thread IO close failed");
    finish(r);
}
static void release_record(AnxExecThread *r)
{
    AnxExecThread **p;
    for (p=&records;*p && *p!=r;p=&(*p)->next) {}
    if (!*p) anx_tx_unsupported("created record missing");
    *p=r->next;
    (void)SetSignal(0,r->ack); FreeSignal(r->signal);
    r->signal=-1; r->ack=0; r->creator=0; r->next=0; r->state=ANX_THREAD_REAPED;
    anx_tx_runtime_drop();
}
int anx_exec_thread_prepare(AnxExecThread *r, TX_THREAD *thread, CHAR *name, APTR stack, ULONG size)
{
    uintptr_t b=(uintptr_t)stack,e=b+size, rb=(uintptr_t)r,tb=(uintptr_t)thread;
    AnxExecThread *other;
    BYTE signal;
    if (!r || !thread || !name || !b || (b&3) || (size&3) || size<8192 || e<=b ||
        rb>UINTPTR_MAX-sizeof(*r) || tb>UINTPTR_MAX-sizeof(*thread) ||
        (b<rb+sizeof(*r) && e>rb) || (b<tb+sizeof(*thread) && e>tb) ||
        (rb<tb+sizeof(*thread) && rb+sizeof(*r)>tb) || !idle()) return 0;
    Forbid();
    for (other=records;other;other=other->next) {
        uintptr_t lo[3]={rb,tb,b}, hi[3]={rb+sizeof(*r),tb+sizeof(*thread),e};
        uintptr_t olo[3]={(uintptr_t)other,(uintptr_t)other->thread,(uintptr_t)other->stack};
        uintptr_t ohi[3]={olo[0]+sizeof(*other),olo[1]+sizeof(*thread),olo[2]+other->stack_size};
        unsigned i,j;
        for (i=0;i<3;i++) for (j=0;j<3;j++)
            if (lo[i]<ohi[j] && hi[i]>olo[j]) {Permit();return 0;}
    }
    if (r->state!=ANX_THREAD_EMPTY && r->state!=ANX_THREAD_REAPED) {Permit(); return 0;}
    signal=AllocSignal(-1);
    if (signal<0) {Permit(); return 0;}
    memset(r,0,sizeof(*r));
    r->creator=FindTask(0); r->thread=thread; r->stack=stack; r->stack_size=size;
    r->signal=signal; r->ack=1UL<<signal; r->state=ANX_THREAD_PREPARING;
    r->next=records; records=r;
    anx_tx_runtime_hold();
    r->task.tc_Node.ln_Type=NT_TASK; r->task.tc_Node.ln_Name=name;
    r->task.tc_SPLower=stack; r->task.tc_SPUpper=(APTR)e; r->task.tc_SPReg=(APTR)e;
    r->task.tc_UserData=r;
    r->task.tc_MemEntry.lh_Head=(struct Node *)&r->task.tc_MemEntry.lh_Tail;
    r->task.tc_MemEntry.lh_TailPred=(struct Node *)&r->task.tc_MemEntry.lh_Head;
    (void)SetSignal(0,r->ack);
    if (!AddTask(&r->task,(APTR)worker,0)) {release_record(r);Permit();return 0;}
    Permit();
    for (;;) {
        unsigned state;
        Forbid(); state=r->state; Permit();
        if (state==ANX_THREAD_PREPARED) return 1;
        if (state==ANX_THREAD_FINISHED) {Forbid();release_record(r);Permit();return 0;}
        (void)Wait(r->ack);
    }
}
int anx_exec_thread_cancel(AnxExecThread *r)
{
    if (!r || !idle() || r->creator!=FindTask(0)) return 0;
    Forbid();
    if (r->state!=ANX_THREAD_PREPARED) {Permit();return 0;}
    r->state=ANX_THREAD_CANCEL; Signal(&r->task,SIGF_SINGLE); Permit();
    for (;;) {
        unsigned done;
        Forbid(); done=r->state==ANX_THREAD_FINISHED; Permit();
        if (done) {Forbid();release_record(r);Permit();return 1;}
        (void)Wait(r->ack);
    }
}
int anx_exec_thread_wait(AnxExecThread *r)
{
    if (!r || !idle() || r->creator!=FindTask(0) ||
        (r->state!=ANX_THREAD_BOUND && r->state!=ANX_THREAD_STOPPING &&
         r->state!=ANX_THREAD_FINISHED)) return 0;
    for (;;) {
        unsigned done;
        Forbid(); done=r->state==ANX_THREAD_FINISHED; Permit();
        if (done) return 1;
        (void)Wait(r->ack);
    }
}
int anx_exec_thread_stop_event(AnxExecThread *r, TX_EVENT_FLAGS_GROUP *group)
{
    anx_tx_require_context(0);
    if (!r || r->creator!=FindTask(0) || r->state!=ANX_THREAD_BOUND || !r->entered) return 0;
    if (!anx_tx_stop_event(&r->bridge,group)) return 0;
    r->state=ANX_THREAD_STOPPING;
    return 1;
}
UINT _tx_thread_create(TX_THREAD *t, CHAR *name, VOID (*entry)(ULONG), ULONG input,
                       VOID *stack, ULONG size, UINT priority, UINT threshold,
                       ULONG slice, UINT auto_start)
{
    AnxExecThread *r;
    TX_THREAD *tail;
    anx_tx_require_context(0);
    if (_tx_thread_system_state) return TX_CALLER_ERROR;
    if (!t || !entry || !name) return TX_PTR_ERROR;
    r=lookup(t);
    if (!r || r->state!=ANX_THREAD_PREPARED) return TX_NO_MEMORY;
    if (r->creator!=FindTask(0)) return TX_CALLER_ERROR;
    if (stack!=r->stack || size!=r->stack_size) return TX_SIZE_ERROR;
    if (priority>=TX_MAX_PRIORITIES) return TX_PRIORITY_ERROR;
    if (threshold>priority) return TX_THRESH_ERROR;
    if (threshold!=priority) return TX_FEATURE_NOT_ENABLED;
    if (auto_start!=TX_AUTO_START && auto_start!=TX_DONT_START) return TX_START_ERROR;
    if (t->tx_thread_id==TX_THREAD_ID) return TX_THREAD_ERROR;
    if (_tx_thread_created_count==(ULONG)-1) anx_tx_unsupported("created count overflow");
    /* Pinned Amiga port disables stack filling/checking: the prepared task
     * already owns this stack. Never link an initializer that fills it here. */
    memset(t,0,sizeof(*t));
    t->tx_thread_name=name; t->tx_thread_entry=entry; t->tx_thread_entry_parameter=input;
    t->tx_thread_stack_start=stack; t->tx_thread_stack_size=size;
    t->tx_thread_stack_end=(UCHAR *)stack+size-1;
    t->tx_thread_stack_ptr=(UCHAR *)t->tx_thread_stack_end-8;
    t->tx_thread_priority=priority; t->tx_thread_user_priority=priority;
    t->tx_thread_preempt_threshold=threshold; t->tx_thread_user_preempt_threshold=threshold;
    t->tx_thread_inherit_priority=TX_MAX_PRIORITIES;
    t->tx_thread_time_slice=slice; t->tx_thread_new_time_slice=slice;
    t->tx_thread_state=TX_SUSPENDED;
    TX_THREAD_CREATE_TIMEOUT_SETUP(t)
    t->tx_thread_amiga_task=&r->task; t->tx_thread_amiga_signal_owner=&r->task;
    t->tx_thread_amiga_run_signal=SIGF_SINGLE;
    t->tx_thread_id=TX_THREAD_ID;
    if (!anx_tx_bind_created(&r->bridge,t,&r->wait.wait,(uintptr_t)&r->task))
        anx_tx_unsupported("prepared creation binding failed");
    if (_tx_thread_created_ptr) {
        tail=_tx_thread_created_ptr->tx_thread_created_previous;
        tail->tx_thread_created_next=t; _tx_thread_created_ptr->tx_thread_created_previous=t;
        t->tx_thread_created_previous=tail; t->tx_thread_created_next=_tx_thread_created_ptr;
    } else {_tx_thread_created_ptr=t;t->tx_thread_created_next=t;t->tx_thread_created_previous=t;}
    _tx_thread_created_count++;
    r->task.tc_Node.ln_Name=name;
    (void)SetTaskPri(&r->task,ANX_THREAD_EXEC_PRIORITY(priority));
    r->state=ANX_THREAD_BOUND;
    if (auto_start==TX_AUTO_START) {t->tx_thread_state=TX_READY;Signal(&r->task,SIGF_SINGLE);}
    return TX_SUCCESS;
}
UINT _tx_thread_resume(TX_THREAD *t)
{
    AnxExecThread *r;
    anx_tx_require_context(0);
    if (_tx_thread_system_state) return TX_CALLER_ERROR;
    r=lookup(t);
    if (!r) return anx_tx_explicit_resume(t);
    if (!t || t->tx_thread_id!=TX_THREAD_ID) return TX_THREAD_ERROR;
    if (r->state!=ANX_THREAD_BOUND || t->tx_thread_state==TX_READY ||
        t->tx_thread_state==TX_COMPLETED) return TX_RESUME_ERROR;
    if (r->entered) return anx_tx_explicit_resume(t);
    if (t->tx_thread_state!=TX_SUSPENDED) return TX_FEATURE_NOT_ENABLED;
    t->tx_thread_state=TX_READY; Signal(&r->task,SIGF_SINGLE);
    return TX_SUCCESS;
}
static void native_yield(void)
{
    struct Task *me=FindTask(0);
    /* Public Exec scheduling point; never alter logical or native priority. */
    (void)SetTaskPri(me,me->tc_Node.ln_Pri);
}
VOID _tx_thread_relinquish(VOID)
{
    if (anx_tx_relinquish(native_yield)!=TX_SUCCESS)
        anx_tx_unsupported("public relinquish outside supported native context");
}
UINT _tx_thread_terminate(TX_THREAD *t)
{
    AnxExecThread *r;
    anx_tx_require_context(0);
    if (_tx_thread_system_state) return TX_CALLER_ERROR;
    r=lookup(t);
    if (!r || !t || t->tx_thread_id!=TX_THREAD_ID) return TX_THREAD_ERROR;
    if (r->creator!=FindTask(0)) return TX_CALLER_ERROR;
    /* Only acknowledge an already stopped, actually removed owner. This is
     * the nonblocking service the unchanged IP delete can safely call. */
    if (r->state!=ANX_THREAD_FINISHED || t->tx_thread_state!=TX_TERMINATED ||
        r->wait.opened || r->bridge.thread || !r->bridge.terminal_pending)
        return TX_FEATURE_NOT_ENABLED;
    return TX_SUCCESS;
}
UINT _tx_thread_delete(TX_THREAD *t)
{
    AnxExecThread *r;
    anx_tx_require_context(0);
    if (_tx_thread_system_state) return TX_CALLER_ERROR;
    r=lookup(t);
    if (!r || !t || t->tx_thread_id!=TX_THREAD_ID) return TX_THREAD_ERROR;
    if (r->creator!=FindTask(0)) return TX_CALLER_ERROR;
    if (r->state!=ANX_THREAD_FINISHED ||
        (t->tx_thread_state!=TX_COMPLETED && t->tx_thread_state!=TX_TERMINATED) ||
        t->tx_thread_suspending) return TX_DELETE_ERROR;
    if (!_tx_thread_created_count) anx_tx_unsupported("created count lost");
    if (t->tx_thread_created_next==t) _tx_thread_created_ptr=0;
    else {
        t->tx_thread_created_previous->tx_thread_created_next=t->tx_thread_created_next;
        t->tx_thread_created_next->tx_thread_created_previous=t->tx_thread_created_previous;
        if (_tx_thread_created_ptr==t) _tx_thread_created_ptr=t->tx_thread_created_next;
    }
    _tx_thread_created_count--; t->tx_thread_id=0;
    t->tx_thread_amiga_task=0; t->tx_thread_amiga_signal_owner=0; t->tx_thread_amiga_run_signal=0;
    t->tx_thread_created_next=0; t->tx_thread_created_previous=0;
    release_record(r);
    return TX_SUCCESS;
}
const AnxExecThread *anx_exec_thread_owner_record(void)
{
    anx_tx_require_context(0);
    if (_tx_thread_system_state || !_tx_thread_current_ptr) return 0;
    AnxExecThread *r=lookup(_tx_thread_current_ptr);
    if (!r || r->state!=ANX_THREAD_BOUND || r->bridge.thread!=_tx_thread_current_ptr ||
        FindTask(0)!=&r->task) return 0;
    return r;
}

UINT anx_exec_thread_stack_in_use(const VOID *start,ULONG size)
{
    uintptr_t lo=(uintptr_t)start,hi=lo+size;
    if (!lo || hi<lo) return TX_TRUE;
    Forbid();
    for (AnxExecThread *r=records;r;r=r->next) {
        uintptr_t b=(uintptr_t)r->stack,e=b+r->stack_size;
        if (lo<=e && b<=hi) {Permit();return TX_TRUE;}
    }
    Permit();return TX_FALSE;
}
