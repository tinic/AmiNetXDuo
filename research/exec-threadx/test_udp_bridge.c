/* Real pinned UDP receive/cleanup, deterministic delivery fixture.
 * This is not wire-input or checksum conformance. SPDX-License-Identifier: MIT */
#define NX_SOURCE_CODE
#include "netx_resume.h"
#include "nx_api.h"
#include "nx_udp.h"
#include "tx_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c); exit(1); } } while (0)
enum { ARRIVAL, TIMEOUT, ABORT_WAIT, UNBIND, TWO_WAITERS, STALE_EXPIRY };
typedef struct {
    TX_THREAD thread;
    AnxTxThread bridge;
    AnxWait wait;
    AnxWaitOps ops;
    NX_PACKET packet, *received;
    ULONG data[8];
    unsigned parks;
} Caller;
static Caller callers[3];
static AnxTxPlatform platform;
static NX_IP ip;
static NX_UDP_SOCKET socket;
static unsigned depth,mode,stage;
static uintptr_t owner;
static uint64_t now;
static uint32_t stale;

static void enter(void *arg) { (void)arg; depth++; }
static void leave(void *arg) { (void)arg; CHECK(depth); depth--; }
static uintptr_t caller(void *arg) { (void)arg; return owner; }
static void panic(void *arg,const char *text) { (void)arg; fprintf(stderr,"panic: %s\n",text); exit(1); }
static uint64_t clock_now(void *arg) { (void)arg; CHECK(depth); return now; }
static void notify(void *arg) { (void)arg; CHECK(depth); }

static void packet_init(Caller *c)
{
    memset(&c->packet,0,sizeof(c->packet)); memset(c->data,0,sizeof(c->data));
    c->packet.nx_packet_prepend_ptr=(UCHAR *)c->data;
    c->packet.nx_packet_length=sizeof(NX_UDP_HEADER)+3;
    c->packet.nx_packet_append_ptr=(UCHAR *)c->data+c->packet.nx_packet_length;
    c->packet.nx_packet_ip_version=NX_IP_VERSION_V4;
    c->packet.nx_packet_address.nx_packet_interface_ptr=&ip.nx_ip_interface[0];
}

/* Only list removal/delivery is a fixture. Receive publication, header removal,
 * status handling and timeout/abort cleanup execute unchanged NetX sources. */
static void deliver(Caller *c)
{
    TX_THREAD *t=socket.nx_udp_socket_receive_suspension_list;
    CHECK(t==&c->thread && t->tx_thread_suspend_cleanup==_nx_udp_receive_cleanup);
    if (t->tx_thread_suspended_next==t) socket.nx_udp_socket_receive_suspension_list=NX_NULL;
    else {
        socket.nx_udp_socket_receive_suspension_list=t->tx_thread_suspended_next;
        t->tx_thread_suspended_next->tx_thread_suspended_previous=t->tx_thread_suspended_previous;
        t->tx_thread_suspended_previous->tx_thread_suspended_next=t->tx_thread_suspended_next;
    }
    socket.nx_udp_socket_receive_suspended_count--;
    t->tx_thread_suspend_cleanup=TX_NULL;
    *((NX_PACKET **)t->tx_thread_additional_suspend_info)=&c->packet;
    t->tx_thread_suspend_status=NX_SUCCESS;
    _tx_thread_preempt_disable++;
    _tx_thread_system_resume(t);
}

static UINT receive(unsigned index,ULONG ticks)
{
    Caller *c=&callers[index];
    AnxTxContext frame;
    anx_tx_context_begin(&frame,&c->thread,0);
    UINT result=_nx_udp_socket_receive(&socket,&c->received,ticks);
    CHECK(_tx_thread_identify()==&c->thread);
    CHECK(!c->thread.tx_thread_suspend_cleanup && c->thread.tx_thread_state==TX_READY);
    CHECK(!c->thread.tx_thread_timer.tx_timer_internal_list_head && !_tx_thread_preempt_disable);
    if (result==NX_SUCCESS) {
        CHECK(c->received==&c->packet && c->packet.nx_packet_length==3);
        CHECK(c->packet.nx_packet_prepend_ptr==(UCHAR *)c->data+sizeof(NX_UDP_HEADER));
    } else CHECK(c->received==NX_NULL);
    anx_tx_context_end(&frame);
    return result;
}

