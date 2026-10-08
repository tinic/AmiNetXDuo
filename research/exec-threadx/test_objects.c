/* Lifetime probes and fatal domain guards, not Exec ABI/performance evidence.
 * SPDX-License-Identifier: MIT */
#include "tx_bridge.h"
#include "tx_mutex.h"
#include "tx_event_flags.h"
#include "tx_semaphore.h"
#include "object_probe.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if (!(x)) {fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}} while (0)
static unsigned depth;
static const char *reject;
static TX_MUTEX m;
static TX_EVENT_FLAGS_GROUP g;
static TX_SEMAPHORE sem;
static void enter(void *p) {(void)p;depth++;}
static void leave(void *p) {(void)p;CHECK(depth);depth--;}
static uintptr_t caller(void *p) {(void)p;return 1;}
static void panic(void *p,const char *s)
{
    (void)p;
    if (reject && !strcmp(s,reject)) {
        CHECK((m.tx_mutex_id==TX_MUTEX_ID && _tx_mutex_created_count==1) ||
              (g.tx_event_flags_group_id==TX_EVENT_FLAGS_ID && _tx_event_flags_created_count==1) ||
              (sem.tx_semaphore_id==TX_SEMAPHORE_ID && _tx_semaphore_created_count==1));
        puts("research_object_guard=PASS live object preserved at fatal guard");exit(0);
    }
    fprintf(stderr,"panic %s\n",s);exit(1);
}
static uint64_t now(void *p) {(void)p;return 0;}
static int park(void *p,uint64_t t) {(void)p;(void)t;CHECK(0);return -1;}
static void notify(void *p) {(void)p;}
int main(int argc,char **argv)
{
    AnxTxPlatform p={enter,leave,caller,panic,0};
    AnxWaitOps ops={enter,leave,now,park,notify,0};
    AnxWait wait;AnxTxThread bridge;TX_THREAD thread;AnxTxContext f;
    anx_tx_runtime_init(&p);anx_wait_init(&wait,&ops);
    CHECK(anx_tx_attach(&bridge,&thread,&wait,1));anx_tx_context_begin(&f,&thread,0);
    if (argc>1) {
        CHECK(argc==2);
        if (!strcmp(argv[1],"--reject-reset-semaphore")) CHECK(tx_semaphore_create(&sem,(CHAR *)"live",0)==TX_SUCCESS);
        else if (!strcmp(argv[1],"--reject-reset-event")) CHECK(tx_event_flags_create(&g,(CHAR *)"live")==TX_SUCCESS);
        else CHECK(tx_mutex_create(&m,(CHAR *)"live",TX_NO_INHERIT)==TX_SUCCESS);
        anx_tx_context_end(&f);
        if (!strcmp(argv[1],"--reject-outside")) {
            reject="service outside serialized call boundary";(void)tx_mutex_delete(&m);
        } else if (!strcmp(argv[1],"--reject-marked")) {
            reject="object lifecycle in callback or marked context not implemented";
            anx_tx_context_begin(&f,TX_NULL,1);(void)tx_mutex_delete(&m);
        } else {
            CHECK(!strcmp(argv[1],"--reject-reset-mutex") || !strcmp(argv[1],"--reject-reset-event") ||
                  !strcmp(argv[1],"--reject-reset-semaphore"));
            CHECK(anx_tx_detach(&bridge));reject="reinitialize active domain";anx_tx_runtime_init(&p);
        }
        CHECK(0);
    }
    int result=anx_object_probe();
    if (result) fprintf(stderr,"object probe line %d\n",result);
    CHECK(!result);anx_tx_context_end(&f);CHECK(anx_tx_detach(&bridge));anx_tx_runtime_init(&p);
    CHECK(!depth && anx_tx_runtime_idle());
    puts("research_objects=PASS real created rings, duplicate/forged/owned guards, pinned deletes, counter preservation and poisoned reuse");
    return 0;
}
