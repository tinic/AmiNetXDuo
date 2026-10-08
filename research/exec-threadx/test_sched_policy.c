/* Bounded logical scheduling policy and real event suspension; not Exec ABI.
 * SPDX-License-Identifier: MIT */
#include "tx_bridge.h"
#include "tx_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if (!(x)) {fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}} while (0)
static unsigned depth,phase;
static uintptr_t owner=1;
static const char *reject;
static TX_THREAD thread[2];
static AnxTxThread bridge[2];
static AnxWait waits[2];
static TX_EVENT_FLAGS_GROUP events;
static void enter(void *p) {(void)p;depth++;}
static void leave(void *p) {(void)p;CHECK(depth);depth--;}
static uintptr_t caller(void *p) {(void)p;return owner;}
static void panic(void *p,const char *s)
{
    (void)p;
    if (reject && !strcmp(s,reject)) {
        CHECK(thread[0].tx_thread_preempt_threshold==2 &&
              thread[0].tx_thread_user_preempt_threshold==2);
        puts("research_sched_guard=PASS rejected without releasing raised boundary");exit(0);
    }
    fprintf(stderr,"panic %s\n",s);exit(1);
}
static uint64_t now(void *p) {(void)p;CHECK(depth);return 0;}
static void notify(void *p) {(void)p;CHECK(depth);}
static int park(void *p,uint64_t deadline)
{
    AnxTxContext f;
    (void)p;CHECK(deadline==ANX_WAIT_FOREVER && !depth && !_tx_thread_current_ptr);
    CHECK(thread[0].tx_thread_preempt_threshold==2 && phase==1);
    owner=2;anx_tx_context_begin(&f,&thread[1],0);
    CHECK(_tx_event_flags_set(&events,1,TX_OR)==TX_SUCCESS);
    /* Producer retains its own serialized boundary after waking the sender.
     * There is no claim that sender wins an Exec priority dispatch race. */
    CHECK(phase==1);phase=2;
    anx_tx_context_end(&f);owner=1;return 1;
}
int main(int argc,char **argv)
{
    AnxTxPlatform p={enter,leave,caller,panic,0,0};
    AnxWaitOps ops={enter,leave,now,park,notify,0};
    AnxTxContext f,nested;
    UINT old=99,invalid_old=99;
    ULONG actual=0,old_slice=99;
    TX_THREAD absent;
    anx_tx_runtime_init(&p);
    for (unsigned i=0;i<2;i++) {
        owner=i+1;anx_wait_init(&waits[i],&ops);
        CHECK(anx_tx_attach(&bridge[i],&thread[i],&waits[i],owner));
        thread[i].tx_thread_priority=thread[i].tx_thread_user_priority=16;
        thread[i].tx_thread_preempt_threshold=thread[i].tx_thread_user_preempt_threshold=16;
    }
    owner=1;
    if (argc>1 && !strcmp(argv[1],"--reject-outside")) {
        reject="service outside serialized call boundary";
        thread[0].tx_thread_preempt_threshold=thread[0].tx_thread_user_preempt_threshold=2;
        (void)_tx_thread_preemption_change(&thread[0],16,&old);CHECK(0);
    }
    anx_tx_context_begin(&f,&thread[0],0);
    CHECK(_tx_event_flags_create(&events,(CHAR *)"raised wait")==TX_SUCCESS);
    CHECK(_tx_thread_preemption_change(&thread[0],2,&old)==TX_SUCCESS && old==16);
    if (argc>1 && !strcmp(argv[1],"--reject-exit")) {
        reject="boundary exit with raised threshold";anx_tx_context_end(&f);CHECK(0);
    }
    if (argc>1 && !strcmp(argv[1],"--reject-inherit")) {
        reject="threshold inheritance not implemented";
        thread[0].tx_thread_inherit_priority=1;
        (void)_tx_thread_preemption_change(&thread[0],0,&old);CHECK(0);
    }
    CHECK(_tx_thread_preemption_change(&thread[0],17,&invalid_old)==TX_THRESH_ERROR && invalid_old==99);
    CHECK(thread[0].tx_thread_preempt_threshold==2 && thread[0].tx_thread_user_preempt_threshold==2);
    CHECK(_tx_thread_preemption_change(&thread[1],2,&invalid_old)==TX_FEATURE_NOT_ENABLED && invalid_old==99);
    CHECK(thread[1].tx_thread_preempt_threshold==16);
    CHECK(_tx_thread_preemption_change(&thread[0],0,TX_NULL)==TX_PTR_ERROR);
    memset(&absent,0,sizeof(absent));
    CHECK(_tx_thread_preemption_change(&absent,0,&old)==TX_THREAD_ERROR);
    CHECK(_tx_thread_preemption_change(TX_NULL,0,&old)==TX_THREAD_ERROR);
    anx_tx_context_begin(&nested,&thread[0],0);anx_tx_context_end(&nested);
    CHECK(depth==1 && thread[0].tx_thread_preempt_threshold==2);
    CHECK(_tx_thread_time_slice_change(&thread[0],1,&old_slice)==TX_SUCCESS && old_slice==0);
    CHECK(_tx_thread_time_slice_change(&thread[0],3,&old_slice)==TX_SUCCESS && old_slice==1);
    CHECK(thread[0].tx_thread_time_slice==3 && thread[0].tx_thread_new_time_slice==3);
    CHECK(_tx_thread_time_slice_change(&thread[0],0,TX_NULL)==TX_PTR_ERROR);
    CHECK(_tx_thread_time_slice_change(TX_NULL,0,&old_slice)==TX_THREAD_ERROR);
    anx_tx_context_begin(&nested,TX_NULL,1);
    CHECK(_tx_thread_preemption_change(&thread[0],0,&invalid_old)==TX_CALLER_ERROR && invalid_old==99);
    CHECK(_tx_thread_time_slice_change(&thread[0],0,&old_slice)==TX_CALLER_ERROR && old_slice==1);
    anx_tx_context_end(&nested);
    phase=1;
    CHECK(_tx_event_flags_get(&events,1,TX_OR_CLEAR,&actual,TX_WAIT_FOREVER)==TX_SUCCESS && actual==1);
    CHECK(phase==2 && depth==1 && _tx_thread_current_ptr==&thread[0] &&
          thread[0].tx_thread_preempt_threshold==2 && !thread[0].tx_thread_suspend_cleanup);
    CHECK(_tx_event_flags_delete(&events)==TX_SUCCESS);
    phase=3;
    CHECK(_tx_thread_preemption_change(&thread[0],old,&invalid_old)==TX_SUCCESS && invalid_old==2);
    CHECK(thread[0].tx_thread_preempt_threshold==16 && thread[0].tx_thread_user_preempt_threshold==16);
    anx_tx_context_end(&f);
    for (unsigned i=0;i<2;i++) {owner=i+1;CHECK(anx_tx_detach(&bridge[i]));}
    CHECK(!depth && phase==3 && !events.tx_event_flags_group_suspended_count);
    puts("research_sched_model=PASS owner threshold, advisory slice, protected raised-threshold suspension and restore");
    return 0;
}
