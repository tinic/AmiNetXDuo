/* Concrete dormant retirement/admission guards. SPDX-License-Identifier: MIT */
#include "tx_bridge.h"
#include "tx_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if (!(x)) {fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}} while (0)
static uintptr_t owner=1;
static unsigned depth,checks,reject;
static void enter(void *arg) {(void)arg;depth++;}
static void leave(void *arg) {(void)arg;CHECK(depth);depth--;}
static uintptr_t caller(void *arg) {(void)arg;return owner;}
static void panic(void *arg,const char *reason)
{
    (void)arg;
    if (reject && !strcmp(reason,"reinitialize active domain")) {puts("research_registry_guard_reset=PASS");exit(0);}
    fprintf(stderr,"panic %s\n",reason);exit(1);
}
static void preflight(void) {CHECK(depth);checks++;}
static uint64_t now(void *arg) {(void)arg;return 0;}
static int park(void *arg,uint64_t deadline) {(void)arg;(void)deadline;CHECK(0);return -1;}
static void notify(void *arg) {(void)arg;}
int main(int argc,char **argv)
{
    AnxTxPlatform p={enter,leave,caller,panic,0,0};
    AnxWaitOps ops={enter,leave,now,park,notify,0};
    AnxTxThread record;TX_THREAD thread;AnxWait wait;TX_MUTEX mutex;
    AnxTxContext frame,marked; (void)argv;
    anx_tx_runtime_init(&p);anx_tx_admission_check=preflight;
    if (argc>1) {reject=1;anx_tx_runtime_init(&p);CHECK(0);}
    anx_wait_init(&wait,&ops);CHECK(anx_tx_attach(&record,&thread,&wait,owner));
    anx_tx_context_begin(&frame,&thread,0);
    CHECK(checks==1 && anx_tx_context_is_outer(&frame) && anx_tx_quiescent(&record) && !anx_tx_forget_dormant(&record));
    CHECK(tx_mutex_create(&mutex,(CHAR *)"retained",TX_NO_INHERIT)==TX_SUCCESS && tx_mutex_get(&mutex,TX_NO_WAIT)==TX_SUCCESS);
    CHECK(!anx_tx_quiescent(&record));
    anx_tx_context_begin(&marked,TX_NULL,1);
    CHECK(checks==2 && !anx_tx_context_is_outer(&frame) && !anx_tx_forget_dormant(&record));
    anx_tx_context_end(&marked);anx_tx_context_end(&frame);
    owner=2;CHECK(!anx_tx_forget_dormant(&record));owner=1;
    anx_tx_context_begin(&frame,&thread,0);
    CHECK(tx_mutex_put(&mutex)==TX_SUCCESS && tx_mutex_delete(&mutex)==TX_SUCCESS);
    anx_tx_context_end(&frame);owner=2;
    record.semaphore_call=(TX_SEMAPHORE *)1;CHECK(!anx_tx_forget_dormant(&record));record.semaphore_call=0;
    record.exec_wait_nesting=1;CHECK(!anx_tx_forget_dormant(&record));record.exec_wait_nesting=0;
    record.abort_pins=1;CHECK(!anx_tx_forget_dormant(&record));record.abort_pins=0;
    wait.result=ANX_WAIT_PENDING;CHECK(!anx_tx_forget_dormant(&record));wait.result=ANX_WAIT_IDLE;
    CHECK(anx_tx_quiescent(&record) && anx_tx_forget_dormant(&record) && !thread.tx_thread_id &&
          !anx_tx_forget_dormant(&record) && !anx_tx_quiescent(&record) && !depth && checks==3);
    anx_tx_admission_check=0;anx_tx_runtime_init(&p);
    puts("research_registry_guard=PASS active/nested/owned/returning/paused/abort/pending retirement refusals and foreign quiescent unlink");return 0;
}
