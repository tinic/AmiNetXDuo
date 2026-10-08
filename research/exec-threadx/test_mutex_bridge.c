/* Concurrent host tasks, real TX/NX control blocks and pinned timeout/cleanup.
 * Packet delivery is a decoded-payload fixture, not wire coverage.
 * SPDX-License-Identifier: MIT */
#define NX_SOURCE_CODE
#include "netx_resume.h"
#include "nx_api.h"
#include "nx_tcp.h"
#include "tx_thread.h"
#include "tx_mutex.h"
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
    UINT result,second;
    NX_PACKET *received,*second_packet;
    void (*body)(Caller *);
};
static Caller callers[3];
static pthread_mutex_t lock;
static pthread_cond_t progress=PTHREAD_COND_INITIALIZER;
static _Thread_local uintptr_t identity;
static _Thread_local unsigned depth;
static TX_MUTEX mutex,other;
static NX_IP ip;
static NX_TCP_SOCKET socket;
static NX_PACKET packet;
static unsigned mode,order[2],order_count;
enum { HANDOFF, RECURSIVE, TIMEOUT, ABORT, STALE, FIFO, EXPIRE_FIRST, RELEASE_FIRST,
       NETX_ABORT, NETX_ARRIVAL, NETX_REARM, NETX_GRACE, NETX_CALLER_CANCEL };
