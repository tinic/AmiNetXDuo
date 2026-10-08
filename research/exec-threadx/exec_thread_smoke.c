/* Native reserved-worker/public service boundary proof. SPDX-License-Identifier: MIT */
#include "exec_thread.h"
#include "tx_bridge_exec.h"
#include "tx_thread.h"
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>

typedef struct {ULONG before,bytes[2048],after;} Stack;
static Stack stacks[2];
static AnxExecThread records[2],unused;
static TX_THREAD children[2],absent,parent_thread,snapshot;
static AnxTxThread parent_bridge;
static AnxExecWait parent_wait;
static TX_EVENT_FLAGS_GROUP events;
static volatile unsigned entries[2],cookie[2],entry_error;
static UINT foreign_delete;
static unsigned passed,reaped;
static UINT expected_priority[2]={16,17};
static ULONG expected_slice[2];
static unsigned policy_probe;
static volatile unsigned policy_phase;

static void say(const char *s)
{
    const char *e=s;while (*e) e++;
    (void)Write(Output(),(APTR)s,(LONG)(e-s));(void)Flush(Output());
}
#define CHECK(x) do {if (!(x)) {say("research_exec_thread=FAIL " #x "\n");return 20;}} while (0)
#define CASE(s) do {passed++;say("research_exec_thread=CASE_PASS " s "\n");} while (0)
static VOID child(ULONG input)
{
    ULONG actual=0;
    TX_THREAD *t;
    UINT old_threshold=99,restored=99;
    ULONG old_slice=99;
    if (input>1) {entry_error=1;return;}
    t=&children[input];
    if (tx_thread_identify()!=t || !cookie[input] || t->tx_thread_id!=TX_THREAD_ID ||
        t->tx_thread_entry!=child || t->tx_thread_entry_parameter!=input ||
        t->tx_thread_stack_start!=stacks[input].bytes || t->tx_thread_stack_size!=8192 ||
        t->tx_thread_priority!=expected_priority[input] || t->tx_thread_preempt_threshold!=expected_priority[input] ||
        t->tx_thread_time_slice!=expected_slice[input] || t->tx_thread_new_time_slice!=expected_slice[input] ||
        records[input].task.tc_Node.ln_Pri!=ANX_THREAD_EXEC_PRIORITY(expected_priority[input]) ||
        records[input].task.tc_Node.ln_Pri>TX_AMIGA_TASK_PRIORITY ||
        (uintptr_t)&actual<(uintptr_t)t->tx_thread_stack_start ||
        (uintptr_t)&actual>(uintptr_t)t->tx_thread_stack_end) entry_error=1;
    entries[input]++;
    if (policy_probe) {
        if (tx_thread_preemption_change(t,0,&old_threshold)!=TX_SUCCESS || old_threshold!=2 ||
            tx_thread_time_slice_change(t,3,&old_slice)!=TX_SUCCESS || old_slice!=1 ||
            tx_thread_time_slice_change(t,1,&old_slice)!=TX_SUCCESS || old_slice!=3)
            entry_error=1;
        policy_phase=1;
    }
    if (tx_event_flags_set(&events,1UL<<input,TX_OR)!=TX_SUCCESS ||
        tx_event_flags_get(&events,256UL<<input,TX_OR_CLEAR,&actual,TX_WAIT_FOREVER)!=TX_SUCCESS ||
        !(actual&(256UL<<input))) entry_error=1;
    if (policy_probe) {
        if (policy_phase!=2 || tx_thread_identify()!=t || t->tx_thread_preempt_threshold!=0 ||
            tx_thread_preemption_change(t,old_threshold,&restored)!=TX_SUCCESS || restored!=0)
            entry_error=1;
        policy_phase=3;
    }
    if (!input) foreign_delete=tx_thread_delete(&children[1]);
}
static int ready(unsigned mask)
{
    AnxTxContext f; ULONG actual=0; UINT s;
    anx_tx_context_begin(&f,&parent_thread,0);
    s=tx_event_flags_get(&events,mask,TX_AND_CLEAR,&actual,50);
    anx_tx_context_end(&f);
    return s==TX_SUCCESS && (actual&mask)==mask;
}
static int release(unsigned mask)
{
    AnxTxContext f; UINT s;
    anx_tx_context_begin(&f,&parent_thread,0);
    s=tx_event_flags_set(&events,mask<<8,TX_OR);
    if (policy_probe) {
        if (policy_phase!=1 || children[0].tx_thread_preempt_threshold!=0) entry_error=1;
        policy_phase=2; /* actual wake cannot enter before producer Permit */
    }
    anx_tx_context_end(&f);
    return s==TX_SUCCESS;
}
static UINT create(unsigned i,UINT start)
{
    return tx_thread_create(&children[i],(CHAR *)"public child",child,i,stacks[i].bytes,
                             8192,expected_priority[i],expected_priority[i],expected_slice[i],start);
}
static int completed(unsigned i)
{
    return records[i].state==ANX_THREAD_FINISHED && children[i].tx_thread_state==TX_COMPLETED &&
        children[i].tx_thread_id==TX_THREAD_ID && !records[i].wait.opened &&
        !children[i].tx_thread_suspend_cleanup && !records[i].bridge.abort_pins &&
        !children[i].tx_thread_owned_mutex_count &&
        !children[i].tx_thread_timer.tx_timer_internal_list_head;
}
static int recovered(unsigned i)
{
    return records[i].state==ANX_THREAD_REAPED && !children[i].tx_thread_id &&
        !records[i].wait.opened && !records[i].creator && !records[i].ack && records[i].signal==-1 &&
        !children[i].tx_thread_amiga_task && !children[i].tx_thread_created_next &&
        stacks[i].before==0x13572468 && stacks[i].after==0x89abcdef;
}
int main(void)
{
    AnxTxContext f,nested;
    ULONG signals;
    unsigned i;
    BYTE held[32],bit; unsigned count=0;
    UINT old_threshold=99,restored=99;
    say("research_exec_thread=START\n");
    anx_tx_runtime_init(anx_tx_exec_platform());
    CHECK(anx_exec_wait_open(&parent_wait));
    CHECK(anx_tx_attach(&parent_bridge,&parent_thread,&parent_wait.wait,(uintptr_t)FindTask(0)));
    anx_tx_context_begin(&f,&parent_thread,0);
    parent_thread.tx_thread_priority=parent_thread.tx_thread_user_priority=16;
    parent_thread.tx_thread_preempt_threshold=parent_thread.tx_thread_user_preempt_threshold=16;
    CHECK(tx_event_flags_create(&events,(CHAR *)"public lifetime")==TX_SUCCESS);
    memset(&absent,0xa5,sizeof(absent)); snapshot=absent;
    CHECK(tx_thread_create(&absent,(CHAR *)"no reservation",child,0,stacks[0].bytes,8192,16,16,0,TX_AUTO_START)==TX_NO_MEMORY);
    CHECK(!memcmp(&absent,&snapshot,sizeof(absent)) && !_tx_thread_created_count);
    anx_tx_context_end(&f);
    CASE("unreserved-create-no-publication");
    signals=FindTask(0)->tc_SigAlloc;
    for (i=0;i<2;i++) {
        stacks[i].before=0x13572468;stacks[i].after=0x89abcdef;
        memset(&children[i],0xa5,sizeof(children[i]));
    }
    snapshot=children[0];
    while ((bit=AllocSignal(-1))>=0) held[count++]=bit;
    CHECK(!anx_exec_thread_prepare(&records[0],&children[0],(CHAR *)"no ACK",stacks[0].bytes,8192));
    CHECK(records[0].state==ANX_THREAD_EMPTY && !memcmp(&snapshot,&children[0],sizeof(snapshot)));
    while (count) FreeSignal(held[--count]);
    CHECK(FindTask(0)->tc_SigAlloc==signals);
    CASE("reservation-ack-failure-rollback");
    CHECK(anx_exec_thread_prepare(&records[0],&children[0],(CHAR *)"reserved0",stacks[0].bytes,8192));
    CHECK(records[0].state==ANX_THREAD_PREPARED && records[0].wait.opened &&
          !memcmp(&snapshot,&children[0],sizeof(snapshot)) && !entries[0]);
    CHECK(!anx_exec_thread_prepare(&unused,&children[0],(CHAR *)"duplicate target",stacks[1].bytes,8192));
    CHECK(!anx_exec_thread_prepare(&unused,&absent,(CHAR *)"overlap stack",stacks[0].bytes,8192));
    CASE("prepared-io-without-public-control-mutation");
    anx_tx_context_begin(&f,&parent_thread,0);
    CHECK(!anx_exec_thread_prepare(&records[1],&children[1],(CHAR *)"nested prepare",stacks[1].bytes,8192));
    CHECK(!anx_exec_thread_cancel(&records[0]));
    CHECK(tx_thread_create(&children[0],(CHAR *)"invalid threshold",child,0,stacks[0].bytes,8192,16,17,1,TX_AUTO_START)==TX_THRESH_ERROR);
    CHECK(tx_thread_create(&children[0],(CHAR *)"unsupported threshold",child,0,stacks[0].bytes,8192,16,15,0,TX_AUTO_START)==TX_FEATURE_NOT_ENABLED);
    CHECK(tx_thread_create(&children[0],(CHAR *)"wrong stack",child,0,stacks[1].bytes,8192,16,16,0,TX_AUTO_START)==TX_SIZE_ERROR);
    CHECK(!memcmp(&snapshot,&children[0],sizeof(snapshot)) && records[0].state==ANX_THREAD_PREPARED);
    CASE("unsupported-options-before-control-mutation");
    anx_tx_context_begin(&nested,&parent_thread,0);
    CHECK(create(0,TX_AUTO_START)==TX_SUCCESS);
    CHECK(children[0].tx_thread_id==TX_THREAD_ID && children[0].tx_thread_state==TX_READY && !entries[0]);
    cookie[0]=0x1234;
    anx_tx_context_end(&nested);
    CHECK(!entries[0] && _tx_thread_created_count==1 && _tx_thread_created_ptr==&children[0]);
    CHECK(tx_thread_delete(&children[0])==TX_DELETE_ERROR);
    anx_tx_context_end(&f);
    CHECK(ready(1) && entries[0]==1 && !entry_error);
    CASE("nested-auto-start-publication-before-entry");
    CHECK(anx_exec_thread_prepare(&records[1],&children[1],(CHAR *)"reserved1",stacks[1].bytes,8192));
    anx_tx_context_begin(&f,&parent_thread,0);
    CHECK(create(1,TX_DONT_START)==TX_SUCCESS);
    CHECK(children[1].tx_thread_state==TX_SUSPENDED && !entries[1] && _tx_thread_created_count==2);
    CHECK(children[0].tx_thread_created_next==&children[1] && children[1].tx_thread_created_next==&children[0]);
    CHECK(tx_thread_delete(&children[1])==TX_DELETE_ERROR);
    anx_tx_context_end(&f);
    anx_tx_context_begin(&f,&parent_thread,0);
    CHECK(tx_thread_sleep(2)==TX_SUCCESS);
    CHECK(!entries[1] && children[1].tx_thread_state==TX_SUSPENDED);
    CHECK(tx_thread_resume(&children[1])==TX_SUCCESS);
    cookie[1]=0x5678;
    CHECK(!entries[1] && tx_thread_resume(&children[1])==TX_RESUME_ERROR);
    anx_tx_context_end(&f);
    CHECK(ready(2) && entries[1]==1 && !entry_error);
    CASE("dont-start-explicit-resume-retained-gate");
    anx_tx_context_begin(&f,&parent_thread,0);
    CHECK(tx_thread_resume(&children[0])==TX_FEATURE_NOT_ENABLED);
    CHECK(!anx_exec_thread_wait(&records[0]) && !anx_exec_thread_cancel(&records[0]));
    anx_tx_context_end(&f);
    CASE("general-resume-and-blocking-lifecycle-rejected");
    CHECK(release(3));
    CHECK(anx_exec_thread_wait(&records[0]) && anx_exec_thread_wait(&records[1]));
    CHECK(completed(0) && completed(1) && !entry_error && foreign_delete==TX_CALLER_ERROR);
    CHECK(_tx_thread_created_count==2);
    CASE("normal-completed-id-private-io-native-retirement");
    anx_tx_context_begin(&f,&parent_thread,0);
    CHECK(tx_thread_delete(&children[0])==TX_SUCCESS);reaped++;
    CHECK(_tx_thread_created_count==1 && _tx_thread_created_ptr==&children[1] && children[1].tx_thread_created_next==&children[1]);
    CHECK(tx_thread_delete(&children[1])==TX_SUCCESS);reaped++;
    CHECK(tx_thread_delete(&children[0])==TX_THREAD_ERROR);
    anx_tx_context_end(&f);
    CHECK(recovered(0) && recovered(1) && !_tx_thread_created_count && !_tx_thread_created_ptr && FindTask(0)->tc_SigAlloc==signals);
    CASE("public-delete-list-signal-and-stack-recovery");
    for (i=0;i<6;i++) {
        cookie[0]=0;
        CHECK(anx_exec_thread_prepare(&records[0],&children[0],(CHAR *)"reused",stacks[0].bytes,8192));
        anx_tx_context_begin(&f,&parent_thread,0);
        CHECK(create(0,TX_AUTO_START)==TX_SUCCESS);cookie[0]=0x1234;
        anx_tx_context_end(&f);
        CHECK(ready(1) && release(1) && anx_exec_thread_wait(&records[0]) && completed(0));
        anx_tx_context_begin(&f,&parent_thread,0);
        CHECK(tx_thread_delete(&children[0])==TX_SUCCESS);reaped++;
        anx_tx_context_end(&f);
        CHECK(recovered(0) && !entry_error && !_tx_thread_created_count && FindTask(0)->tc_SigAlloc==signals);
    }
    CHECK(entries[0]==7 && entries[1]==1);
    CASE("six-public-create-complete-delete-restarts");
    expected_priority[0]=2; expected_slice[0]=1; cookie[0]=0; policy_probe=1;
    CHECK(anx_exec_thread_prepare(&records[0],&children[0],(CHAR *)"IP priority band",stacks[0].bytes,8192));
    anx_tx_context_begin(&f,&parent_thread,0);
    CHECK(tx_thread_preemption_change(&parent_thread,2,&old_threshold)==TX_SUCCESS && old_threshold==16);
    CHECK(tx_thread_preemption_change(&parent_thread,17,&restored)==TX_THRESH_ERROR && restored==99 &&
          parent_thread.tx_thread_preempt_threshold==2);
    anx_tx_context_begin(&nested,&parent_thread,0);
    CHECK(create(0,TX_AUTO_START)==TX_SUCCESS);cookie[0]=0x1234;
    CHECK(records[0].task.tc_Node.ln_Pri==TX_AMIGA_TASK_PRIORITY && !records[0].entered &&
          children[0].tx_thread_time_slice==1 && children[0].tx_thread_new_time_slice==1);
    anx_tx_context_end(&nested);
    CHECK(!records[0].entered && tx_thread_preemption_change(&children[0],0,&restored)==TX_FEATURE_NOT_ENABLED);
    CHECK(tx_thread_preemption_change(&parent_thread,old_threshold,&restored)==TX_SUCCESS && restored==2);
    CHECK(!records[0].entered && parent_thread.tx_thread_preempt_threshold==16);
    anx_tx_context_end(&f);
    CHECK(ready(1) && release(1) && anx_exec_thread_wait(&records[0]) && completed(0));
    anx_tx_context_begin(&f,&parent_thread,0);
    CHECK(tx_thread_delete(&children[0])==TX_SUCCESS);reaped++;
    anx_tx_context_end(&f);
    CHECK(recovered(0) && entries[0]==8 && !entry_error && policy_phase==3 && FindTask(0)->tc_SigAlloc==signals);
    CASE("IP-helper-logical-priority-stays-in-safe-Exec-band");
    CASE("creator-threshold-restored-before-advisory-slice-helper-entry");
    CASE("raised-threshold-event-suspend-resume-restores-boundary-and-threshold");
    policy_probe=0;
    snapshot=children[0];
    CHECK(anx_exec_thread_prepare(&records[0],&children[0],(CHAR *)"cancel unbound",stacks[0].bytes,8192));
    CHECK(anx_exec_thread_cancel(&records[0]));reaped++;
    CHECK(!anx_exec_thread_cancel(&records[0]) && !records[0].wait.opened && records[0].state==ANX_THREAD_REAPED);
    CHECK(!memcmp(&snapshot,&children[0],sizeof(snapshot)) && FindTask(0)->tc_SigAlloc==signals);
    CHECK(stacks[0].before==0x13572468 && stacks[0].after==0x89abcdef);
    CASE("unbound-cancel-reaps-without-public-mutation");
    anx_tx_context_begin(&f,&parent_thread,0);
    CHECK(tx_event_flags_delete(&events)==TX_SUCCESS);
    anx_tx_context_end(&f);
    CHECK(anx_tx_detach(&parent_bridge) && anx_exec_wait_close(&parent_wait));
    anx_tx_runtime_init(anx_tx_exec_platform()); /* all reservations really released */
    CHECK(passed==14 && reaped==10 && !events.tx_event_flags_group_suspended_count);
    say("research_exec_thread=PASS 14/14 tasks_reaped=10 restarts=6\n");
    return 0;
}
