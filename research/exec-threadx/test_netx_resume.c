/* Actual ThreadX abort and NetX suspension/cleanup/resume. Packet delivery is a
 * fixture matching state_data_check's queue/pop/additional-info transfer, not
 * a wire/TCP state-machine test. SPDX-License-Identifier: MIT */
#define NX_SOURCE_CODE
#include "netx_resume.h"
#include "nx_api.h"
#include "nx_tcp.h"
#include "tx_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c); exit(1); } } while (0)
enum { BASELINE, SINGLE, HEAD, TAIL, BOTH, RECURSIVE, ARRIVAL_FIRST, ORDINARY, SLEEP_REUSE };
typedef struct {
    TX_THREAD thread;
    AnxTxThread bridge;
    AnxWait wait;
    AnxWaitOps ops;
    NX_PACKET *received;
} Caller;
static Caller callers[3];
static NX_IP ip;
static NX_TCP_SOCKET socket;
static NX_PACKET packet;
static UCHAR payload[3]={1,2,3};
static unsigned depth,mode,stage,reject;
static uintptr_t owner;
static uint64_t now;
static void enter(void *a) { (void)a; depth++; }
static void leave(void *a) { (void)a; CHECK(depth); depth--; }
static uintptr_t caller(void *a) { (void)a; return owner; }
static uint64_t clock_now(void *a) { (void)a; CHECK(depth); return now; }
static void notify(void *a) { (void)a; CHECK(depth); }
static void panic(void *a,const char *message)
{
    (void)a;
    if (reject && (!strcmp(message,"resume cleanup integration rejected") ||
                  (reject==4 && !strcmp(message,"blocking mutex contention not implemented")))) {
        CHECK(callers[0].thread.tx_thread_state==TX_SUSPENDED);
        CHECK(callers[0].thread.tx_thread_suspend_cleanup==_nx_tcp_cleanup_deferred);
        CHECK(callers[0].thread.tx_thread_suspend_status==TX_WAIT_ABORTED);
        CHECK(!callers[0].received && !callers[0].bridge.resumes);
        CHECK(socket.nx_tcp_socket_receive_suspended_count==1);
        if (reject==4) CHECK(ip.nx_ip_protection.tx_mutex_owner==&callers[1].thread &&
                             ip.nx_ip_protection.tx_mutex_ownership_count==1);
        puts("research_netx_resume_guard=PASS rejected invalid cleanup integration before READY");
        exit(0);
    }
    fprintf(stderr,"panic: %s\n",message); exit(1);
}
static const AnxTxPlatform platform={enter,leave,caller,panic,0,0};

/* The packet is an already decoded payload. Header/checksum/state processing
 * and packet pool release are outside this fixture's coverage. */
