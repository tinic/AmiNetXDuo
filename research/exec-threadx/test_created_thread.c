/* Host binding/retirement guards, not Exec lifecycle/ABI evidence.
 * SPDX-License-Identifier: MIT */
#include "tx_bridge.h"
#include "tx_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if (!(x)) {fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}} while (0)
static unsigned depth,reject_reset;
static uintptr_t owner=1;
static void enter(void *p) {(void)p;depth++;}
static void leave(void *p) {(void)p;CHECK(depth);depth--;}
static uintptr_t caller(void *p) {(void)p;return owner;}
static void panic(void *p,const char *s)
{
    (void)p;
    if (reject_reset && !strcmp(s,"reinitialize active domain")) {
        puts("research_created_guard=PASS reserved runtime reset rejected");exit(0);
    }
    fprintf(stderr,"panic %s\n",s);exit(1);
}
static uint64_t clock_now(void *p) {(void)p;return 0;}
static int park(void *p,uint64_t deadline) {(void)p;(void)deadline;CHECK(0);return -1;}
static void notify(void *p) {(void)p;}
static void entry(ULONG input) {(void)input;}
int main(int argc,char **argv)
{
    AnxTxPlatform p={enter,leave,caller,panic,0};
    AnxWaitOps ops={enter,leave,clock_now,park,notify,0};
    AnxWait waits[2]; AnxTxThread parent,created,duplicate;
    TX_THREAD pt,t,before;
    TX_MUTEX m; AnxTxContext f;
    (void)argv;
    anx_tx_runtime_init(&p);
    if (argc>1) {reject_reset=1;anx_tx_runtime_hold();anx_tx_runtime_init(&p);CHECK(0);}
    anx_wait_init(&waits[0],&ops);anx_wait_init(&waits[1],&ops);
    CHECK(anx_tx_attach(&parent,&pt,&waits[0],1));
    memset(&t,0,sizeof(t));
    t.tx_thread_id=TX_THREAD_ID;t.tx_thread_state=TX_SUSPENDED;
    t.tx_thread_amiga_task=(VOID *)2;t.tx_thread_name=(CHAR *)"kept";
    t.tx_thread_entry=entry;t.tx_thread_entry_parameter=0x12345678;
    t.tx_thread_priority=16;t.tx_thread_preempt_threshold=16;
    t.tx_thread_stack_size=8192;t.tx_thread_stack_start=&t;
    before=t;
    anx_tx_context_begin(&f,&pt,0);
    CHECK(_tx_mutex_create(&m,(CHAR *)"owned",TX_NO_INHERIT)==TX_SUCCESS);
    CHECK(anx_tx_bind_created(&created,&t,&waits[1],2));
    CHECK(!memcmp(&t,&before,sizeof(t)));
    CHECK(!anx_tx_bind_created(&duplicate,&t,&waits[1],2));
    CHECK(!memcmp(&t,&before,sizeof(t)));
    CHECK(!anx_tx_complete(&created)); /* foreign owner */
    t.tx_thread_state=TX_READY;
    anx_tx_context_end(&f);
    owner=2;
    anx_tx_context_begin(&f,&t,0);
    CHECK(_tx_thread_identify()==&t && _tx_mutex_get(&m,TX_NO_WAIT)==TX_SUCCESS);
    CHECK(!anx_tx_complete(&created)); /* active boundary */
    anx_tx_context_end(&f);
    CHECK(!anx_tx_complete(&created)); /* real owned mutex */
    CHECK(t.tx_thread_state==TX_READY && t.tx_thread_id==TX_THREAD_ID);
    anx_tx_context_begin(&f,&t,0);
    CHECK(_tx_mutex_put(&m)==TX_SUCCESS);
    anx_tx_context_end(&f);
    created.abort_pins=1;CHECK(!anx_tx_complete(&created));created.abort_pins=0;
    CHECK(anx_tx_complete(&created));
    CHECK(t.tx_thread_id==TX_THREAD_ID && t.tx_thread_state==TX_COMPLETED &&
          t.tx_thread_entry==entry && t.tx_thread_entry_parameter==before.tx_thread_entry_parameter);
    CHECK(!anx_tx_complete(&created));
    owner=1;CHECK(anx_tx_detach(&parent));
    anx_tx_runtime_hold();anx_tx_runtime_drop();anx_tx_runtime_init(&p);
    CHECK(!depth && anx_tx_runtime_idle());
    puts("research_created_model=PASS preserved fields, duplicate/foreign/active/owned/pinned retirement guards, retained completed ID");
    return 0;
}
