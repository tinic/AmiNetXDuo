/* Real event cleanup + private terminal return, not Exec task/IO evidence.
 * SPDX-License-Identifier: MIT */
#include "tx_bridge.h"
#include "tx_thread.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if (!(x)) {fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}} while (0)
static unsigned depth,terminals,return_terminal;
static uintptr_t owner=1;
static TX_THREAD target,parent;
static AnxTxThread bridge,pb;
static AnxWait waits[2];
static TX_EVENT_FLAGS_GROUP events,wrong;
static jmp_buf done;
static const char *reject;
static void enter(void *p) {(void)p;depth++;}
static void leave(void *p) {(void)p;CHECK(depth);depth--;}
static uintptr_t caller(void *p) {(void)p;return owner;}
static void panic(void *p,const char *s)
{
    (void)p;
    if (reject && !strcmp(s,reject)) {
        CHECK(!depth && bridge.terminal_pending && !bridge.thread && terminals==1);
        puts("research_stop_guard=PASS returning terminal rejected");exit(0);
    }
    fprintf(stderr,"panic %s\n",s);exit(1);
}
static uint64_t now(void *p) {(void)p;CHECK(depth);return 0;}
static void notify(void *p) {(void)p;CHECK(depth);}
static void terminal(void *p)
{
    CHECK(p==&bridge && owner==1 && !depth && anx_tx_runtime_idle());
    CHECK(!bridge.thread && bridge.terminal_pending && waits[0].result==ANX_WAIT_DELETED &&
          target.tx_thread_id==TX_THREAD_ID && target.tx_thread_state==TX_TERMINATED &&
          !target.tx_thread_suspend_cleanup && !target.tx_thread_timer.tx_timer_internal_list_head &&
          !events.tx_event_flags_group_suspended_count && !events.tx_event_flags_group_suspension_list);
    terminals++;
    if (!return_terminal) longjmp(done,1); /* host escape only; native callback RemTask */
}
static int park(void *p,uint64_t deadline)
{
    AnxTxContext f,nested;
    TX_THREAD before;
    (void)p;CHECK(!depth && !_tx_thread_current_ptr && deadline!=ANX_WAIT_FOREVER);
    owner=2;anx_tx_context_begin(&f,&parent,0);
    before=target;
    TX_EVENT_FLAGS_GROUP saved=events;
    CHECK(_tx_event_flags_delete(&events)==TX_FEATURE_NOT_ENABLED && !memcmp(&events,&saved,sizeof(events)));
    CHECK(!anx_tx_stop_event(&bridge,&wrong) && !memcmp(&target,&before,sizeof(target)));
    bridge.abort_pins=1;CHECK(!anx_tx_stop_event(&bridge,&events));bridge.abort_pins=0;
    bridge.pending_resume=1;CHECK(!anx_tx_stop_event(&bridge,&events));bridge.pending_resume=0;
    bridge.pending_token=1;CHECK(!anx_tx_stop_event(&bridge,&events));bridge.pending_token=0;
    anx_tx_context_begin(&nested,&parent,0);
    CHECK(!anx_tx_stop_event(&bridge,&events));anx_tx_context_end(&nested);
    CHECK(!memcmp(&target,&before,sizeof(target)) && waits[0].result==ANX_WAIT_PENDING);
    _tx_thread_preempt_disable++;
    CHECK(anx_tx_stop_event(&bridge,&events));
    CHECK(_tx_thread_preempt_disable==1 && !bridge.thread && bridge.terminal_pending &&
          target.tx_thread_id==TX_THREAD_ID && target.tx_thread_state==TX_TERMINATED &&
          !events.tx_event_flags_group_suspended_count && !target.tx_thread_suspend_cleanup &&
          !target.tx_thread_timer.tx_timer_internal_list_head && !terminals);
    CHECK(!anx_tx_stop_event(&bridge,&events));
    _tx_thread_preempt_disable--;
    anx_tx_context_end(&f);owner=1;return 1;
}
int main(int argc,char **argv)
{
    AnxTxPlatform p={enter,leave,caller,panic,0};
    AnxWaitOps ops={enter,leave,now,park,notify,0};
    AnxTxContext f;
    ULONG actual=0;
    (void)argv;
    if (argc>1) {return_terminal=1;reject="private terminal owner returned";}
    anx_tx_runtime_init(&p);
    anx_wait_init(&waits[0],&ops);anx_wait_init(&waits[1],&ops);
    CHECK(anx_tx_attach(&bridge,&target,&waits[0],1));
    CHECK(anx_tx_set_terminal_owner(&bridge,terminal,&bridge));
    CHECK(!anx_tx_set_terminal_owner(&bridge,terminal,&bridge));
    owner=2;CHECK(anx_tx_attach(&pb,&parent,&waits[1],2));
    anx_tx_context_begin(&f,&parent,0);
    CHECK(_tx_event_flags_create(&events,(CHAR *)"terminal")==TX_SUCCESS &&
          _tx_event_flags_create(&wrong,(CHAR *)"wrong")==TX_SUCCESS);
    CHECK(!anx_tx_stop_event(&bridge,&events)); /* READY, no blocked generation */
    anx_tx_context_end(&f);owner=1;
    if (!setjmp(done)) {
        anx_tx_context_begin(&f,&target,0);
        (void)_tx_event_flags_get(&events,1,TX_OR_CLEAR,&actual,50);
        CHECK(0); /* terminal event wait must never return to caller body */
    }
    CHECK(terminals==1 && !depth && !bridge.thread && anx_tx_runtime_idle());
    owner=2;anx_tx_context_begin(&f,&parent,0);
    CHECK(_tx_event_flags_delete(&events)==TX_SUCCESS && _tx_event_flags_delete(&wrong)==TX_SUCCESS);
    anx_tx_context_end(&f);CHECK(anx_tx_detach(&pb));anx_tx_runtime_init(&p);
    puts("research_stop_model=PASS real event cleanup, timer unlink, private terminal before public reentry");
    return 0;
}
