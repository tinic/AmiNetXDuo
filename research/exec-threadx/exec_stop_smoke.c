/* Native owner stop from real event suspension; no full IP helper claim.
 * SPDX-License-Identifier: MIT */
#include "exec_thread.h"
#include "tx_bridge_exec.h"
#include "tx_thread.h"
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
typedef struct {ULONG before,bytes[2048],after;} Stack;
static Stack stack;
static AnxExecThread record;
static TX_THREAD target,parent,snapshot;
static AnxTxThread pb;
static AnxExecWait pw;
static TX_EVENT_FLAGS_GROUP events,wrong;
static TX_MUTEX mutex;
static volatile unsigned entries,returned,error;
static ULONG wait_ticks;
static unsigned passed,reaped,timed;
static void say(const char *s)
{
    const char *e=s;while (*e) e++;
    (void)Write(Output(),(APTR)s,(LONG)(e-s));(void)Flush(Output());
}
#define CHECK(x) do {if (!(x)) {say("research_exec_stop=FAIL " #x "\n");return 20;}} while (0)
#define CASE(s) do {passed++;say("research_exec_stop=CASE_PASS " s "\n");} while (0)
static VOID child(ULONG input)
{
    ULONG actual=0;
    (void)input;entries++;
    if (anx_exec_thread_stop_event(&record,&events) ||
        tx_mutex_get(&mutex,TX_NO_WAIT)!=TX_SUCCESS ||
        tx_event_flags_set(&events,1,TX_OR)!=TX_SUCCESS ||
        tx_event_flags_get(&events,256,TX_OR_CLEAR,&actual,TX_WAIT_FOREVER)!=TX_SUCCESS ||
        tx_mutex_put(&mutex)!=TX_SUCCESS || tx_event_flags_set(&events,2,TX_OR)!=TX_SUCCESS)
        error=1;
    (void)tx_event_flags_get(&events,512,TX_OR_CLEAR,&actual,wait_ticks);
    returned++; /* stop must bypass this statement and the remaining NetX frame */
}
static int ready(ULONG flags)
{
    AnxTxContext f;ULONG actual=0;UINT s;
    anx_tx_context_begin(&f,&parent,0);
    s=tx_event_flags_get(&events,flags,TX_AND_CLEAR,&actual,50);
    anx_tx_context_end(&f);
    return s==TX_SUCCESS && actual==flags;
}
int main(void)
{
    AnxTxContext f,nested;
    ULONG signals;
    unsigned cycle;
    say("research_exec_stop=START\n");
    anx_tx_runtime_init(anx_tx_exec_platform());
    CHECK(anx_exec_wait_open(&pw) && anx_tx_attach(&pb,&parent,&pw.wait,(uintptr_t)FindTask(0)));
    signals=FindTask(0)->tc_SigAlloc;
    stack.before=0x13572468;stack.after=0x89abcdef;
    anx_tx_context_begin(&f,&parent,0);
    CHECK(tx_event_flags_create(&events,(CHAR *)"stop")==TX_SUCCESS &&
          tx_event_flags_create(&wrong,(CHAR *)"wrong")==TX_SUCCESS &&
          tx_mutex_create(&mutex,(CHAR *)"owned",TX_NO_INHERIT)==TX_SUCCESS);
    anx_tx_context_end(&f);
    for (cycle=0;cycle<4;cycle++) {
        wait_ticks=(cycle&1)?500:TX_WAIT_FOREVER;
        memset(&target,0xa5,sizeof(target));memset(stack.bytes,0xa5,sizeof(stack.bytes));
        CHECK(anx_exec_thread_prepare(&record,&target,(CHAR *)"stop worker",stack.bytes,8192));
        anx_tx_context_begin(&f,&parent,0);
        CHECK(tx_thread_create(&target,(CHAR *)"stop worker",child,0,stack.bytes,8192,2,2,1,TX_AUTO_START)==TX_SUCCESS);
        CHECK(!anx_exec_thread_stop_event(&record,&events) && !record.entered);
        anx_tx_context_end(&f);
        CHECK(ready(1));
        anx_tx_context_begin(&f,&parent,0);
        snapshot=target;
        CHECK(target.tx_thread_owned_mutex_count==1 && !anx_exec_thread_stop_event(&record,&events) &&
              !memcmp(&target,&snapshot,sizeof(target)));
        CHECK(tx_event_flags_set(&events,256,TX_OR)==TX_SUCCESS);
        anx_tx_context_end(&f);
        CHECK(ready(2));
        if (cycle&1) {
            anx_tx_context_begin(&f,&parent,0);
            CHECK(tx_thread_sleep(1)==TX_SUCCESS); /* allow actual timer submission before stop */
            anx_tx_context_end(&f);
        }
        anx_tx_context_begin(&f,&parent,0);
        CHECK(target.tx_thread_state==TX_EVENT_FLAG && !target.tx_thread_owned_mutex_count &&
              events.tx_event_flags_group_suspended_count==1 && !error && !returned);
        if (cycle&1) {
            CHECK(record.wait.timer_sends==1 && record.wait.timer_reaps==0 &&
                  !CheckIO((struct IORequest *)record.wait.timer));timed++;
        }
        snapshot=target;
        CHECK(!anx_exec_thread_stop_event(&record,&wrong) && !memcmp(&target,&snapshot,sizeof(target)));
        record.bridge.abort_pins=1;CHECK(!anx_exec_thread_stop_event(&record,&events));record.bridge.abort_pins=0;
        record.bridge.pending_resume=1;CHECK(!anx_exec_thread_stop_event(&record,&events));record.bridge.pending_resume=0;
        anx_tx_context_begin(&nested,&parent,0);
        CHECK(!anx_exec_thread_stop_event(&record,&events));anx_tx_context_end(&nested);
        CHECK(!memcmp(&target,&snapshot,sizeof(target)));
        _tx_thread_preempt_disable++;
        CHECK(anx_exec_thread_stop_event(&record,&events));
        CHECK(_tx_thread_preempt_disable==1 && target.tx_thread_id==TX_THREAD_ID &&
              target.tx_thread_state==TX_TERMINATED && !record.bridge.thread &&
              record.state==ANX_THREAD_STOPPING && record.wait.opened &&
              !events.tx_event_flags_group_suspended_count && !events.tx_event_flags_group_suspension_list &&
              !target.tx_thread_suspend_cleanup && !target.tx_thread_timer.tx_timer_internal_list_head);
        CHECK(!anx_exec_thread_stop_event(&record,&events) && tx_thread_delete(&target)==TX_DELETE_ERROR);
        _tx_thread_preempt_disable--;
        anx_tx_context_end(&f);
        CHECK(anx_exec_thread_wait(&record) && record.state==ANX_THREAD_FINISHED && !record.wait.opened &&
              record.wait.timer_sends==record.wait.timer_reaps && !returned && !error);
        anx_tx_context_begin(&f,&parent,0);
        CHECK(tx_thread_delete(&target)==TX_SUCCESS);reaped++;
        anx_tx_context_end(&f);
        CHECK(record.state==ANX_THREAD_REAPED && !target.tx_thread_id && !_tx_thread_created_count &&
              !record.creator && !record.ack && record.signal==-1 && FindTask(0)->tc_SigAlloc==signals &&
              stack.before==0x13572468 && stack.after==0x89abcdef);
        memset(&target,0x5a,sizeof(target));memset(stack.bytes,0x5a,sizeof(stack.bytes));
        if (!cycle) {
            CASE("ready-and-self-stop-refused-without-publication");
            CASE("owned-mutex-event-stop-refused-without-mutation");
            CASE("wrong-object-pin-pending-wake-and-nested-stop-refused");
            CASE("real-event-cleanup-and-unlink-under-preemption-disable");
            CASE("public-delete-refused-before-native-finished");
            CASE("private-owner-close-and-no-return-into-entry");
        }
    }
    CHECK(entries==4 && reaped==4 && timed==2 && !returned);
    CASE("two-inflight-timer-stops-reap-device-io-before-close");
    CASE("four-stop-delete-cycles-poisoned-storage-reused");
    CHECK(anx_tx_detach(&pb) && anx_exec_wait_close(&pw));
    anx_tx_runtime_init(anx_tx_exec_platform());
    CASE("all-domain-reservations-signals-and-task-storage-recovered");
    CHECK(passed==9);
    say("research_exec_stop=PASS 9/9 tasks_reaped=4 timed_io_reaped=2 restarts=3\n");
    return 0;
}
