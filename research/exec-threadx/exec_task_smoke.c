/* Disposable native task lifetime experiment. SPDX-License-Identifier: MIT */
#include "exec_task.h"
#include "tx_bridge_exec.h"
#include "tx_thread.h"
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>

typedef struct { ULONG before; ULONG bytes[8192/sizeof(ULONG)]; ULONG after; } Stack;
static Stack stacks[2];
static AnxExecTask children[2];
static TX_THREAD parent_thread;
static AnxTxThread parent_bridge;
static AnxExecWait parent_wait;
static TX_EVENT_FLAGS_GROUP events;
static volatile unsigned entries[2],returned[2],entry_error;
static unsigned passed,reaped;

static void say(const char *s)
{
    const char *end=s;
    while (*end) end++;
    (void)Write(Output(),(APTR)s,(LONG)(end-s));
    (void)Flush(Output());
}
#define CHECK(x) do { if (!(x)) {say("research_exec_task=FAIL " #x "\n"); return 20;} } while (0)
#define CASE(s) do {passed++;say("research_exec_task=CASE_PASS " s "\n");} while (0)

static VOID child(ULONG input)
{
    ULONG actual=0,bit;
    AnxExecTask *r;
    if (input>1) {entry_error=1;return;}
    bit=1UL<<input; r=&children[input];
    if (tx_thread_identify()!=&r->thread ||
        r->thread.tx_thread_entry!=child || r->thread.tx_thread_entry_parameter!=input ||
        r->thread.tx_thread_stack_start!=stacks[input].bytes ||
        r->thread.tx_thread_stack_size!=8192 ||
        (uintptr_t)&actual<(uintptr_t)r->stack ||
        (uintptr_t)&actual>=(uintptr_t)r->stack+r->stack_size)
        entry_error=1;
    entries[input]++;
    if (tx_event_flags_set(&events,bit,TX_OR)!=TX_SUCCESS ||
        tx_event_flags_get(&events,bit<<8,TX_OR_CLEAR,&actual,TX_WAIT_FOREVER)!=TX_SUCCESS ||
        !(actual&(bit<<8)))
        entry_error=1;
    returned[input]++;
}

static VOID immediate(ULONG input)
{
    if (input!=42 || tx_thread_identify()!=&children[0].thread) entry_error=1;
}

static int wait_ready(unsigned mask)
{
    AnxTxContext f;
    ULONG flags=0;
    UINT status;
    anx_tx_context_begin(&f,&parent_thread,0);
    status=tx_event_flags_get(&events,mask,TX_AND_CLEAR,&flags,50);
    anx_tx_context_end(&f);
    return status==TX_SUCCESS && (flags&mask)==mask;
}

static void release(unsigned mask)
{
    AnxTxContext f;
    anx_tx_context_begin(&f,&parent_thread,0);
    if (tx_event_flags_set(&events,mask<<8,TX_OR)!=TX_SUCCESS) entry_error=1;
    anx_tx_context_end(&f);
}

static int recovered(unsigned i)
{
    AnxExecTask *r=&children[i];
    return r->state==ANX_TASK_REAPED && !r->thread.tx_thread_id &&
        !r->wait.opened && !r->creator && !r->ack && r->signal==-1 &&
        !r->thread.tx_thread_suspend_cleanup && !r->bridge.abort_pins &&
        !r->thread.tx_thread_owned_mutex_count &&
        !r->thread.tx_thread_timer.tx_timer_internal_list_head &&
        stacks[i].before==0x13572468 && stacks[i].after==0x89abcdef;
}

