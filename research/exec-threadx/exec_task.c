/* Bounded Exec task publication and normal retirement, no ThreadX scheduler.
 * SPDX-License-Identifier: MIT */
#include "exec_task.h"
#include <exec/execbase.h>
#include <proto/exec.h>
#include <string.h>

static int idle_caller(void)
{
    return SysBase->TDNestCnt<0 && SysBase->IDNestCnt<0 && anx_tx_runtime_idle();
}

static void finish(AnxExecTask *r)
{
    /* Parent cannot observe FINISHED before Exec removes this task. Neither
     * embedded Task nor caller stack is on tc_MemEntry: creator retains both.
     * RemTask discards our Forbid; no Permit/scheduling gap after publication. */
    Forbid();
    r->state=ANX_TASK_FINISHED;
    Signal(r->creator,r->ack);
    RemTask(0);
    for (;;) {}
}

static VOID task_entry(VOID)
{
    AnxExecTask *r=(AnxExecTask *)FindTask(0)->tc_UserData;
    AnxTxContext frame;
    if (!anx_exec_wait_open(&r->wait)) {
        finish(r);
    }
    if (!anx_tx_attach(&r->bridge,&r->thread,&r->wait.wait,(uintptr_t)FindTask(0))) {
        if (!anx_exec_wait_close(&r->wait))
            anx_tx_unsupported("task startup private IO rollback failed");
        finish(r);
    }
    /* attach initializes the control block. Publish metadata only afterwards,
     * under protection, before the creator receives its startup ACK. */
    Forbid();
    r->thread.tx_thread_name=r->name;
    r->thread.tx_thread_entry=r->entry;
    r->thread.tx_thread_entry_parameter=r->input;
    r->thread.tx_thread_stack_start=r->stack;
    r->thread.tx_thread_stack_end=(UBYTE *)r->stack+r->stack_size-1;
    r->thread.tx_thread_stack_size=r->stack_size;
    r->started=1;
    r->state=ANX_TASK_RUNNING;
    Signal(r->creator,r->ack);
    Permit();
    anx_tx_context_begin(&frame,&r->thread,0);
    r->entry(r->input);
    anx_tx_context_end(&frame);
    /* The caller contracts producer quiescence. The bridge checks concrete
     * pending waits, cleanup gates, abort pins, owned mutexes and timer links. */
    if (!anx_tx_detach(&r->bridge))
        anx_tx_unsupported("task entry returned before quiescence");
    if (!anx_exec_wait_close(&r->wait))
        anx_tx_unsupported("task entry private IO retirement failed");
    finish(r);
}

int anx_exec_task_reap(AnxExecTask *r)
{
    BYTE signal;
    if (!r || !idle_caller() || r->creator!=FindTask(0))
        return 0;
    Forbid();
    if (r->state!=ANX_TASK_FINISHED) {
        Permit();
        return 0;
    }
    signal=r->signal;
    (void)SetSignal(0,r->ack);
    FreeSignal(signal);
    r->signal=-1;
    r->ack=0;
    r->creator=0;
    r->state=ANX_TASK_REAPED;
    Permit();
    return 1;
}

int anx_exec_task_join(AnxExecTask *r)
{
    if (!r || !idle_caller() || r->creator!=FindTask(0))
        return 0;
    for (;;) {
        unsigned finished;
        Forbid();
        finished=r->state==ANX_TASK_FINISHED;
        Permit();
        if (finished) return anx_exec_task_reap(r);
        /* ACK is creator-owned and retained by Exec even before Wait. */
        (void)Wait(r->ack);
    }
}

int anx_exec_task_start(AnxExecTask *r, CHAR *name, VOID (*entry)(ULONG),
                        ULONG input, APTR stack, ULONG size)
{
    uintptr_t base=(uintptr_t)stack, end=base+size, rb=(uintptr_t)r;
    BYTE signal;
    if (!r || !name || !entry || !base || (base&3) || (size&3) ||
        size<8192 || end<=base || rb>UINTPTR_MAX-sizeof(*r) ||
        (base<rb+sizeof(*r) && end>rb) || !idle_caller())
        return 0;
    if (r->state!=ANX_TASK_EMPTY && r->state!=ANX_TASK_REAPED)
        return 0;
    signal=AllocSignal(-1);
    if (signal<0) return 0;
    Forbid();
    memset(r,0,sizeof(*r));
    r->creator=FindTask(0); r->signal=signal; r->ack=1UL<<signal;
    r->entry=entry; r->input=input; r->stack=stack; r->stack_size=size; r->name=name;
    r->state=ANX_TASK_STARTING;
    (void)SetSignal(0,r->ack);
    r->task.tc_Node.ln_Type=NT_TASK;
    r->task.tc_Node.ln_Name=name;
    r->task.tc_Node.ln_Pri=0;
    r->task.tc_SPLower=stack; r->task.tc_SPUpper=(APTR)end; r->task.tc_SPReg=(APTR)end;
    r->task.tc_UserData=r;
    r->task.tc_MemEntry.lh_Head=(struct Node *)&r->task.tc_MemEntry.lh_Tail;
    r->task.tc_MemEntry.lh_Tail=0;
    r->task.tc_MemEntry.lh_TailPred=(struct Node *)&r->task.tc_MemEntry.lh_Head;
    if (!AddTask(&r->task,(APTR)task_entry,0)) {
        FreeSignal(signal);
        r->signal=-1; r->creator=0; r->ack=0; r->state=ANX_TASK_EMPTY;
        Permit();
        return 0;
    }
    Permit();
    for (;;) {
        unsigned state,started;
        Forbid(); state=r->state; started=r->started; Permit();
        if (started) return 1;
        if (state==ANX_TASK_FINISHED) {
            if (!anx_exec_task_reap(r))
                anx_tx_unsupported("task startup reap failed");
            return 0;
        }
        (void)Wait(r->ack);
    }
}
