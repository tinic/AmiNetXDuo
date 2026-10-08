/* Actual pinned event services and NetX periodic callback, concurrent callers.
 * This models the IP helper event boundary, not the full IP helper/wire stack.
 * SPDX-License-Identifier: MIT */
#define NX_SOURCE_CODE
#include "netx_resume.h"
#include "nx_ip.h"
#include "tx_thread.h"
#include "tx_event_flags.h"
#include "tx_timer.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c); abort(); } } while (0)
typedef struct Caller Caller;
struct Caller {
    TX_THREAD thread;
    AnxTxThread bridge;
    AnxWait wait;
    AnxWaitOps ops;
    pthread_cond_t wake;
    pthread_t task;
    uintptr_t id;
    unsigned parks,done,allow_exit;
    UINT result,option;
    ULONG requested,actual,ticks;
    void (*body)(Caller *);
};
static Caller callers[3];
static pthread_mutex_t lock;
static pthread_cond_t progress=PTHREAD_COND_INITIALIZER;
static _Thread_local uintptr_t identity;
static _Thread_local unsigned depth;
static NX_IP ip;
static TX_TIMER timer,other;
static unsigned calls,cancel_self,cancel_other;
static const char *reject;
static void enter(void *a) { (void)a; CHECK(!pthread_mutex_lock(&lock)); depth++; }
static void leave(void *a) { (void)a; CHECK(depth); depth--; CHECK(!pthread_mutex_unlock(&lock)); }
static uintptr_t caller(void *a) { (void)a; return identity; }
static void panic(void *a,const char *s)
{
    (void)a;
    if (reject && !strcmp(s,reject)) {
        CHECK(!ip.nx_ip_events.tx_event_flags_group_suspended_count &&
              !callers[0].thread.tx_thread_suspend_cleanup && callers[0].thread.tx_thread_state==TX_READY);
        if (!strcmp(s,"timer lifecycle inside callback not implemented"))
            CHECK(_tx_timer_created_count==1 && timer.tx_timer_id==TX_TIMER_ID &&
                  timer.tx_timer_internal.tx_timer_internal_list_head);
        puts("research_event_timer=PASS guard rejected before mutation"); exit(0);
    }
    fprintf(stderr,"panic: %s\n",s); abort();
}
static uint64_t now(void *a)
{
    struct timespec t; (void)a; CHECK(depth); CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));
    return (uint64_t)t.tv_sec*1000000+(uint64_t)t.tv_nsec/1000;
}
static struct timespec realtime_after(uint64_t us)
{
    struct timespec t; CHECK(!clock_gettime(CLOCK_REALTIME,&t));
    t.tv_sec+=(time_t)(us/1000000); t.tv_nsec+=(long)(us%1000000)*1000;
    if (t.tv_nsec>=1000000000) { t.tv_sec++; t.tv_nsec-=1000000000; }
    return t;
}
static void notify(void *a) { Caller *c=a; CHECK(depth); CHECK(!pthread_cond_broadcast(&c->wake)); CHECK(!pthread_cond_broadcast(&progress)); }
static int park(void *a,uint64_t deadline)
{
    Caller *c=a; int result=0;
    CHECK(!depth); enter(0);
    CHECK(!_tx_thread_current_ptr && !_tx_thread_preempt_disable);
    c->parks++; CHECK(!pthread_cond_broadcast(&progress));
    while (c->wait.result==ANX_WAIT_PENDING) {
        uint64_t clock=now(0);
        if (deadline!=ANX_WAIT_FOREVER && clock>=deadline) break;
        struct timespec end=realtime_after(deadline==ANX_WAIT_FOREVER ? 3000000 : deadline-clock);
        int err=pthread_cond_timedwait(&c->wake,&lock,&end);
        CHECK(!err || err==ETIMEDOUT);
        CHECK(!err || deadline!=ANX_WAIT_FOREVER); /* bounded harness, not infinite hang */
    }
    result=c->wait.result!=ANX_WAIT_PENDING;
    leave(0); return result;
}
static const AnxTxPlatform platform={enter,leave,caller,panic,0};
static void attach(Caller *c,uintptr_t id)
{
    identity=id; c->id=id;
    CHECK(!pthread_cond_init(&c->wake,0));
    c->ops=(AnxWaitOps){enter,leave,now,park,notify,c};
    anx_wait_init(&c->wait,&c->ops);
    CHECK(anx_tx_attach(&c->bridge,&c->thread,&c->wait,id));
}
static void wait_for(unsigned *value,unsigned minimum)
{
    struct timespec end=realtime_after(3000000);
    CHECK(!depth); enter(0);
    while (*value<minimum) CHECK(!pthread_cond_timedwait(&progress,&lock,&end));
    leave(0);
}
static void *start(void *a)
{
    Caller *c=a; attach(c,c->id); c->body(c);
    enter(0); c->done=1; CHECK(!pthread_cond_broadcast(&progress)); leave(0);
    wait_for(&c->allow_exit,1);
    CHECK(anx_tx_detach(&c->bridge));
    return 0;
}
static void spawn(unsigned index,void (*body)(Caller *))
{
    Caller *c=&callers[index]; c->id=index+1; c->body=body;
    CHECK(!pthread_create(&c->task,0,start,c));
}
static void release_workers(unsigned count)
{
    for (unsigned i=1;i<=count;i++) wait_for(&callers[i].done,1);
    enter(0);
    for (unsigned i=1;i<=count;i++) callers[i].allow_exit=1;
    CHECK(!pthread_cond_broadcast(&progress)); leave(0);
    for (unsigned i=1;i<=count;i++) {
        CHECK(!pthread_join(callers[i].task,0)); CHECK(!pthread_cond_destroy(&callers[i].wake));
    }
}

