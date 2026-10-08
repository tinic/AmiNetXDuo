/* Stable IO creator plus serialized backend-owned commands. No old scheduler.
 * Commands wait on an owner signal outside protection: no caller-owned timer
 * request/reply port or borrowed output pointer. Research lifecycle, not worker policy.
 * SPDX-License-Identifier: MIT */
#include "exec_kernel.h"
#include "tx_bridge_exec.h"
#include <exec/execbase.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <string.h>
enum {DOWN,STARTING,UP,STOPPING,RECOVERING};
enum {START,STOP};
#define STACK_BYTES 8192
static struct Task manager;
static AnxExecClock clock_record;
static APTR manager_stack,clock_stack;
static volatile unsigned phase;
static ULONG manager_stamp;
static struct {
    struct Task *volatile owner;
    ULONG stamp,mask;
    BYTE signal;
    unsigned operation;
    volatile unsigned done;
    UINT result;
} command;
static ULONG task_stamp(struct Task *t)
{
    return (ULONG)t->tc_SPLower ^ (ULONG)t->tc_SPUpper ^ (ULONG)t->tc_Node.ln_Name;
}
static int command_live(void)
{
    return command.owner && tx_amiga_exec_task_alive(command.owner) && task_stamp(command.owner)==command.stamp && (!command.mask || (command.owner->tc_SigAlloc&command.mask));
}
static void wake_command(void)
{
    /* Caller holds Forbid across membership, allocated-bit, stamp and Signal. */
    if (command_live() && command.mask) Signal(command.owner,command.mask);
}
static void watch_manager(void)
{
    if (command.owner && !command_live() && (command.done || !tx_amiga_exec_task_alive(&manager))) {
        memset(&command,0,sizeof(command));return;
    }
    if (command.owner && !command.done && !tx_amiga_exec_task_alive(&manager)) wake_command();
}
static void consume_command(void)
{
    BYTE bit=command.signal;ULONG mask=command.mask;
    memset(&command,0,sizeof(command));
    if (mask) {(void)SetSignal(0,mask);FreeSignal(bit);}
}
static void reply(UINT result)
{
    Forbid();command.result=result;command.done=1;
    if (!command_live()) memset(&command,0,sizeof(command));
    else wake_command();
    Permit();
}
static void release_stacks(void)
{
    /* Native removal proved under caller's Forbid, no Task can use these. */
    if (manager_stack) FreeMem(manager_stack,STACK_BYTES);
    if (clock_stack) FreeMem(clock_stack,STACK_BYTES);
    manager_stack=clock_stack=0;
}
static int close_clock(void)
{
    AnxTxContext f;
    if (clock_record.state==ANX_CLOCK_RUNNING) {
        anx_tx_context_begin(&f,TX_NULL,0);
        int stopped=anx_tx_runtime_retains_only(1) && anx_exec_clock_stop(&clock_record);
        anx_tx_context_end(&f);
        if (!stopped) return 0;
    }
    return anx_exec_clock_join(&clock_record);
}
static VOID finish(VOID)
{
    Forbid();phase=DOWN;command.result=TX_SUCCESS;command.done=1;
    if (!command_live()) memset(&command,0,sizeof(command));
    else wake_command();
    RemTask(0);for (;;) {}
}
static VOID entry(VOID)
{
    anx_tx_runtime_init(anx_tx_exec_platform());
    if (!anx_exec_clock_start(&clock_record,(CHAR *)"Exec NetX clock",clock_stack,STACK_BYTES)) {
        Forbid();phase=DOWN;reply(TX_NO_MEMORY);RemTask(0);for (;;) {}
    }
    if (!anx_exec_callers_start(&clock_record)) {
        if (!close_clock()) anx_tx_unsupported("kernel startup clock rollback failed");
        Forbid();phase=DOWN;reply(TX_NOT_DONE);RemTask(0);for (;;) {}
    }
    Forbid();clock_record.observer=watch_manager;phase=UP;reply(TX_SUCCESS);Permit();
    for (;;) {
        Forbid();int stop=command.owner && !command.done && command.operation==STOP;Permit();
        if (!stop) {(void)Wait(SIGF_SINGLE);continue;}
        /* Prepared workers have runtime holds even before their public IDs.
         * Dormant/paused/sleeping callers have retained bindings. Neither may
         * be confused with an idle global current pointer. */
        if (!anx_tx_runtime_retains_only(2) || !anx_exec_callers_stop()) {
            Forbid();phase=UP;reply(TX_NOT_DONE);Permit();continue;
        }
        if (!close_clock()) anx_tx_unsupported("kernel clock close lost quiescence");
        finish();
    }
}
static UINT recover_manager(void)
{
    /* Bounded recovery: positively removed stable manager, empty domain. A
     * live caller/object/reservation refuses without mutating ownership. */
    if (tx_amiga_exec_task_alive(&manager) || task_stamp(&manager)!=manager_stamp) return TX_NOT_DONE;
    int registry=anx_tx_admission_check!=0;
    if (!anx_tx_runtime_retains_only(registry ? 2 :
        (clock_record.state==ANX_CLOCK_EMPTY || clock_record.state==ANX_CLOCK_REAPED ? 0 : 1))) return TX_NOT_DONE;
    if (registry && !anx_exec_callers_claim_recovery(&manager,1)) return TX_NOT_DONE;
    if (clock_record.state!=ANX_CLOCK_EMPTY && clock_record.state!=ANX_CLOCK_REAPED) {
        if (!anx_exec_clock_recover_creator(&clock_record,&manager)) {
            if (registry) (void)anx_exec_callers_claim_recovery(&manager,0);
            return TX_NOT_DONE;
        }
        if (registry && (!anx_exec_callers_recover_creator(&manager) || !anx_exec_callers_stop()))
            anx_tx_unsupported("kernel dead-manager registry transfer failed");
        if (!close_clock()) anx_tx_unsupported("kernel dead-manager clock retirement failed");
    }
    Forbid();release_stacks();phase=DOWN;Permit();return TX_SUCCESS;
}
static UINT request(unsigned operation)
{
    if (!tx_amiga_exec_task_context()) return TX_CALLER_ERROR;
    Forbid();
    if (command.owner && !command_live() && (command.done || !tx_amiga_exec_task_alive(&manager))) memset(&command,0,sizeof(command));
    if (command.owner) {Permit();return TX_NOT_DONE;}
    if (phase!=DOWN && !tx_amiga_exec_task_alive(&manager)) {
        phase=RECOVERING;command.owner=FindTask(0);command.stamp=task_stamp(command.owner);Permit();
        UINT result=recover_manager();Forbid();memset(&command,0,sizeof(command));
        if (result!=TX_SUCCESS) phase=RECOVERING;
        Permit();if (result!=TX_SUCCESS) return result;Forbid();
    }
    if (operation==START && phase==UP) {Permit();return TX_SUCCESS;}
    if (operation==STOP && phase==DOWN) {release_stacks();Permit();return TX_SUCCESS;}
    if ((operation==START && phase!=DOWN) || (operation==STOP && phase!=UP)) {Permit();return TX_NOT_DONE;}
    if (operation==START) {
        if (!anx_tx_runtime_resettable()) {Permit();return TX_NOT_DONE;}
        release_stacks();
        manager_stack=AllocMem(STACK_BYTES,MEMF_PUBLIC|MEMF_CLEAR);
        clock_stack=AllocMem(STACK_BYTES,MEMF_PUBLIC|MEMF_CLEAR);
        if (!manager_stack || !clock_stack) {release_stacks();Permit();return TX_NO_MEMORY;}
        memset(&manager,0,sizeof(manager));
        manager.tc_Node.ln_Type=NT_TASK;manager.tc_Node.ln_Name=(CHAR *)"Exec NetX management";
        manager.tc_SPLower=manager_stack;manager.tc_SPUpper=(UBYTE *)manager_stack+STACK_BYTES;
        manager.tc_SPReg=manager.tc_SPUpper;
        manager.tc_MemEntry.lh_Head=(struct Node *)&manager.tc_MemEntry.lh_Tail;
        manager.tc_MemEntry.lh_TailPred=(struct Node *)&manager.tc_MemEntry.lh_Head;
        manager_stamp=task_stamp(&manager);phase=STARTING;
    } else phase=STOPPING;
    BYTE bit=AllocSignal(-1);
    if (bit<0) {phase=operation==START ? DOWN : UP;if (operation==START) release_stacks();Permit();return TX_NO_MEMORY;}
    command.mask=1UL<<bit;command.signal=bit;(void)SetSignal(0,command.mask);
    command.owner=FindTask(0);command.stamp=task_stamp(command.owner);command.operation=operation;command.done=0;
    if (operation==START) {
        if (!AddTask(&manager,(APTR)entry,0)) {
            phase=DOWN;consume_command();release_stacks();Permit();return TX_NO_MEMORY;
        }
    } else Signal(&manager,SIGF_SINGLE);
    Permit();
    for (;;) {
        Forbid();
        if (command.owner!=FindTask(0)) anx_tx_unsupported("kernel command ownership changed");
        if (command.done) {
            UINT result=command.result;consume_command();
            if (phase==DOWN) release_stacks();
            Permit();return result;
        }
        if (!tx_amiga_exec_task_alive(&manager)) {
            phase=RECOVERING;Permit();UINT result=recover_manager();
            Forbid();consume_command();Permit();return result==TX_SUCCESS ? TX_NOT_DONE : result;
        }
        ULONG mask=command.mask;Permit();(void)Wait(mask);
    }
}
UINT tx_amiga_kernel_start(VOID) {return request(START);}
UINT tx_amiga_kernel_stop(VOID) {return request(STOP);}
UINT tx_amiga_kernel_running(VOID)
{
    UINT running;Forbid();running=phase==UP && tx_amiga_exec_task_alive(&manager);Permit();return running;
}
/* Public delete requires FINISHED, published with actual RemTask under one
 * Forbid; no live Task can outlast a publicly deleted control in this backend. */