static int park(void *arg,uint64_t deadline)
{
    Caller *c=arg;
    uintptr_t saved=owner;
    AnxTxContext frame;
    CHECK(!depth && !_tx_thread_current_ptr && !_tx_thread_preempt_disable);
    c->parks++;
    if (mode==TIMEOUT) { CHECK(deadline==40000); now=deadline; return 0; }
    if (mode==TWO_WAITERS && !stage++) {
        owner=2; CHECK(receive(1,2)==NX_NO_PACKET); owner=saved; return 0;
    }
    owner=3;
    if (mode==TWO_WAITERS || mode==STALE_EXPIRY) {
        anx_tx_context_begin(&frame,TX_NULL,1);
        if (mode==STALE_EXPIRY) CHECK(!anx_tx_expire(&c->thread,stale));
        else {
            CHECK(anx_tx_expire(&callers[0].thread,callers[0].bridge.token));
            CHECK(anx_tx_expire(&callers[1].thread,callers[1].bridge.token));
            CHECK(!socket.nx_udp_socket_receive_suspended_count);
        }
        anx_tx_context_end(&frame);
    }
    if (mode!=TWO_WAITERS) {
        anx_tx_context_begin(&frame,&callers[2].thread,0);
        if (mode==ABORT_WAIT) CHECK(_tx_thread_wait_abort(&c->thread)==TX_SUCCESS);
        else if (mode==UNBIND) {
            socket.nx_udp_socket_bound_next=NX_NULL;
            _nx_udp_receive_cleanup(&c->thread NX_CLEANUP_ARGUMENT);
            _nx_udp_receive_cleanup(&c->thread NX_CLEANUP_ARGUMENT);
        } else deliver(c);
        anx_tx_context_end(&frame);
    }
    owner=saved;
    return 1;
}

static void init(void)
{
    CHECK(!depth);
    memset(callers,0,sizeof(callers)); memset(&ip,0,sizeof(ip)); memset(&socket,0,sizeof(socket));
    platform=(AnxTxPlatform){enter,leave,caller,panic,0,0};
    anx_tx_runtime_init(&platform); now=0; stage=0;
    for (unsigned i=0;i<3;i++) {
        Caller *c=&callers[i]; owner=i+1;
        c->ops=(AnxWaitOps){enter,leave,clock_now,park,notify,c};
        anx_wait_init(&c->wait,&c->ops);
        CHECK(anx_tx_attach(&c->bridge,&c->thread,&c->wait,owner));
        CHECK(anx_tx_set_abort_policy(&c->bridge,anx_netx_receive_abort_policy));
        packet_init(c);
    }
    owner=1;
    socket.nx_udp_socket_id=NX_UDP_ID;
    socket.nx_udp_socket_ip_ptr=&ip;
    socket.nx_udp_socket_bound_next=&socket;
    socket.nx_udp_socket_disable_checksum=NX_TRUE;
}

static void finish(void)
{
    CHECK(!depth && !_tx_thread_current_ptr && !_tx_thread_system_state && !_tx_thread_preempt_disable);
    CHECK(!socket.nx_udp_socket_receive_suspension_list && !socket.nx_udp_socket_receive_suspended_count);
    CHECK(!ip.nx_ip_events.tx_event_flags_group_current); /* UDP timeout never defers */
    for (unsigned i=0;i<3;i++) { owner=i+1; CHECK(anx_tx_detach(&callers[i].bridge)); }
}

int main(void)
{
    init(); socket.nx_udp_socket_receive_head=&callers[0].packet;
    socket.nx_udp_socket_receive_tail=&callers[0].packet; socket.nx_udp_socket_receive_count=1;
    CHECK(receive(0,TX_NO_WAIT)==NX_SUCCESS && !callers[0].parks);
    CHECK(!socket.nx_udp_socket_receive_count && !socket.nx_udp_socket_receive_head && !socket.nx_udp_socket_receive_tail);
    finish();
    init(); CHECK(receive(0,TX_NO_WAIT)==NX_NO_PACKET && !callers[0].parks); finish();
    for (mode=ARRIVAL;mode<=STALE_EXPIRY;mode++) {
        init();
        if (mode==STALE_EXPIRY) {
            mode=ARRIVAL; CHECK(receive(0,2)==NX_SUCCESS);
            stale=callers[0].bridge.token; mode=STALE_EXPIRY; packet_init(&callers[0]);
        }
        CHECK(receive(0,2)==(mode==TIMEOUT || mode==TWO_WAITERS || mode==UNBIND ? NX_NO_PACKET :
                            mode==ABORT_WAIT ? TX_WAIT_ABORTED : NX_SUCCESS));
        CHECK(callers[0].bridge.resumes==(mode==STALE_EXPIRY ? 2U : 1U));
        if (mode==TIMEOUT) CHECK(callers[0].parks==1 && now==40000);
        finish();
    }
    puts("research_udp_bridge_model=PASS checks=8/8 real UDP receive/cleanup + timeout/wait_abort; delivery fixture, no checksum/wire conformance");
    return 0;
}
