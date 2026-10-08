/* Real pinned public suspend plus deterministic owner/protection model.
 * SPDX-License-Identifier: MIT */
#include "tx_bridge.h"
#include "tx_thread.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do {if (!(x)) {fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}} while (0)
static unsigned depth,disabled,parks,yields;
static uintptr_t owner=1;
static TX_THREAD thread[2];static AnxTxThread bridge[2];static AnxWait waits[2];
static TX_MUTEX mutex;
static void enter(void *p) {(void)p;depth++;}
static void leave(void *p) {(void)p;CHECK(depth);depth--;}
static uintptr_t caller(void *p) {(void)p;return owner;}
static int can_pause(void *p,unsigned levels) {(void)p;return depth==levels && !disabled;}
static void panic(void *p,const char *s) {(void)p;fprintf(stderr,"panic %s\n",s);exit(1);}
static uint64_t now(void *p) {(void)p;return 0;}
static void notify(void *p) {(void)p;CHECK(depth);}
static int park(void *p,uint64_t deadline)
{
    (void)p;AnxTxContext f;
    CHECK(deadline==ANX_WAIT_FOREVER && !depth && !_tx_thread_current_ptr && !anx_tx_holder);
    CHECK(bridge[0].explicit_suspend && thread[0].tx_thread_state==TX_SUSPENDED &&
          thread[0].tx_thread_timer.tx_timer_internal_remaining_ticks==0 &&
          !thread[0].tx_thread_timer.tx_timer_internal_list_head && !anx_tx_quiescent(&bridge[0]));
    owner=2;anx_tx_context_begin(&f,&thread[1],0);
    CHECK(tx_thread_suspend(&thread[0])==TX_SUCCESS && bridge[0].explicit_suspend);
    CHECK(anx_tx_explicit_resume(&thread[0])==TX_SUCCESS && thread[0].tx_thread_state==TX_READY);
    CHECK(!anx_tx_quiescent(&bridge[0]) && !anx_tx_forget_dormant(&bridge[0]));
    CHECK(bridge[0].explicit_suspend && waits[0].result==ANX_WAIT_READY);
    CHECK(anx_tx_explicit_resume(&thread[0])==TX_RESUME_ERROR && !_tx_thread_preempt_disable);
    anx_tx_context_end(&f);owner=1;parks++;return 1;
}
static void yield_point(void)
{
    AnxTxContext f;
    CHECK(!depth && !_tx_thread_current_ptr && !anx_tx_holder && bridge[0].paused_depth==2 &&
          !anx_tx_quiescent(&bridge[0]) && mutex.tx_mutex_owner==&thread[0]);
    owner=2;anx_tx_context_begin(&f,&thread[1],0);
    CHECK(tx_mutex_get(&mutex,TX_NO_WAIT)==TX_NOT_AVAILABLE);
    CHECK(tx_thread_suspend(&thread[0])==TX_FEATURE_NOT_ENABLED && thread[0].tx_thread_state==TX_READY);
    anx_tx_context_end(&f);owner=1;yields++;
}
int main(void)
{
    AnxTxPlatform p={enter,leave,caller,panic,0,can_pause};AnxWaitOps ops={enter,leave,now,park,notify,0};
    AnxTxContext f,nested,marked;
    anx_tx_runtime_init(&p);
    for (unsigned i=0;i<2;i++) {owner=i+1;anx_wait_init(&waits[i],&ops);CHECK(anx_tx_attach(&bridge[i],&thread[i],&waits[i],owner));}
    owner=1;anx_tx_context_begin(&f,&thread[0],0);
    CHECK(tx_thread_suspend((TX_THREAD *)1)==TX_THREAD_ERROR && anx_tx_explicit_resume((TX_THREAD *)1)==TX_THREAD_ERROR);
    CHECK(tx_thread_suspend(0)==TX_THREAD_ERROR && anx_tx_explicit_resume(0)==TX_THREAD_ERROR);
    CHECK(tx_thread_suspend(&thread[1])==TX_FEATURE_NOT_ENABLED && thread[1].tx_thread_state==TX_READY);
    thread[1].tx_thread_state=TX_SLEEP;
    CHECK(tx_thread_suspend(&thread[1])==TX_FEATURE_NOT_ENABLED && !thread[1].tx_thread_delayed_suspend);
    CHECK(anx_tx_explicit_resume(&thread[1])==TX_FEATURE_NOT_ENABLED && thread[1].tx_thread_state==TX_SLEEP);
    thread[1].tx_thread_state=TX_READY;
    anx_tx_context_begin(&marked,TX_NULL,1);
    CHECK(tx_thread_suspend(&thread[0])==TX_CALLER_ERROR && anx_tx_explicit_resume(&thread[0])==TX_CALLER_ERROR &&
          anx_tx_relinquish(yield_point)==TX_CALLER_ERROR);anx_tx_context_end(&marked);
    enter(0);CHECK(tx_thread_suspend(&thread[0])==TX_CALLER_ERROR && anx_tx_relinquish(yield_point)==TX_CALLER_ERROR);leave(0);
    disabled=1;CHECK(tx_thread_suspend(&thread[0])==TX_CALLER_ERROR && anx_tx_relinquish(yield_point)==TX_CALLER_ERROR);disabled=0;
    _tx_thread_preempt_disable=1;CHECK(tx_thread_suspend(&thread[0])==TX_SUSPEND_ERROR &&
        anx_tx_relinquish(yield_point)==TX_CALLER_ERROR);_tx_thread_preempt_disable=0;
    anx_tx_context_begin(&nested,&thread[0],0);CHECK(tx_thread_suspend(&thread[0])==TX_CALLER_ERROR);
    CHECK(anx_tx_relinquish(0)==TX_PTR_ERROR && !parks && !yields && depth==2 && thread[0].tx_thread_state==TX_READY);
    CHECK(tx_mutex_create(&mutex,(CHAR *)"yield retention",TX_NO_INHERIT)==TX_SUCCESS && tx_mutex_get(&mutex,0)==TX_SUCCESS);
    CHECK(anx_tx_relinquish(yield_point)==TX_SUCCESS && yields==1 && depth==2 &&
          tx_thread_identify()==&thread[0] && mutex.tx_mutex_owner==&thread[0]);
    CHECK(tx_mutex_put(&mutex)==TX_SUCCESS && tx_mutex_delete(&mutex)==TX_SUCCESS);anx_tx_context_end(&nested);
    for (unsigned i=0;i<2;i++) {CHECK(tx_thread_suspend(&thread[0])==TX_SUCCESS && parks==i+1 && depth==1 &&
        tx_thread_identify()==&thread[0] && thread[0].tx_thread_state==TX_READY && !bridge[0].explicit_suspend &&
        !thread[0].tx_thread_timer.tx_timer_internal_list_head && !thread[0].tx_thread_suspend_cleanup);}
    CHECK(anx_tx_explicit_resume(&thread[0])==TX_RESUME_ERROR);
    anx_tx_context_end(&f);
    for (unsigned i=0;i<2;i++) {owner=i+1;CHECK(anx_tx_detach(&bridge[i]));}
    CHECK(!depth && anx_tx_runtime_resettable());
    puts("research_public_schedule=PASS real pinned explicit suspension, retained return lifetime, actual resume token, nested yield identity/mutex and refusal guards");return 0;
}
