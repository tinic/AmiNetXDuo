/* Host ownership/lock model; native IO is tested separately. SPDX-License-Identifier: MIT */
#include "tx_bridge.h"
#include "tx_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if (!(x)) {fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}} while (0)
static unsigned depth,disabled;
static uintptr_t owner=1;
static const char *expected;
static void enter(void *p) {(void)p;depth++;}
static void leave(void *p) {(void)p;CHECK(depth);depth--;}
static uintptr_t caller(void *p) {(void)p;return owner;}
static int can_pause(void *p,unsigned levels) {(void)p;return depth==levels && !disabled;}
static void panic(void *p,const char *s)
{
    (void)p;
    if (expected && !strcmp(s,expected)) {puts("research_context_pause_guard=PASS");exit(0);}
    fprintf(stderr,"panic %s\n",s);exit(1);
}
static uint64_t clock_now(void *p) {(void)p;return 0;}
static int park(void *p,uint64_t deadline) {(void)p;(void)deadline;CHECK(0);return -1;}
static void notify(void *p) {(void)p;CHECK(depth);}
int main(int argc,char **argv)
{
    AnxTxPlatform p={enter,leave,caller,panic,0,can_pause};
    AnxWaitOps ops={enter,leave,clock_now,park,notify,0};
    AnxTxThread bridges[2]; TX_THREAD threads[2]; AnxWait waits[2];
    AnxTxContext frames[3],other,marked; TX_MUTEX mutex; TX_SEMAPHORE semaphore;
    anx_tx_runtime_init(&p);
    for (unsigned i=0;i<2;i++) {
        owner=i+1;anx_wait_init(&waits[i],&ops);
        CHECK(anx_tx_attach(&bridges[i],&threads[i],&waits[i],owner));
    }
    owner=1; CHECK(!anx_tx_context_pause() && !anx_tx_context_resume() && !depth);
    for (unsigned i=0;i<3;i++) anx_tx_context_begin(&frames[i],&threads[0],0);
    CHECK(tx_mutex_create(&mutex,(CHAR *)"kept",TX_NO_INHERIT)==TX_SUCCESS &&
          tx_mutex_get(&mutex,TX_NO_WAIT)==TX_SUCCESS &&
          tx_semaphore_create(&semaphore,(CHAR *)"progress",0)==TX_SUCCESS);
    enter(0); CHECK(!anx_tx_context_pause() && depth==4); leave(0);
    disabled=1; CHECK(!anx_tx_context_pause() && depth==3); disabled=0;
    anx_tx_context_begin(&marked,TX_NULL,1);
    CHECK(!anx_tx_context_pause() && depth==4); anx_tx_context_end(&marked);
    _tx_thread_preempt_disable=1;CHECK(!anx_tx_context_pause() && depth==3);_tx_thread_preempt_disable=0;
    CHECK(anx_tx_context_pause() && !depth && anx_tx_runtime_idle() && !tx_thread_identify() &&
          bridges[0].paused_frame==&frames[2] && bridges[0].paused_depth==3 && !anx_tx_detach(&bridges[0]));
    if (argc>1) {
        if (!strcmp(argv[1],"--reject-begin")) {
            expected="context begin while Exec wait paused";anx_tx_context_begin(&other,&threads[0],0);
        } else if (!strcmp(argv[1],"--reject-end")) {
            expected="context owner/order";anx_tx_context_end(&frames[2]);
        } else if (!strcmp(argv[1],"--reject-reset")) {
            expected="reinitialize active domain";anx_tx_runtime_init(&p);
        } else if (!strcmp(argv[1],"--reject-service")) {
            expected="service outside serialized call boundary"; (void)tx_semaphore_put(&semaphore);
        }
        CHECK(0);
    }
    CHECK(!anx_tx_set_resume_cleanup(&bridges[0],0) && !anx_tx_set_abort_policy(&bridges[0],0));
    CHECK(anx_tx_context_pause() && bridges[0].exec_wait_nesting==2 && !depth);
    owner=2;CHECK(!anx_tx_context_resume() && !anx_tx_context_pause() && !depth);
    anx_tx_context_begin(&other,&threads[1],0);
    CHECK(tx_thread_identify()==&threads[1] && tx_mutex_get(&mutex,TX_NO_WAIT)==TX_NOT_AVAILABLE &&
          tx_semaphore_put(&semaphore)==TX_SUCCESS && !anx_tx_context_resume());
    anx_tx_context_end(&other);owner=1;
    enter(0);CHECK(!anx_tx_context_resume() && bridges[0].exec_wait_nesting==2);leave(0);
    CHECK(anx_tx_context_resume() && !depth && !tx_thread_identify() && bridges[0].exec_wait_nesting==1);
    CHECK(anx_tx_context_resume() && depth==3 && tx_thread_identify()==&threads[0] &&
          !bridges[0].paused_frame && !bridges[0].paused_depth && !bridges[0].exec_wait_nesting);
    CHECK(!anx_tx_context_resume() && depth==3 && tx_semaphore_get(&semaphore,TX_NO_WAIT)==TX_SUCCESS);
    CHECK(mutex.tx_mutex_owner==&threads[0] && tx_mutex_put(&mutex)==TX_SUCCESS &&
          tx_mutex_delete(&mutex)==TX_SUCCESS && tx_semaphore_delete(&semaphore)==TX_SUCCESS);
    for (unsigned i=3;i;i--) anx_tx_context_end(&frames[i-1]);
    for (unsigned i=0;i<2;i++) {owner=i+1;CHECK(anx_tx_detach(&bridges[i]));}
    anx_tx_runtime_init(&p);CHECK(!depth && anx_tx_runtime_idle());
    puts("research_context_pause=PASS nested restore, independent owner progress, retained mutex, invalid protection/owner/retirement guards");
    return 0;
}