static void init(void)
{
    AnxTxContext frame;
    memset(callers,0,sizeof(callers)); memset(&ip,0,sizeof(ip));
    anx_tx_runtime_init(&platform); attach(&callers[0],1);
    anx_tx_context_begin(&frame,&callers[0].thread,0);
    CHECK(_tx_event_flags_create(&ip.nx_ip_events,(CHAR *)"IP events")==TX_SUCCESS);
    CHECK(_tx_mutex_create(&ip.nx_ip_protection,(CHAR *)"IP",TX_NO_INHERIT)==TX_SUCCESS);
    anx_tx_context_end(&frame);
}
static void finish(void)
{
    CHECK(!ip.nx_ip_events.tx_event_flags_group_suspension_list && !ip.nx_ip_events.tx_event_flags_group_suspended_count);
    CHECK(!_tx_timer_created_count && !_tx_thread_current_ptr && !_tx_thread_system_state && !_tx_thread_preempt_disable && !depth);
    CHECK(anx_tx_detach(&callers[0].bridge)); CHECK(!pthread_cond_destroy(&callers[0].wake));
}
static void getter(Caller *c)
{
    AnxTxContext frame;
    CHECK(anx_tx_set_abort_policy(&c->bridge,anx_netx_receive_abort_policy));
    anx_tx_context_begin(&frame,&c->thread,0);
    /* Same mutex release/event wait/acquire ordering as nx_ip_thread_entry. */
    CHECK(_tx_mutex_get(&ip.nx_ip_protection,TX_WAIT_FOREVER)==TX_SUCCESS);
    CHECK(_tx_mutex_put(&ip.nx_ip_protection)==TX_SUCCESS);
    c->result=_tx_event_flags_get(&ip.nx_ip_events,c->requested,c->option,&c->actual,c->ticks);
    CHECK(_tx_mutex_get(&ip.nx_ip_protection,TX_WAIT_FOREVER)==TX_SUCCESS);
    CHECK(!c->thread.tx_thread_suspend_cleanup && c->thread.tx_thread_state==TX_READY);
    CHECK(!c->thread.tx_thread_timer.tx_timer_internal_list_head);
    CHECK(_tx_mutex_put(&ip.nx_ip_protection)==TX_SUCCESS);
    anx_tx_context_end(&frame);
}
static void worker(unsigned index,ULONG requested,UINT option,ULONG ticks)
{
    callers[index].requested=requested; callers[index].option=option; callers[index].ticks=ticks;
    spawn(index,getter); wait_for(&callers[index].parks,1);
}
static void tick(void)
{
    AnxTxContext frame;
    anx_tx_context_begin(&frame,TX_NULL,1); anx_tx_timer_tick(); anx_tx_context_end(&frame);
}
static void delete_callback(ULONG value)
{
    (void)value; (void)_tx_timer_delete(&timer); CHECK(0);
}
static void count_callback(ULONG value)
{
    CHECK(_tx_thread_system_state && !_tx_thread_current_ptr && value==123);
    calls++;
    if (cancel_self) CHECK(_tx_timer_deactivate(&timer)==TX_SUCCESS);
    if (cancel_other) CHECK(_tx_timer_deactivate(&other)==TX_SUCCESS);
}
int main(int argc,char **argv)
{
    pthread_mutexattr_t attr;
    AnxTxContext frame,marked;
    ULONG actual;
    CHECK(!pthread_mutexattr_init(&attr)); CHECK(!pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_RECURSIVE));
    CHECK(!pthread_mutex_init(&lock,&attr)); CHECK(!pthread_mutexattr_destroy(&attr));
    if (argc==2) {
        init(); anx_tx_context_begin(&frame,&callers[0].thread,0);
        if (!strcmp(argv[1],"--reject-nested-get")) {
            reject="unsupported event blocking context";
            anx_tx_context_begin(&marked,&callers[0].thread,0);
            (void)_tx_event_flags_get(&ip.nx_ip_events,1,TX_OR,&actual,1);
        } else if (!strcmp(argv[1],"--reject-timer-get")) {
            reject="unsupported event blocking context";
            anx_tx_context_begin(&marked,TX_NULL,1);
            (void)_tx_event_flags_get(&ip.nx_ip_events,1,TX_OR,&actual,1);
        } else if (!strcmp(argv[1],"--reject-tick-context")) {
            reject="unsupported timer tick context"; anx_tx_timer_tick();
        } else if (!strcmp(argv[1],"--reject-callback-delete")) {
            CHECK(_tx_timer_create(&timer,(CHAR *)"delete",delete_callback,0,1,2,TX_AUTO_ACTIVATE)==TX_SUCCESS);
            anx_tx_context_end(&frame); reject="timer lifecycle inside callback not implemented"; tick();
        }
        CHECK(0);
    }
    init(); anx_tx_context_begin(&frame,&callers[0].thread,0);
    CHECK(_tx_event_flags_set(&ip.nx_ip_events,7,TX_OR)==TX_SUCCESS);
    CHECK(_tx_event_flags_get(&ip.nx_ip_events,3,TX_AND_CLEAR,&actual,TX_NO_WAIT)==TX_SUCCESS && actual==7);
    CHECK(ip.nx_ip_events.tx_event_flags_group_current==4);
    CHECK(_tx_event_flags_get(&ip.nx_ip_events,3,TX_AND,&actual,TX_NO_WAIT)==TX_NO_EVENTS && actual==4);
    CHECK(_tx_event_flags_set(&ip.nx_ip_events,0,TX_AND)==TX_SUCCESS);
    CHECK(_tx_event_flags_get(&ip.nx_ip_events,0,TX_OR,&actual,TX_WAIT_FOREVER)==TX_NO_EVENTS);
    _tx_thread_preempt_disable++;
    CHECK(_tx_event_flags_get(&ip.nx_ip_events,1,TX_OR,&actual,1)==TX_NO_EVENTS);
    _tx_thread_preempt_disable--;
    CHECK(!callers[0].parks);
    anx_tx_context_end(&frame); finish();
    /* OR_CLEAR arrival, timeout, abort, stale cleanup, expire first, set first. */
    for (unsigned mode=0;mode<6;mode++) {
        init(); worker(1,1,TX_OR_CLEAR,mode==1 ? 2 : mode>=4 ? 50 : TX_WAIT_FOREVER);
        anx_tx_context_begin(&frame,&callers[0].thread,0);
        uint32_t token=callers[1].bridge.token;
        if (mode==2) CHECK(_tx_thread_wait_abort(&callers[1].thread)==TX_SUCCESS);
        if (mode==3) {
            ULONG sequence=callers[1].thread.tx_thread_suspension_sequence;
            _tx_event_flags_cleanup(&callers[1].thread,sequence-1);
            CHECK(ip.nx_ip_events.tx_event_flags_group_suspended_count==1 && callers[1].thread.tx_thread_suspend_cleanup);
        }
        if (mode==4) {
            anx_tx_context_begin(&marked,TX_NULL,1);
            CHECK(anx_tx_expire(&callers[1].thread,token)); anx_tx_context_end(&marked);
        }
        if (mode!=1 && mode!=2) CHECK(_tx_event_flags_set(&ip.nx_ip_events,5,TX_OR)==TX_SUCCESS);
        if (mode==5) {
            anx_tx_context_begin(&marked,TX_NULL,1);
            CHECK(!anx_tx_expire(&callers[1].thread,token)); anx_tx_context_end(&marked);
        }
        anx_tx_context_end(&frame); release_workers(1);
        CHECK(callers[1].result==(mode==2 ? TX_WAIT_ABORTED : (mode==1 || mode==4) ? TX_NO_EVENTS : TX_SUCCESS));
        if (callers[1].result==TX_SUCCESS) CHECK(callers[1].actual==5 && ip.nx_ip_events.tx_event_flags_group_current==4);
        finish();
    }
    /* Both clear waiters see the same set snapshot; selective wait retains a node. */
    for (unsigned mode=0;mode<3;mode++) {
        init(); worker(1,1,TX_OR_CLEAR,TX_WAIT_FOREVER);
        worker(2,mode==1 ? 3 : 1,mode==1 ? TX_AND_CLEAR : TX_OR_CLEAR,TX_WAIT_FOREVER);
        anx_tx_context_begin(&frame,&callers[0].thread,0);
        if (mode==2) CHECK(_tx_thread_wait_abort(&callers[1].thread)==TX_SUCCESS);
        CHECK(_tx_event_flags_set(&ip.nx_ip_events,1,TX_OR)==TX_SUCCESS);
        if (mode==1) {
            CHECK(ip.nx_ip_events.tx_event_flags_group_suspended_count==1);
            CHECK(ip.nx_ip_events.tx_event_flags_group_suspension_list==&callers[2].thread);
            CHECK(_tx_event_flags_set(&ip.nx_ip_events,3,TX_OR)==TX_SUCCESS);
        }
        anx_tx_context_end(&frame); release_workers(2);
        CHECK(callers[1].result==(mode==2 ? TX_WAIT_ABORTED : TX_SUCCESS));
        CHECK(callers[2].result==TX_SUCCESS && callers[2].actual==(mode==1 ? 3 : 1)); finish();
    }
    /* Real NetX periodic callback wakes an event waiter, then coalesces idle ticks. */
    init(); worker(1,NX_IP_ALL_EVENTS,TX_OR_CLEAR,50);
    anx_tx_context_begin(&frame,&callers[0].thread,0);
    CHECK(_tx_timer_create(&timer,(CHAR *)"NetX periodic",_nx_ip_periodic_timer_entry,(ULONG)(uintptr_t)&ip,2,2,TX_AUTO_ACTIVATE)==TX_SUCCESS);
    anx_tx_context_end(&frame);
    tick(); enter(0);
    CHECK(ip.nx_ip_events.tx_event_flags_group_suspended_count==1 &&
          callers[1].thread.tx_thread_timer.tx_timer_internal_remaining_ticks==50);
    leave(0);
    tick(); release_workers(1); CHECK(callers[1].result==TX_SUCCESS && callers[1].actual==NX_IP_PERIODIC_EVENT);
    tick(); tick(); tick(); tick();
    anx_tx_context_begin(&frame,&callers[0].thread,0);
    CHECK(_tx_time_get()==6 && ip.nx_ip_events.tx_event_flags_group_current==NX_IP_PERIODIC_EVENT);
    CHECK(_tx_timer_deactivate(&timer)==TX_SUCCESS);
    CHECK(_tx_event_flags_get(&ip.nx_ip_events,NX_IP_ALL_EVENTS,TX_OR_CLEAR,&actual,TX_NO_WAIT)==TX_SUCCESS);
    CHECK(_tx_timer_delete(&timer)==TX_SUCCESS); anx_tx_context_end(&frame);
    tick(); CHECK(!ip.nx_ip_events.tx_event_flags_group_current); finish();
    /* Partial countdown survives deactivate/reactivate; active change is ignored. */
    init(); calls=0; anx_tx_context_begin(&frame,&callers[0].thread,0);
    CHECK(_tx_timer_create(&timer,(CHAR *)"counter",count_callback,123,3,4,TX_AUTO_ACTIVATE)==TX_SUCCESS);
    CHECK(_tx_timer_activate(&timer)==TX_ACTIVATE_ERROR);
    CHECK(_tx_timer_change(&timer,8,9)==TX_SUCCESS && timer.tx_timer_internal.tx_timer_internal_remaining_ticks==3);
    anx_tx_context_end(&frame); tick();
    anx_tx_context_begin(&frame,&callers[0].thread,0);
    CHECK(_tx_timer_deactivate(&timer)==TX_SUCCESS && timer.tx_timer_internal.tx_timer_internal_remaining_ticks==2);
    anx_tx_context_end(&frame); tick(); tick(); CHECK(!calls);
    anx_tx_context_begin(&frame,&callers[0].thread,0); CHECK(_tx_timer_activate(&timer)==TX_SUCCESS); anx_tx_context_end(&frame);
    tick(); CHECK(!calls); tick(); CHECK(calls==1);
    for (unsigned i=0;i<4;i++) tick(); CHECK(calls==2);
    anx_tx_context_begin(&frame,&callers[0].thread,0);
    CHECK(_tx_timer_deactivate(&timer)==TX_SUCCESS && _tx_timer_change(&timer,1,0)==TX_SUCCESS && _tx_timer_activate(&timer)==TX_SUCCESS);
    anx_tx_context_end(&frame); tick(); tick(); CHECK(calls==3);
    anx_tx_context_begin(&frame,&callers[0].thread,0); CHECK(_tx_timer_delete(&timer)==TX_SUCCESS); anx_tx_context_end(&frame); finish();
    /* Self cancel, other expired cancel, delete active, empty and wrapping clock. */
    init(); calls=0; cancel_self=cancel_other=1;
    anx_tx_context_begin(&frame,&callers[0].thread,0);
    CHECK(_tx_timer_create(&timer,(CHAR *)"self",count_callback,123,1,2,TX_AUTO_ACTIVATE)==TX_SUCCESS);
    CHECK(_tx_timer_create(&other,(CHAR *)"other",count_callback,123,1,2,TX_AUTO_ACTIVATE)==TX_SUCCESS);
    anx_tx_context_end(&frame); tick(); tick(); tick(); CHECK(calls==1);
    anx_tx_context_begin(&frame,&callers[0].thread,0);
    CHECK(_tx_timer_delete(&timer)==TX_SUCCESS && _tx_timer_delete(&other)==TX_SUCCESS);
    CHECK(_tx_timer_create(&timer,(CHAR *)"null",TX_NULL,0,1,0,TX_AUTO_ACTIVATE)==TX_SUCCESS);
    anx_tx_context_end(&frame); tick();
    CHECK(!timer.tx_timer_internal.tx_timer_internal_list_head);
    anx_tx_context_begin(&frame,&callers[0].thread,0);
    CHECK(_tx_timer_delete(&timer)==TX_SUCCESS);
    CHECK(_tx_timer_create(&timer,(CHAR *)"active delete",count_callback,123,1,1,TX_AUTO_ACTIVATE)==TX_SUCCESS);
    CHECK(_tx_timer_delete(&timer)==TX_SUCCESS);
    _tx_timer_system_clock=(ULONG)-1;
    anx_tx_context_end(&frame); tick();
    anx_tx_context_begin(&frame,&callers[0].thread,0); CHECK(_tx_time_get()==0); anx_tx_context_end(&frame);
    finish();
    CHECK(!pthread_mutex_destroy(&lock));
    puts("research_event_timer_model=PASS event10 timer3 real ThreadX event bodies/NetX periodic callback");
    return 0;
}