ULONG tx_amiga_zombie_tasks(VOID) {return 0;}
ULONG tx_amiga_zombie_tasks_live(VOID) {return 0;}
VOID tx_amiga_tick_stats(TX_AMIGA_TICK_STATS *out)
{
    if (out) {Forbid();*out=clock_record.stats;Permit();}
}
TX_AMIGA_TICK_STATS *tx_amiga_tick_stats_live(VOID) {return &clock_record.stats;}
UINT tx_amiga_stack_in_use(const VOID *start,ULONG size)
{
    uintptr_t lo=(uintptr_t)start,hi=lo+size;
    if (!lo || hi<lo) return TX_TRUE;
    Forbid();
    int used=(manager_stack && lo<=(uintptr_t)manager_stack+STACK_BYTES && (uintptr_t)manager_stack<=hi) ||
        (clock_stack && lo<=(uintptr_t)clock_stack+STACK_BYTES && (uintptr_t)clock_stack<=hi);
    if (!used) used=anx_exec_thread_stack_in_use(start,size);
    Permit();return used ? TX_TRUE : TX_FALSE;
}
void anx_exec_kernel_snapshot(AnxKernelSnapshot *out)
{
    if (out) {Forbid();*out=(AnxKernelSnapshot){&manager,&clock_record.task,manager_stack,clock_stack,STACK_BYTES,phase};Permit();}
}
