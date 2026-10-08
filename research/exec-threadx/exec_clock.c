/* Exec timer owner: serialize ticks, close producer gate, reap before release.
 * SPDX-License-Identifier: MIT */
#include "exec_clock.h"
#include "clock_schedule.h"
#include "tx_thread.h"
#include <exec/execbase.h>
#include <proto/exec.h>
#include <string.h>
_Static_assert(TX_TIMER_TICKS_PER_SECOND>0 &&
               1000000%TX_TIMER_TICKS_PER_SECOND==0,"clock requires integral microsecond ticks");
#define PERIOD (1000000UL/TX_TIMER_TICKS_PER_SECOND)
static AnxExecClock *active;
static int idle(void)
{
    return SysBase->TDNestCnt<0 && SysBase->IDNestCnt<0 && anx_tx_runtime_idle();
}
static void finish(AnxExecClock *r)
{
    /* No scheduling gap between FINISHED and actual native removal. */
    Forbid();r->state=ANX_CLOCK_FINISHED;Signal(r->creator,r->ack);RemTask(0);
    for (;;) {}
}
static void release(AnxExecClock *r)
{
    (void)SetSignal(0,r->ack);FreeSignal(r->signal);
    r->signal=-1;r->ack=0;r->creator=0;r->state=ANX_CLOCK_REAPED;
    active=0;anx_tx_runtime_drop();
}
static VOID worker(VOID)
{
    AnxExecClock *r=FindTask(0)->tc_UserData;
    AnxTxContext f;
    if (!anx_exec_wait_open(&r->wait)) finish(r);
    Forbid();
    uint64_t now=r->wait.ops.clock(r->wait.ops.context);
    if (now>UINT64_MAX-PERIOD) anx_tx_unsupported("clock phase overflow");
    r->next=now+PERIOD;r->started=1;r->state=ANX_CLOCK_RUNNING;
    Signal(r->creator,r->ack);Permit();
    for (;;) {
        unsigned due;
        anx_tx_context_begin(&f,TX_NULL,1);
        if (r->state!=ANX_CLOCK_RUNNING) {anx_tx_context_end(&f);break;}
        now=r->wait.ops.clock(r->wait.ops.context);
        if (r->service) r->service(r->service_context,now);
        due=anx_clock_batch(now,r->next,PERIOD);
        if (due) {
            r->batches++;if (due>1) r->catchup_batches++;
            for (unsigned i=0;i<due;i++) {
                if (r->next>UINT64_MAX-PERIOD) anx_tx_unsupported("clock phase overflow");
                anx_tx_timer_tick();r->ticks++;r->next+=PERIOD;
            }
            anx_tx_context_end(&f); /* dispatch helper/creator between batches */
            continue;
        }
        /* Publish the pending wait while still under the same producer lock.
         * Stop either completes it or is seen before any later publication. */
        r->token=anx_wait_begin(&r->wait.wait,r->next-now,0,0,0,0);
        if (!r->token) anx_tx_unsupported("clock wait publication failed");
        anx_tx_context_end(&f);
        AnxWaitResult result=anx_exec_wait_run(&r->wait,r->token);
        if (result!=ANX_WAIT_TIMEOUT && result!=ANX_WAIT_CANCELLED)
            anx_tx_unsupported("clock timer IO failed");
    }
    if (!anx_exec_wait_close(&r->wait)) anx_tx_unsupported("clock private IO retirement failed");
    finish(r);
}
int anx_exec_clock_can_stop(AnxExecClock *r)
{
    anx_tx_require_context(0);
    return !_tx_thread_system_state && r && active==r && r->creator==FindTask(0) &&
           r->state==ANX_CLOCK_RUNNING && r->wait.opened &&
           (!r->service || r->service_can_detach(r->service_context));
}
int anx_exec_clock_service(AnxExecClock *r,void (*service)(void *,uint64_t),int (*guard)(void *),void *arg)
{
    if (!r || !idle() || active!=r || r->creator!=FindTask(0)) return 0;
    Forbid();
    if (r->state!=ANX_CLOCK_RUNNING || (service ? (!guard || r->service) :
        (!r->service || !r->service_can_detach(r->service_context)))) {Permit();return 0;}
    r->service=service;r->service_can_detach=guard;r->service_context=arg;
    Permit();return 1;
}
int anx_exec_clock_now(AnxExecClock *r,uint64_t *now)
{
    int valid;
    if (!r || !now) return 0;
    Forbid();valid=active==r && r->state==ANX_CLOCK_RUNNING && r->wait.opened;
    if (valid) *now=r->wait.ops.clock(r->wait.ops.context);
    Permit();return valid;
}
int anx_exec_clock_stop(AnxExecClock *r)
{
    if (!anx_exec_clock_can_stop(r)) return 0;
    r->state=ANX_CLOCK_STOPPING;
    if (r->wait.wait.result==ANX_WAIT_PENDING &&
        !anx_wait_complete(&r->wait.wait,r->token,ANX_WAIT_CANCELLED))
        anx_tx_unsupported("clock stop generation mismatch");
    return 1;
}
int anx_exec_clock_join(AnxExecClock *r)
{
    if (!r || !idle() || active!=r || r->creator!=FindTask(0) ||
        (r->state!=ANX_CLOCK_STOPPING && r->state!=ANX_CLOCK_FINISHED)) return 0;
    for (;;) {
        Forbid();
        if (r->state==ANX_CLOCK_FINISHED) {
            if (r->wait.opened || r->wait.timer_sends!=r->wait.timer_reaps)
                anx_tx_unsupported("clock native retirement before IO drain");
            release(r);Permit();return 1;
        }
        Permit();(void)Wait(r->ack);
    }
}
int anx_exec_clock_start(AnxExecClock *r,CHAR *name,APTR stack,ULONG size)
{
    uintptr_t b=(uintptr_t)stack,e=b+size,rb=(uintptr_t)r;
    BYTE signal;
    if (!r || !name || !b || (b&3) || (size&3) || size<8192 || e<=b ||
        rb>UINTPTR_MAX-sizeof(*r) || (b<rb+sizeof(*r) && e>rb) || !idle()) return 0;
    Forbid();
    if (active || (r->state!=ANX_CLOCK_EMPTY && r->state!=ANX_CLOCK_REAPED)) {Permit();return 0;}
    signal=AllocSignal(-1);if (signal<0) {Permit();return 0;}
    memset(r,0,sizeof(*r));r->creator=FindTask(0);r->stack=stack;r->stack_size=size;
    r->signal=signal;r->ack=1UL<<signal;r->state=ANX_CLOCK_STARTING;active=r;
    anx_tx_runtime_hold();(void)SetSignal(0,r->ack);
    r->task.tc_Node.ln_Type=NT_TASK;r->task.tc_Node.ln_Name=name;r->task.tc_Node.ln_Pri=0;
    r->task.tc_SPLower=stack;r->task.tc_SPUpper=(APTR)e;r->task.tc_SPReg=(APTR)e;
    r->task.tc_UserData=r;
    r->task.tc_MemEntry.lh_Head=(struct Node *)&r->task.tc_MemEntry.lh_Tail;
    r->task.tc_MemEntry.lh_TailPred=(struct Node *)&r->task.tc_MemEntry.lh_Head;
    if (!AddTask(&r->task,(APTR)worker,0)) {release(r);Permit();return 0;}
    Permit();
    for (;;) {
        Forbid();
        if (r->started) {Permit();return 1;}
        if (r->state==ANX_CLOCK_FINISHED) {release(r);Permit();return 0;}
        Permit();(void)Wait(r->ack);
    }
}