static void arrival(void)
{
    CHECK(ip.nx_ip_protection.tx_mutex_owner==_tx_thread_identify());
    CHECK(!socket.nx_tcp_socket_receive_queue_head);
    socket.nx_tcp_socket_receive_queue_head=&packet;
    socket.nx_tcp_socket_receive_queue_tail=&packet;
    socket.nx_tcp_socket_receive_queue_count=1;
    if (socket.nx_tcp_socket_receive_suspension_list) {
        TX_THREAD *t=socket.nx_tcp_socket_receive_suspension_list;
        socket.nx_tcp_socket_receive_queue_head=NX_NULL;
        socket.nx_tcp_socket_receive_queue_tail=NX_NULL;
        socket.nx_tcp_socket_receive_queue_count--;
        *((NX_PACKET **)t->tx_thread_additional_suspend_info)=&packet;
        socket.nx_tcp_socket_receive_suspended_count--;
        _nx_tcp_socket_thread_resume(&socket.nx_tcp_socket_receive_suspension_list,NX_SUCCESS);
    }
}
static UINT receive(unsigned index)
{
    Caller *c=&callers[index]; AnxTxContext frame;
    unsigned before=c->bridge.resumes;
    anx_tx_context_begin(&frame,&c->thread,0);
    CHECK(_tx_mutex_get(&ip.nx_ip_protection,TX_WAIT_FOREVER)==TX_SUCCESS);
    c->received=NX_NULL;
    c->thread.tx_thread_additional_suspend_info=&c->received;
    socket.nx_tcp_socket_receive_suspended_count++;
    _nx_tcp_socket_thread_suspend(&socket.nx_tcp_socket_receive_suspension_list,
        _nx_tcp_receive_cleanup,&socket,&ip.nx_ip_protection,50);
    CHECK(_tx_thread_identify()==&c->thread);
    CHECK(c->thread.tx_thread_state==TX_READY && !c->thread.tx_thread_suspend_cleanup);
    CHECK(!c->bridge.pending_resume && !c->bridge.pending_token);
    CHECK(c->bridge.resumes==before+1 && !c->thread.tx_thread_timer.tx_timer_internal_list_head);
    anx_tx_context_end(&frame);
    return c->thread.tx_thread_suspend_status;
}
static void expire(unsigned index)
{
    AnxTxContext frame;
    anx_tx_context_begin(&frame,TX_NULL,1);
    CHECK(anx_tx_expire(&callers[index].thread,callers[index].bridge.token));
    CHECK(!anx_tx_expire(&callers[index].thread,callers[index].bridge.token));
    anx_tx_context_end(&frame);
}
static void abort_wait(unsigned index)
{
    Caller *c=&callers[index];
    if (reject==1) c->bridge.sequence_at_suspend++;
    if (reject==2) c->bridge.cleanup_at_suspend=TX_NULL;
    if (reject==3) socket.nx_tcp_socket_id=0; /* retained storage */
    CHECK(_tx_thread_wait_abort(&c->thread)==TX_SUCCESS);
    if (mode!=BASELINE) {
        CHECK(!c->thread.tx_thread_suspend_cleanup && !c->bridge.pending_resume);
        CHECK(c->thread.tx_thread_suspend_status==TX_WAIT_ABORTED && !c->received);
    }
    CHECK(_tx_thread_wait_abort(&c->thread)==TX_WAIT_ABORT_ERROR);
}
static int park(void *a,uint64_t deadline)
{
    Caller *c=a; AnxTxContext frame; uintptr_t saved=owner;
    CHECK(!depth && !_tx_thread_current_ptr && !_tx_thread_preempt_disable);
    if (mode==SLEEP_REUSE) { CHECK(deadline==now+20000); now=deadline; return 0; }
    CHECK(deadline==now+1000000);
    CHECK(!anx_tx_set_resume_cleanup(&c->bridge,0)); /* pending/foreign mutation rejected */
    if ((mode==HEAD || mode==TAIL || mode==BOTH) && !stage) {
        stage=1; owner=2;
        CHECK(receive(1)==(mode==HEAD ? NX_SUCCESS : TX_WAIT_ABORTED));
        CHECK(callers[1].received==(mode==HEAD ? &packet : NX_NULL));
        owner=saved; return 1;
    }
    owner=3;
    if (mode!=ORDINARY && mode!=ARRIVAL_FIRST) {
        expire(mode==TAIL ? 1 : 0);
        if (mode==BOTH) expire(1);
    }
    if (reject==4) {
        owner=2; anx_tx_context_begin(&frame,&callers[1].thread,0);
        CHECK(_tx_mutex_get(&ip.nx_ip_protection,TX_WAIT_FOREVER)==TX_SUCCESS);
        anx_tx_context_end(&frame); owner=3;
    }
    anx_tx_context_begin(&frame,&callers[2].thread,0);
    if (mode==RECURSIVE || mode==ARRIVAL_FIRST)
        CHECK(_tx_mutex_get(&ip.nx_ip_protection,TX_WAIT_FOREVER)==TX_SUCCESS);
    if (mode==ARRIVAL_FIRST) {
        arrival(); CHECK(_tx_thread_wait_abort(&c->thread)==TX_WAIT_ABORT_ERROR);
    } else {
        abort_wait(mode==TAIL ? 1 : 0);
        if (mode==BOTH) abort_wait(1);
    }
    if (mode==RECURSIVE) {
        CHECK(ip.nx_ip_protection.tx_mutex_owner==&callers[2].thread);
        CHECK(ip.nx_ip_protection.tx_mutex_ownership_count==1);
    }
    if (mode==RECURSIVE || mode==ARRIVAL_FIRST)
        CHECK(_tx_mutex_put(&ip.nx_ip_protection)==TX_SUCCESS);
    anx_tx_context_end(&frame); /* non-IP abort finished before packet producer */
    anx_tx_context_begin(&frame,&callers[2].thread,0);
    CHECK(_tx_mutex_get(&ip.nx_ip_protection,TX_WAIT_FOREVER)==TX_SUCCESS);
    if (mode!=ARRIVAL_FIRST) arrival();
    _nx_tcp_deferred_cleanup_check(&ip);
    _nx_tcp_deferred_cleanup_check(&ip);
    CHECK(_tx_mutex_put(&ip.nx_ip_protection)==TX_SUCCESS);
    anx_tx_context_end(&frame);
    owner=saved; return 1;
}
static void init(void)
{
    AnxTxContext frame;
    memset(callers,0,sizeof(callers)); memset(&ip,0,sizeof(ip)); memset(&socket,0,sizeof(socket));
    memset(&packet,0,sizeof(packet)); stage=0; now=0;
    anx_tx_runtime_init(&platform);
    for (unsigned i=0;i<3;i++) {
        Caller *c=&callers[i]; owner=i+1;
        c->ops=(AnxWaitOps){enter,leave,clock_now,park,notify,c};
        anx_wait_init(&c->wait,&c->ops);
        CHECK(anx_tx_attach(&c->bridge,&c->thread,&c->wait,owner));
        if (mode!=BASELINE) CHECK(anx_tx_set_resume_cleanup(&c->bridge,anx_netx_receive_abort_cleanup));
    }
    owner=1; anx_tx_context_begin(&frame,&callers[0].thread,0);
    CHECK(_tx_mutex_create(&ip.nx_ip_protection,(CHAR *)"IP",TX_NO_INHERIT)==TX_SUCCESS);
    CHECK(_tx_event_flags_create(&ip.nx_ip_events,(CHAR *)"events")==TX_SUCCESS);
    anx_tx_context_end(&frame);
    socket.nx_tcp_socket_id=NX_TCP_ID; socket.nx_tcp_socket_state=NX_TCP_ESTABLISHED;
    socket.nx_tcp_socket_ip_ptr=&ip; socket.nx_tcp_socket_created_next=&socket;
    ip.nx_ip_tcp_created_sockets_count=1; ip.nx_ip_tcp_created_sockets_ptr=&socket;
    packet.nx_packet_prepend_ptr=payload; packet.nx_packet_length=sizeof(payload);
}
int main(int argc,char **argv)
{
    if (argc!=1) {
        CHECK(argc==2); mode=SINGLE;
        if (!strcmp(argv[1],"--reject-sequence")) reject=1;
        else if (!strcmp(argv[1],"--reject-callback")) reject=2;
        else if (!strcmp(argv[1],"--reject-invalid-socket")) reject=3;
        else if (!strcmp(argv[1],"--reject-contention")) reject=4;
        else CHECK(0);
        init(); (void)receive(0); CHECK(0);
    }
    for (mode=BASELINE;mode<=ORDINARY;mode++) {
        init();
        UINT result=receive(0);
        CHECK(result==(mode==BASELINE || mode==TAIL || mode==ARRIVAL_FIRST ? NX_SUCCESS : TX_WAIT_ABORTED));
        CHECK(callers[0].received==(result==NX_SUCCESS ? &packet : NX_NULL));
        CHECK(!socket.nx_tcp_socket_receive_suspension_list && !socket.nx_tcp_socket_receive_suspended_count);
        if (mode==SINGLE || mode==BOTH || mode==RECURSIVE || mode==ORDINARY) {
            CHECK(socket.nx_tcp_socket_receive_queue_head==&packet && socket.nx_tcp_socket_receive_queue_tail==&packet);
            CHECK(socket.nx_tcp_socket_receive_queue_count==1);
            CHECK(!callers[0].received && !callers[1].received);
            /* A later caller can claim the queued packet once. */
            socket.nx_tcp_socket_receive_queue_head=NX_NULL; socket.nx_tcp_socket_receive_queue_tail=NX_NULL;
            socket.nx_tcp_socket_receive_queue_count--;
        } else CHECK(!socket.nx_tcp_socket_receive_queue_count && !socket.nx_tcp_socket_receive_queue_head);
        CHECK(packet.nx_packet_length==3 && packet.nx_packet_prepend_ptr==payload);
        CHECK(!depth && !ip.nx_ip_protection.tx_mutex_ownership_count && !_tx_thread_preempt_disable);
        if (mode==SINGLE) {
            AnxTxContext frame;
            mode=SLEEP_REUSE;
            owner=1; anx_tx_context_begin(&frame,&callers[0].thread,0);
            CHECK(_tx_thread_sleep(1)==TX_SUCCESS);
            CHECK(!callers[0].bridge.cleanup_at_suspend);
            anx_tx_context_end(&frame);
            mode=SINGLE;
            CHECK(receive(0)==TX_WAIT_ABORTED && !callers[0].received);
            CHECK(callers[0].bridge.cleanup_at_suspend==_nx_tcp_receive_cleanup);
            CHECK(callers[0].bridge.control_at_suspend==&socket && callers[0].bridge.resumes==3);
            CHECK(socket.nx_tcp_socket_receive_queue_count==1 && socket.nx_tcp_socket_receive_queue_head==&packet);
            socket.nx_tcp_socket_receive_queue_head=NX_NULL; socket.nx_tcp_socket_receive_queue_tail=NX_NULL;
            socket.nx_tcp_socket_receive_queue_count--;
        }
        AnxTxContext frame;owner=1;anx_tx_context_begin(&frame,&callers[0].thread,0);
        CHECK(_tx_mutex_delete(&ip.nx_ip_protection)==TX_SUCCESS && _tx_event_flags_delete(&ip.nx_ip_events)==TX_SUCCESS);
        anx_tx_context_end(&frame);
        for (unsigned i=0;i<3;i++) { owner=i+1; CHECK(anx_tx_detach(&callers[i].bridge)); }
    }
    puts("research_netx_resume_model=PASS baseline overwrite reproduced; 8/8 integration schedules preserve status + fixture packet ownership");
    return 0;
}
