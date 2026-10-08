/* Concurrent actual packet allocation/cleanup/release and UDP producer.
 * LP64 host packet headers are model layouts, not wire/checksum ABI evidence.
 * SPDX-License-Identifier: MIT */
#define NX_SOURCE_CODE
#include "netx_resume.h"
#include "nx_packet.h"
#include "nx_udp.h"
#include "nx_ip.h"
#include "tx_thread.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c); abort(); } } while (0)
#define PAYLOAD 256
#define STRIDE (((sizeof(NX_PACKET)+PAYLOAD+NX_PACKET_ALIGNMENT-1)/NX_PACKET_ALIGNMENT)*NX_PACKET_ALIGNMENT)
typedef struct Caller Caller;
struct Caller {
    TX_THREAD thread; AnxTxThread bridge; AnxWait wait; AnxWaitOps ops;
    pthread_cond_t wake; pthread_t task; uintptr_t id;
    unsigned parks,done,allow_exit,acquired,allow_release;
    UINT result; ULONG ticks,offset; NX_PACKET *received;
    void (*body)(Caller *);
};
static Caller callers[3];
static pthread_mutex_t lock;
static pthread_cond_t progress=PTHREAD_COND_INITIALIZER;
static _Thread_local uintptr_t identity;
static _Thread_local unsigned depth;
static NX_PACKET_POOL pool,other_pool;
static union { ULONG align; UCHAR bytes[3*STRIDE]; } arena;
static union { ULONG align; UCHAR bytes[STRIDE]; } other_arena;
static NX_PACKET *held[3];
static NX_IP ip;
static NX_UDP_SOCKET socket;
static const char *reject;
static void enter(void *a) { (void)a; CHECK(!pthread_mutex_lock(&lock)); depth++; }
static void leave(void *a) { (void)a; CHECK(depth); depth--; CHECK(!pthread_mutex_unlock(&lock)); }
static uintptr_t caller(void *a) { (void)a; return identity; }
static void panic(void *a,const char *s)
{
    (void)a;
    if (reject && !strcmp(reject,s)) {
        CHECK(!pool.nx_packet_pool_suspended_count && !pool.nx_packet_pool_suspension_list &&
              !callers[0].thread.tx_thread_suspend_cleanup && callers[0].thread.tx_thread_state==TX_READY);
        CHECK(pool.nx_packet_pool_id==NX_PACKET_POOL_ID && _nx_packet_pool_created_count==1);
        puts("research_packet_pool=PASS guard rejected before mutation"); exit(0);
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
static const AnxTxPlatform platform={enter,leave,caller,panic,0,0};
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
    CHECK(!_nx_packet_pool_created_count);
    memset(callers,0,sizeof(callers)); memset(&ip,0,sizeof(ip)); memset(&socket,0,sizeof(socket));
    memset(held,0,sizeof(held)); anx_tx_runtime_init(&platform); attach(&callers[0],1);
    anx_tx_context_begin(&frame,&callers[0].thread,0);
    _nx_packet_pool_initialize();
    CHECK(_nx_packet_pool_create(&pool,(CHAR *)"owned",PAYLOAD,arena.bytes,sizeof(arena.bytes))==NX_SUCCESS);
    CHECK(pool.nx_packet_pool_total==3 && pool.nx_packet_pool_available==3);
    CHECK(_tx_mutex_create(&ip.nx_ip_protection,(CHAR *)"IP",TX_NO_INHERIT)==TX_SUCCESS);
    socket.nx_udp_socket_id=NX_UDP_ID; socket.nx_udp_socket_ip_ptr=&ip;
    socket.nx_udp_socket_bound_next=socket.nx_udp_socket_bound_previous=&socket;
    socket.nx_udp_socket_port=1234; socket.nx_udp_socket_queue_maximum=2;
    socket.nx_udp_socket_disable_checksum=NX_TRUE;
    ip.nx_ip_udp_port_table[(1234+(1234>>8))&NX_UDP_PORT_TABLE_MASK]=&socket;
    anx_tx_context_end(&frame);
}
static void exhaust(void)
{
    for (unsigned i=0;i<3;i++) CHECK(_nx_packet_allocate(&pool,&held[i],0,NX_NO_WAIT)==NX_SUCCESS);
    NX_PACKET *absent=(NX_PACKET *)(uintptr_t)1;
    CHECK(_nx_packet_allocate(&pool,&absent,0,NX_NO_WAIT)==NX_NO_PACKET && !absent);
    CHECK(!pool.nx_packet_pool_available);
}
static void finish(void)
{
    AnxTxContext frame;
    anx_tx_context_begin(&frame,&callers[0].thread,0);
    for (unsigned i=0;i<3;i++) if (held[i]) CHECK(_nx_packet_release(held[i])==NX_SUCCESS);
    CHECK(!pool.nx_packet_pool_suspension_list && !pool.nx_packet_pool_suspended_count);
    CHECK(pool.nx_packet_pool_available==pool.nx_packet_pool_total);
    NX_PACKET *p=pool.nx_packet_pool_available_list;
    unsigned count=0;
    for (;p;p=p->nx_packet_queue_next) {
        CHECK(p->nx_packet_pool_owner==&pool && p->nx_packet_union_next.nx_packet_tcp_queue_next==(NX_PACKET *)NX_PACKET_FREE);
        CHECK(++count<=3);
    }
    CHECK(count==3 && !socket.nx_udp_socket_receive_head && !socket.nx_udp_socket_receive_suspension_list);
    CHECK(_nx_packet_pool_delete(&pool)==NX_SUCCESS && !_nx_packet_pool_created_count && !_nx_packet_pool_created_ptr);
    CHECK(_tx_mutex_delete(&ip.nx_ip_protection)==TX_SUCCESS);
    anx_tx_context_end(&frame);
    CHECK(anx_tx_detach(&callers[0].bridge)); CHECK(!pthread_cond_destroy(&callers[0].wake));
    CHECK(!depth && !_tx_thread_preempt_disable && !_tx_thread_current_ptr && !_tx_thread_system_state);
}
static void allocator(Caller *c)
{
    AnxTxContext frame;
    CHECK(anx_tx_set_abort_policy(&c->bridge,anx_netx_receive_abort_policy));
    anx_tx_context_begin(&frame,&c->thread,0);
    c->result=_nx_packet_allocate(&pool,&c->received,c->offset,c->ticks);
    CHECK(!c->thread.tx_thread_suspend_cleanup && !c->thread.tx_thread_timer.tx_timer_internal_list_head);
    CHECK(c->thread.tx_thread_state==TX_READY);
    if (c->result==NX_SUCCESS) {
        CHECK(c->received && c->received->nx_packet_pool_owner==&pool);
        CHECK(c->received->nx_packet_prepend_ptr==c->received->nx_packet_data_start+c->offset && !c->received->nx_packet_length);
        CHECK(c->received->nx_packet_union_next.nx_packet_tcp_queue_next==(NX_PACKET *)NX_PACKET_ALLOCATED);
    } else CHECK(!c->received);
    c->acquired=1; CHECK(!pthread_cond_broadcast(&progress));
    anx_tx_context_end(&frame);
    if (c->result==NX_SUCCESS) {
        wait_for(&c->allow_release,1);
        anx_tx_context_begin(&frame,&c->thread,0);
        CHECK(_nx_packet_release(c->received)==NX_SUCCESS);
        anx_tx_context_end(&frame);
    }
}
static void worker(unsigned index,ULONG ticks)
{
    callers[index].offset=index*sizeof(ULONG); callers[index].ticks=ticks;
    spawn(index,allocator); wait_for(&callers[index].parks,1);
}
static void grant(unsigned index)
{
    enter(0); callers[index].allow_release=1; CHECK(!pthread_cond_broadcast(&progress)); leave(0);
}
static NX_PACKET *make_udp(void)
{
    NX_PACKET *p;
    NX_UDP_HEADER header={((ULONG)5678<<16)|1234,((ULONG)(sizeof(NX_UDP_HEADER)+3)<<16)};
    CHECK(_nx_packet_allocate(&pool,&p,sizeof(NX_IPV4_HEADER),NX_NO_WAIT)==NX_SUCCESS);
    CHECK(_nx_packet_data_append(p,&header,sizeof(header),&pool,NX_NO_WAIT)==NX_SUCCESS);
    CHECK(_nx_packet_data_append(p,(VOID *)"abc",3,&pool,NX_NO_WAIT)==NX_SUCCESS);
    p->nx_packet_ip_header=p->nx_packet_data_start;
    memset(p->nx_packet_ip_header,0,sizeof(NX_IPV4_HEADER));
    p->nx_packet_ip_version=NX_IP_VERSION_V4;
    p->nx_packet_address.nx_packet_interface_ptr=&ip.nx_ip_interface[0];
    return p;
}
static void receiver(Caller *c)
{
    AnxTxContext frame;
    CHECK(anx_tx_set_abort_policy(&c->bridge,anx_netx_receive_abort_policy));
    anx_tx_context_begin(&frame,&c->thread,0);
    c->result=_nx_udp_socket_receive(&socket,&c->received,TX_WAIT_FOREVER);
    CHECK(c->result==NX_SUCCESS && c->received->nx_packet_length==3 && !memcmp(c->received->nx_packet_prepend_ptr,"abc",3));
    CHECK(_nx_packet_release(c->received)==NX_SUCCESS);
    anx_tx_context_end(&frame);
}
int main(int argc,char **argv)
{
    pthread_mutexattr_t attr; AnxTxContext frame,timer;
    CHECK(!pthread_mutexattr_init(&attr)); CHECK(!pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_RECURSIVE));
    CHECK(!pthread_mutex_init(&lock,&attr)); CHECK(!pthread_mutexattr_destroy(&attr));
    if (argc==2) {
        init(); anx_tx_context_begin(&frame,&callers[0].thread,0); exhaust();
        if (!strcmp(argv[1],"--reject-outstanding-delete")) {
            reject="packet pool deletion before quiescence"; (void)_nx_packet_pool_delete(&pool);
        } else {
            reject="unsupported NetX blocking context";
            if (!strcmp(argv[1],"--reject-timer-allocate")) anx_tx_context_begin(&timer,TX_NULL,1);
            else if (!strcmp(argv[1],"--reject-nested-allocate")) anx_tx_context_begin(&timer,&callers[0].thread,0);
            else if (!strcmp(argv[1],"--reject-preempt-allocate")) _tx_thread_preempt_disable++;
            else CHECK(0);
            NX_PACKET *p; (void)_nx_packet_allocate(&pool,&p,0,1);
        }
        CHECK(0);
    }
    /* Handoff, actual finite timeout, abort, expiry-before-release, release-before-expiry. */
    for (unsigned mode=0;mode<5;mode++) {
        init(); anx_tx_context_begin(&frame,&callers[0].thread,0); exhaust(); anx_tx_context_end(&frame);
        worker(1,mode==1 ? 2 : mode>=3 ? 50 : TX_WAIT_FOREVER);
        anx_tx_context_begin(&frame,&callers[0].thread,0);
        uint32_t token=callers[1].bridge.token;
        if (mode==2) CHECK(_tx_thread_wait_abort(&callers[1].thread)==TX_SUCCESS);
        if (mode==3) {
            anx_tx_context_begin(&timer,TX_NULL,1); CHECK(anx_tx_expire(&callers[1].thread,token)); anx_tx_context_end(&timer);
        }
        if (mode!=1) {
            NX_PACKET *p=held[0]; CHECK(_nx_packet_release(p)==NX_SUCCESS); held[0]=NX_NULL;
            if (mode==0 || mode==4) CHECK(callers[1].received==p && !pool.nx_packet_pool_available);
        }
        if (mode==4) {
            anx_tx_context_begin(&timer,TX_NULL,1); CHECK(!anx_tx_expire(&callers[1].thread,token)); anx_tx_context_end(&timer);
        }
        anx_tx_context_end(&frame); grant(1); release_workers(1);
        CHECK(callers[1].result==(mode==2 ? TX_WAIT_ABORTED : mode==1 || mode==3 ? NX_NO_PACKET : NX_SUCCESS)); finish();
    }
    /* FIFO ownership, then cancel the head and release only to the tail. */
    for (unsigned abort_head=0;abort_head<2;abort_head++) {
        init(); anx_tx_context_begin(&frame,&callers[0].thread,0); exhaust(); anx_tx_context_end(&frame);
        worker(1,TX_WAIT_FOREVER); worker(2,TX_WAIT_FOREVER);
        anx_tx_context_begin(&frame,&callers[0].thread,0);
        if (abort_head) CHECK(_tx_thread_wait_abort(&callers[1].thread)==TX_SUCCESS);
        NX_PACKET *p=held[0]; CHECK(_nx_packet_release(p)==NX_SUCCESS); held[0]=NX_NULL;
        CHECK(pool.nx_packet_pool_suspended_count==(abort_head ? 0U : 1U));
        CHECK(callers[abort_head ? 2 : 1].received==p);
        anx_tx_context_end(&frame);
        if (!abort_head) { grant(1); wait_for(&callers[2].acquired,1); }
        grant(2); release_workers(2);
        CHECK(callers[1].result==(abort_head ? TX_WAIT_ABORTED : NX_SUCCESS) && callers[2].result==NX_SUCCESS); finish();
    }
    /* Actual copy and chain release restore two distinct pools. */
    init(); anx_tx_context_begin(&frame,&callers[0].thread,0);
    NX_PACKET *p,*q; CHECK(_nx_packet_allocate(&pool,&p,0,NX_NO_WAIT)==NX_SUCCESS);
    CHECK(_nx_packet_data_append(p,(VOID *)"owned payload",13,&pool,NX_NO_WAIT)==NX_SUCCESS);
    p->nx_packet_ip_header=p->nx_packet_data_start;
    CHECK(_nx_packet_copy(p,&q,&pool,NX_NO_WAIT)==NX_SUCCESS && q!=p && q->nx_packet_length==13 && !memcmp(q->nx_packet_prepend_ptr,"owned payload",13));
    CHECK(_nx_packet_release(q)==NX_SUCCESS);
    CHECK(_nx_packet_pool_create(&other_pool,(CHAR *)"other",PAYLOAD,other_arena.bytes,sizeof(other_arena.bytes))==NX_SUCCESS);
    CHECK(_nx_packet_allocate(&other_pool,&q,0,NX_NO_WAIT)==NX_SUCCESS);
    p->nx_packet_next=q; p->nx_packet_last=q;
    CHECK(_nx_packet_release(p)==NX_SUCCESS && other_pool.nx_packet_pool_available==1);
    CHECK(_nx_packet_release(p)==NX_PTR_ERROR && pool.nx_packet_pool_available==3); /* double release doesn't mint a packet */
    CHECK(_nx_packet_pool_delete(&other_pool)==NX_SUCCESS);
    anx_tx_context_end(&frame); finish();
    /* Actual UDP producer queue and direct handoff, all packets returned to pool. */
    for (unsigned blocked=0;blocked<2;blocked++) {
        init();
        if (blocked) { spawn(1,receiver); wait_for(&callers[1].parks,1); }
        anx_tx_context_begin(&frame,&callers[0].thread,0);
        p=make_udp(); _nx_udp_packet_receive(&ip,p);
        if (!blocked) {
            CHECK(socket.nx_udp_socket_receive_count==1 && socket.nx_udp_socket_receive_head==p);
            CHECK(_nx_udp_socket_receive(&socket,&q,NX_NO_WAIT)==NX_SUCCESS && q==p && q->nx_packet_length==3);
            CHECK(!memcmp(q->nx_packet_prepend_ptr,"abc",3) && _nx_packet_release(q)==NX_SUCCESS);
        }
        anx_tx_context_end(&frame);
        if (blocked) release_workers(1);
        finish();
    }
    CHECK(!pthread_mutex_destroy(&lock));
    puts("research_packet_pool=PASS allocation7 ownership1 UDP2 actual vendor services; host checksum disabled"); return 0;
}