int main(void)
{
    AnxTxContext f;
    ULONG initial_signals;
    unsigned i;
    BYTE held[32], bit,old_priority;
    unsigned held_count=0;
    say("research_exec_task=START\n");
    anx_tx_runtime_init(anx_tx_exec_platform());
    CHECK(anx_exec_wait_open(&parent_wait));
    CHECK(anx_tx_attach(&parent_bridge,&parent_thread,&parent_wait.wait,(uintptr_t)FindTask(0)));
    anx_tx_context_begin(&f,&parent_thread,0);
    CHECK(tx_event_flags_create(&events,(CHAR *)"lifecycle events")==TX_SUCCESS);
    anx_tx_context_end(&f);
    initial_signals=FindTask(0)->tc_SigAlloc;
    for (i=0;i<2;i++) {
        stacks[i].before=0x13572468; stacks[i].after=0x89abcdef;
        memset(stacks[i].bytes,0xa5,sizeof(stacks[i].bytes));
    }
    CHECK(!anx_exec_task_start(0,(CHAR *)"bad",child,0,stacks[0].bytes,8192));
    CHECK(!anx_exec_task_start(&children[0],(CHAR *)"bad",child,0,(UBYTE *)stacks[0].bytes+1,8192));
    CHECK(!anx_exec_task_start(&children[0],(CHAR *)"bad",child,0,stacks[0].bytes,4096));
    CHECK(!anx_exec_task_start(&children[0],(CHAR *)"bad",child,0,&children[0],8192));
    CHECK(children[0].state==ANX_TASK_EMPTY && FindTask(0)->tc_SigAlloc==initial_signals);
    CASE("invalid-storage-no-publication");
    while ((bit=AllocSignal(-1))>=0) held[held_count++]=bit;
    CHECK(!anx_exec_task_start(&children[0],(CHAR *)"no ACK signal",child,0,stacks[0].bytes,8192));
    CHECK(children[0].state==ANX_TASK_EMPTY && !children[0].creator);
    while (held_count) FreeSignal(held[--held_count]);
    CHECK(FindTask(0)->tc_SigAlloc==initial_signals);
    CASE("startup-ack-allocation-failure-rollback");
    Forbid();
    CHECK(!anx_exec_task_start(&children[0],(CHAR *)"bad",child,0,stacks[0].bytes,8192));
    Permit();
    anx_tx_context_begin(&f,&parent_thread,0);
    CHECK(!anx_exec_task_start(&children[0],(CHAR *)"bad",child,0,stacks[0].bytes,8192));
    anx_tx_context_end(&f);
    CHECK(children[0].state==ANX_TASK_EMPTY && FindTask(0)->tc_SigAlloc==initial_signals);
    CASE("blocking-boundary-rejected");
    CHECK(anx_exec_task_start(&children[0],(CHAR *)"owned child0",child,0,stacks[0].bytes,8192));
    CHECK(wait_ready(1));
    CHECK(children[0].started && entries[0]==1 && !entry_error && children[0].thread.tx_thread_id==TX_THREAD_ID);
    CHECK(children[0].task.tc_UserData==&children[0] && children[0].task.tc_SPLower==stacks[0].bytes &&
          children[0].task.tc_SPUpper==(UBYTE *)stacks[0].bytes+8192);
    CASE("startup-identity-stack-metadata");
    CHECK(!anx_exec_task_reap(&children[0]));
    CHECK(!anx_exec_task_start(&children[0],(CHAR *)"duplicate",child,0,stacks[0].bytes,8192));
    CHECK(children[0].creator==FindTask(0) && children[0].state==ANX_TASK_RUNNING);
    CASE("live-reap-and-duplicate-rejected");
    CHECK(anx_exec_task_start(&children[1],(CHAR *)"owned child1",child,1,stacks[1].bytes,8192));
    CHECK(wait_ready(2));
    CHECK(entries[1]==1 && !entry_error);
    CASE("second-independent-task");
    anx_tx_context_begin(&f,&parent_thread,0);
    CHECK(!anx_exec_task_join(&children[0]) && !anx_exec_task_reap(&children[0]));
    anx_tx_context_end(&f);
    CASE("join-boundary-rejected");
    release(3);
    CHECK(anx_exec_task_join(&children[0])); reaped++;
    CHECK(anx_exec_task_join(&children[1])); reaped++;
    CHECK(recovered(0) && recovered(1) && returned[0]==1 && returned[1]==1 && !entry_error);
    CHECK(FindTask(0)->tc_SigAlloc==initial_signals && anx_tx_runtime_idle());
    CASE("normal-return-detach-private-io-reap");
    CHECK(!anx_exec_task_join(&children[0]) && !anx_exec_task_reap(&children[0]));
    CASE("double-reap-rejected");
    for (i=0;i<12;i++) {
        CHECK(anx_exec_task_start(&children[0],(CHAR *)"restarted child",child,0,stacks[0].bytes,8192));
        CHECK(wait_ready(1)); release(1);
        CHECK(anx_exec_task_join(&children[0])); reaped++;
        CHECK(recovered(0) && !entry_error && FindTask(0)->tc_SigAlloc==initial_signals);
    }
    CHECK(entries[0]==13 && returned[0]==13);
    CASE("twelve-restarts-signal-and-stack-recovery");
    /* Higher-priority child finishes before the creator can observe startup.
     * This tests retained ACK/terminal publication, not ThreadX priorities. */
    old_priority=SetTaskPri(FindTask(0),-1);
    CHECK(anx_exec_task_start(&children[0],(CHAR *)"immediate child",immediate,42,stacks[0].bytes,8192));
    CHECK(children[0].started && children[0].state==ANX_TASK_FINISHED);
    (void)SetTaskPri(FindTask(0),old_priority);
    CHECK(anx_exec_task_join(&children[0])); reaped++;
    CHECK(recovered(0) && !entry_error && FindTask(0)->tc_SigAlloc==initial_signals);
    CASE("completion-before-startup-observation");
    anx_tx_context_begin(&f,&parent_thread,0);
    CHECK(tx_event_flags_delete(&events)==TX_SUCCESS);
    anx_tx_context_end(&f);
    CHECK(anx_tx_detach(&parent_bridge) && anx_exec_wait_close(&parent_wait));
    CHECK(anx_tx_runtime_idle() && passed==11 && reaped==15);
    say("research_exec_task=PASS 11/11 tasks_reaped=15 restarts=12\n");
    return 0;
}
