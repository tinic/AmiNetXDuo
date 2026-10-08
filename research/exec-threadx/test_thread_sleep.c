/* Real ThreadX sleep/timeout/wait_abort against the research bridge.
 * SPDX-License-Identifier: MIT */
#include "netx_resume.h"
#include "tx_thread.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c); exit(1); } } while (0)
static TX_THREAD thread[2];
static AnxTxThread bridge[2];
static AnxWait waits[2];
static AnxWaitOps ops[2];
static unsigned depth,abort_sleep;
static uintptr_t owner=1;
static uint64_t now;
static void enter(void *a) { (void)a; depth++; }
static void leave(void *a) { (void)a; CHECK(depth); depth--; }
static uintptr_t caller(void *a) { (void)a; return owner; }
static void panic(void *a,const char *t) { (void)a; fprintf(stderr,"panic: %s\n",t); exit(1); }
static uint64_t clock_now(void *a) { (void)a; CHECK(depth); return now; }
static void notify(void *a) { (void)a; CHECK(depth); }
static int park(void *a,uint64_t deadline)
{
    (void)a; CHECK(!depth && !_tx_thread_current_ptr && !_tx_thread_preempt_disable);
    if (!abort_sleep) { CHECK(deadline==20000); now=deadline; return 0; }
    AnxTxContext frame;
    owner=2; anx_tx_context_begin(&frame,&thread[1],0);
    CHECK(_tx_thread_wait_abort(&thread[0])==TX_SUCCESS);
    anx_tx_context_end(&frame); owner=1; return 1;
}
int main(void)
{
    static const AnxTxPlatform p={enter,leave,caller,panic,0,0};
    AnxTxContext frame;
    anx_tx_runtime_init(&p);
    for (unsigned i=0;i<2;i++) {
        owner=i+1; ops[i]=(AnxWaitOps){enter,leave,clock_now,park,notify,0};
        anx_wait_init(&waits[i],&ops[i]);
        CHECK(anx_tx_attach(&bridge[i],&thread[i],&waits[i],owner));
        CHECK(anx_tx_set_abort_policy(&bridge[i],anx_netx_receive_abort_policy));
    }
    owner=1; anx_tx_context_begin(&frame,&thread[0],0);
    CHECK(_tx_thread_sleep(0)==TX_SUCCESS && !bridge[0].parks);
    CHECK(_tx_thread_sleep(1)==TX_SUCCESS && now==20000 && bridge[0].resumes==1);
    CHECK(thread[0].tx_thread_state==TX_READY && !thread[0].tx_thread_timer.tx_timer_internal_list_head);
    abort_sleep=1;
    CHECK(_tx_thread_sleep(2)==TX_WAIT_ABORTED && bridge[0].resumes==2);
    anx_tx_context_end(&frame);
    anx_tx_context_begin(&frame,TX_NULL,1);
    CHECK(_tx_thread_sleep(1)==TX_CALLER_ERROR);
    anx_tx_context_end(&frame);
    CHECK(!depth && !_tx_thread_current_ptr && !_tx_thread_system_state && !_tx_thread_preempt_disable);
    for (unsigned i=0;i<2;i++) { owner=i+1; CHECK(anx_tx_detach(&bridge[i])); }
    puts("research_thread_sleep_model=PASS checks=4/4 real sleep + timeout/wait_abort");
    return 0;
}