static void enter(void *a) { (void)a; CHECK(!pthread_mutex_lock(&lock)); depth++; }
static void leave(void *a) { (void)a; CHECK(depth); depth--; CHECK(!pthread_mutex_unlock(&lock)); }
static uintptr_t caller(void *a) { (void)a; return identity; }
static void panic(void *a,const char *s) { (void)a; fprintf(stderr,"panic: %s\n",s); abort(); }
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
    memset(callers,0,sizeof(callers)); memset(&mutex,0,sizeof(mutex)); memset(&other,0,sizeof(other));
    memset(&ip,0,sizeof(ip)); memset(&socket,0,sizeof(socket)); memset(&packet,0,sizeof(packet));
    order_count=0; anx_tx_runtime_init(&platform); attach(&callers[0],1);
    anx_tx_context_begin(&frame,&callers[0].thread,0);
    CHECK(_tx_mutex_create(&mutex,(CHAR *)"mutex",TX_NO_INHERIT)==TX_SUCCESS);
    CHECK(_tx_mutex_create(&other,(CHAR *)"other",TX_NO_INHERIT)==TX_SUCCESS);
    CHECK(_tx_mutex_create(&ip.nx_ip_protection,(CHAR *)"IP",TX_NO_INHERIT)==TX_SUCCESS);
    CHECK(_tx_event_flags_create(&ip.nx_ip_events,(CHAR *)"events")==TX_SUCCESS);
    anx_tx_context_end(&frame);
    socket.nx_tcp_socket_id=NX_TCP_ID; socket.nx_tcp_socket_state=NX_TCP_ESTABLISHED;
    socket.nx_tcp_socket_ip_ptr=&ip; socket.nx_tcp_socket_created_next=&socket;
    ip.nx_ip_tcp_created_sockets_count=1; ip.nx_ip_tcp_created_sockets_ptr=&socket;
    packet.nx_packet_length=3;
}
static void finish(void)
{
    AnxTxContext frame;
    CHECK(!depth);anx_tx_context_begin(&frame,&callers[0].thread,0);
    CHECK(_tx_mutex_delete(&mutex)==TX_SUCCESS && _tx_mutex_delete(&other)==TX_SUCCESS &&
          _tx_mutex_delete(&ip.nx_ip_protection)==TX_SUCCESS && _tx_event_flags_delete(&ip.nx_ip_events)==TX_SUCCESS);
    anx_tx_context_end(&frame);
    CHECK(anx_tx_detach(&callers[0].bridge)); CHECK(!pthread_cond_destroy(&callers[0].wake));
    CHECK(!mutex.tx_mutex_owner && !mutex.tx_mutex_suspended_count && !mutex.tx_mutex_suspension_list);
    CHECK(!ip.nx_ip_protection.tx_mutex_owner && !ip.nx_ip_protection.tx_mutex_suspended_count);
    CHECK(!socket.nx_tcp_socket_receive_suspended_count && !socket.nx_tcp_socket_receive_suspension_list);
}
static void getter(Caller *c)
{
    AnxTxContext frame;
    CHECK(anx_tx_set_abort_policy(&c->bridge,anx_netx_receive_abort_policy));
    anx_tx_context_begin(&frame,&c->thread,0);
    CHECK(_tx_mutex_get(&mutex,TX_NO_WAIT)==TX_NOT_AVAILABLE && !c->parks);
    c->result=_tx_mutex_get(&mutex,mode==TIMEOUT ? 2 :
        (mode==EXPIRE_FIRST || mode==RELEASE_FIRST) ? 50 : TX_WAIT_FOREVER);
    if (c->result==TX_SUCCESS) {
        CHECK(mutex.tx_mutex_owner==&c->thread && mutex.tx_mutex_ownership_count==1);
        CHECK(c->thread.tx_thread_owned_mutex_count==1 && c->thread.tx_thread_owned_mutex_list==&mutex);
        CHECK(order_count<2); order[order_count++]=(unsigned)c->id;
        CHECK(_tx_mutex_get(&mutex,TX_NO_WAIT)==TX_SUCCESS && mutex.tx_mutex_ownership_count==2);
        CHECK(_tx_mutex_put(&mutex)==TX_SUCCESS && mutex.tx_mutex_ownership_count==1);
        anx_tx_context_end(&frame);
        CHECK(!anx_tx_detach(&c->bridge)); /* owns mutex, outside a call boundary */
        anx_tx_context_begin(&frame,&c->thread,0);
        CHECK(_tx_mutex_put(&mutex)==TX_SUCCESS);
    } else CHECK(!c->thread.tx_thread_owned_mutex_count && !c->thread.tx_thread_suspend_cleanup);
    anx_tx_context_end(&frame);
}
static void deliver(void)
{
    TX_THREAD *t=socket.nx_tcp_socket_receive_suspension_list;
    CHECK(t && socket.nx_tcp_socket_receive_suspended_count==1);
    *((NX_PACKET **)t->tx_thread_additional_suspend_info)=&packet;
    socket.nx_tcp_socket_receive_suspended_count--;
    _nx_tcp_socket_thread_resume(&socket.nx_tcp_socket_receive_suspension_list,NX_SUCCESS);
}
static UINT receive(Caller *c,NX_PACKET **out)
{
    CHECK(_tx_mutex_get(&ip.nx_ip_protection,TX_WAIT_FOREVER)==TX_SUCCESS);
    *out=NX_NULL; c->thread.tx_thread_additional_suspend_info=out;
    socket.nx_tcp_socket_receive_suspended_count++;
    _nx_tcp_socket_thread_suspend(&socket.nx_tcp_socket_receive_suspension_list,
        _nx_tcp_receive_cleanup,&socket,&ip.nx_ip_protection,50);
    return c->thread.tx_thread_suspend_status;
}
static void receiver(Caller *c)
{
    AnxTxContext frame;
    CHECK(anx_tx_set_resume_cleanup(&c->bridge,anx_netx_receive_abort_cleanup));
    CHECK(anx_tx_set_abort_policy(&c->bridge,anx_netx_receive_abort_policy));
    anx_tx_context_begin(&frame,&c->thread,0);
    c->result=receive(c,&c->received);
    if (mode==NETX_REARM) c->second=receive(c,&c->second_packet);
    anx_tx_context_end(&frame);
    if (mode==NETX_ARRIVAL) CHECK(!anx_tx_detach(&c->bridge)); /* READY, retained by pending abort */
}
static void aborter(Caller *c)
{
    AnxTxContext frame;
    anx_tx_context_begin(&frame,&c->thread,0);
    c->result=_tx_thread_wait_abort(&callers[1].thread);
    CHECK(!callers[1].bridge.abort_pins);
    anx_tx_context_end(&frame);
}
int main(void)
{
    pthread_mutexattr_t attr;
    CHECK(!pthread_mutexattr_init(&attr)); CHECK(!pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_RECURSIVE));
    CHECK(!pthread_mutex_init(&lock,&attr)); CHECK(!pthread_mutexattr_destroy(&attr));
    for (mode=HANDOFF;mode<=RELEASE_FIRST;mode++) {
        AnxTxContext frame,timer; init();
        anx_tx_context_begin(&frame,&callers[0].thread,0);
        CHECK(_tx_mutex_get(&mutex,TX_NO_WAIT)==TX_SUCCESS);
        CHECK(_tx_mutex_get(&other,TX_NO_WAIT)==TX_SUCCESS);
        CHECK(callers[0].thread.tx_thread_owned_mutex_count==2 && mutex.tx_mutex_owned_next==&other && other.tx_mutex_owned_next==&mutex);
        CHECK(_tx_mutex_put(&other)==TX_SUCCESS && callers[0].thread.tx_thread_owned_mutex_list==&mutex);
        if (mode==RECURSIVE) CHECK(_tx_mutex_get(&mutex,TX_NO_WAIT)==TX_SUCCESS);
        anx_tx_context_end(&frame);
        CHECK(!anx_tx_detach(&callers[0].bridge));
        spawn(1,getter); wait_for(&callers[1].parks,1);
        if (mode==FIFO) { spawn(2,getter); wait_for(&callers[2].parks,1); }
        anx_tx_context_begin(&frame,&callers[0].thread,0);
        TX_MUTEX snapshot=mutex;
        CHECK(_tx_mutex_delete(&mutex)==TX_FEATURE_NOT_ENABLED && !memcmp(&mutex,&snapshot,sizeof(mutex)));
        if (mode==STALE) {
            _tx_mutex_cleanup(&callers[1].thread,callers[1].thread.tx_thread_suspension_sequence-1);
            CHECK(mutex.tx_mutex_suspended_count==1 && callers[1].thread.tx_thread_suspend_cleanup==_tx_mutex_cleanup);
        }
        if (mode==ABORT) CHECK(_tx_thread_wait_abort(&callers[1].thread)==TX_SUCCESS);
        if (mode==EXPIRE_FIRST) {
            anx_tx_context_begin(&timer,TX_NULL,1);
            CHECK(anx_tx_expire(&callers[1].thread,callers[1].bridge.token)); anx_tx_context_end(&timer);
        }
        if (mode==RECURSIVE) {
            CHECK(_tx_mutex_put(&mutex)==TX_SUCCESS && mutex.tx_mutex_ownership_count==1);
            CHECK(mutex.tx_mutex_suspended_count==1 && callers[1].thread.tx_thread_state==TX_MUTEX_SUSP);
        }
        if (mode!=TIMEOUT) CHECK(_tx_mutex_put(&mutex)==TX_SUCCESS);
        if (mode==RELEASE_FIRST) {
            anx_tx_context_begin(&timer,TX_NULL,1);
            CHECK(!anx_tx_expire(&callers[1].thread,callers[1].bridge.token)); anx_tx_context_end(&timer);
        }
        anx_tx_context_end(&frame);
        wait_for(&callers[1].done,1);
        CHECK(callers[1].result==(mode==TIMEOUT || mode==EXPIRE_FIRST ? TX_NOT_AVAILABLE : mode==ABORT ? TX_WAIT_ABORTED : TX_SUCCESS));
        if (mode==TIMEOUT) {
            anx_tx_context_begin(&frame,&callers[0].thread,0);
            CHECK(mutex.tx_mutex_owner==&callers[0].thread && !mutex.tx_mutex_suspended_count);
            CHECK(_tx_mutex_put(&mutex)==TX_SUCCESS); anx_tx_context_end(&frame);
        }
        release_workers(mode==FIFO ? 2 : 1);
        if (mode==FIFO) CHECK(order_count==2 && order[0]==2 && order[1]==3);
        finish();
    }
    for (mode=NETX_ABORT;mode<=NETX_CALLER_CANCEL;mode++) {
        AnxTxContext frame,timer; init();
        spawn(1,receiver); wait_for(&callers[1].parks,1);
        anx_tx_context_begin(&frame,&callers[0].thread,0);
        CHECK(_tx_mutex_get(&ip.nx_ip_protection,TX_WAIT_FOREVER)==TX_SUCCESS);
        if (mode==NETX_GRACE) {
            uint32_t operation=callers[1].bridge.operation;
            anx_tx_context_begin(&timer,TX_NULL,1);
            CHECK(anx_tx_expire(&callers[1].thread,callers[1].bridge.token)); anx_tx_context_end(&timer);
            anx_tx_context_end(&frame);
            wait_for(&callers[1].parks,2);
            anx_tx_context_begin(&frame,&callers[0].thread,0);
            CHECK(callers[1].bridge.operation==operation && callers[1].thread.tx_thread_suspend_cleanup==_nx_tcp_cleanup_deferred);
        }
        anx_tx_context_end(&frame);
        spawn(2,aborter); wait_for(&callers[2].parks,1);
        anx_tx_context_begin(&frame,&callers[0].thread,0);
        CHECK(callers[1].bridge.abort_pins==1 && callers[1].thread.tx_thread_state==TX_TCP_IP);
        if (mode==NETX_ARRIVAL || mode==NETX_REARM) deliver();
        if (mode==NETX_CALLER_CANCEL) CHECK(_tx_thread_wait_abort(&callers[2].thread)==TX_SUCCESS);
        anx_tx_context_end(&frame);
        if (mode==NETX_ARRIVAL) wait_for(&callers[1].done,1);
        if (mode==NETX_REARM) wait_for(&callers[1].parks,2);
        if (mode==NETX_CALLER_CANCEL) wait_for(&callers[2].done,1);
        anx_tx_context_begin(&frame,&callers[0].thread,0);
        CHECK(_tx_mutex_put(&ip.nx_ip_protection)==TX_SUCCESS); anx_tx_context_end(&frame);
        wait_for(&callers[2].done,1);
        if (mode==NETX_REARM) wait_for(&callers[1].parks,3);
        if (mode==NETX_REARM || mode==NETX_CALLER_CANCEL) {
            anx_tx_context_begin(&frame,&callers[0].thread,0);
            CHECK(_tx_thread_wait_abort(&callers[1].thread)==TX_SUCCESS); anx_tx_context_end(&frame);
        }
        release_workers(2);
        CHECK(callers[2].result==(mode==NETX_ABORT || mode==NETX_GRACE ? TX_SUCCESS : TX_WAIT_ABORT_ERROR));
        CHECK(callers[1].result==(mode==NETX_ARRIVAL || mode==NETX_REARM ? NX_SUCCESS : TX_WAIT_ABORTED));
        CHECK(callers[1].received==(mode==NETX_ARRIVAL || mode==NETX_REARM ? &packet : NX_NULL));
        if (mode==NETX_REARM) CHECK(callers[1].second==TX_WAIT_ABORTED && !callers[1].second_packet);
        finish();
    }
    CHECK(!pthread_mutex_destroy(&lock));
    puts("research_mutex_bridge_model=PASS mutex8/8 + contended NetX abort5/5 concurrent schedules; fixture packet transfer");
    return 0;
}
