/* Deterministic schedules executing unchanged pinned NetX/ThreadX sources.
 * Host layouts are not m68k ABI evidence. SPDX-License-Identifier: MIT */
#define NX_SOURCE_CODE
#include "tx_bridge.h"
#include "nx_api.h"
#include "nx_tcp.h"
#include "nx_ip.h"
#include "tx_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c); exit(1); } } while (0)
enum { ARRIVAL, TIMEOUT, CLOSE, ABORT_WAIT, EXPIRE_ARRIVAL, TWO_WAITERS, STALE_TIMER, DETACH_PENDING, EXPIRE_ABORT, LATE_ABORT, TWO_GATED };
typedef struct {
    TX_THREAD thread;
    AnxTxThread bridge;
    AnxWait wait;
    AnxWaitOps ops;
    unsigned parks, signals;
} Caller;
static Caller callers[3];
static AnxTxPlatform platform;
static NX_IP ip;
static NX_TCP_SOCKET socket;
static unsigned depth, mode, stage;
static uintptr_t owner;
static uint64_t now;
static uint32_t stale_token;
static unsigned aborted, reject_blocking_mutex, reject_timer_mutex, reject_stuck_cleanup;

static void enter(void *arg) { (void)arg; depth++; }
static void leave(void *arg) { (void)arg; CHECK(depth); depth--; }
static uintptr_t caller(void *arg) { (void)arg; return owner; }
static void panic(void *arg,const char *text)
{
    (void)arg;
    if (reject_stuck_cleanup && !strcmp(text,"cleanup did not complete within research grace")) {
        CHECK(callers[0].bridge.pending_resume && callers[0].thread.tx_thread_state==TX_READY);
        CHECK(callers[0].thread.tx_thread_suspend_cleanup==_nx_tcp_cleanup_deferred);
        CHECK(socket.nx_tcp_socket_receive_suspended_count==1);
        CHECK(now==ANX_TX_CLEANUP_GRACE_US && aborted==TX_SUCCESS);
        puts("research_tx_bridge_guard=PASS rejected stalled cleanup after bounded grace");
        exit(0);
    }
    if (reject_timer_mutex && !strcmp(text,"mutex get without registered thread context")) {
        CHECK(!_tx_thread_current_ptr && _tx_thread_system_state==1);
        CHECK(!ip.nx_ip_protection.tx_mutex_owner && !ip.nx_ip_protection.tx_mutex_ownership_count);
        puts("research_tx_bridge_guard=PASS rejected mutex get in marked timer context");
        exit(0);
    }
    if (reject_blocking_mutex && !strcmp(text,"mutex blocking while preemption disabled")) {
        CHECK(ip.nx_ip_protection.tx_mutex_owner==&callers[0].thread);
        CHECK(ip.nx_ip_protection.tx_mutex_ownership_count==1);
        CHECK(_tx_thread_current_ptr==&callers[2].thread);
        CHECK(_tx_thread_preempt_disable==1 && !ip.nx_ip_protection.tx_mutex_suspended_count);
        puts("research_tx_bridge_guard=PASS rejected mutex blocking with preemption disabled");
        exit(0);
    }
    fprintf(stderr,"bridge panic: %s\n",text);
    exit(1);
}
static uint64_t clock_now(void *arg) { (void)arg; CHECK(depth); return now; }
static void notify(void *arg) { Caller *c=arg; CHECK(depth); c->signals++; }

static void resume_arrival(void)
{
    CHECK(socket.nx_tcp_socket_receive_suspended_count);
    socket.nx_tcp_socket_receive_suspended_count--;
    _nx_tcp_socket_thread_resume(&socket.nx_tcp_socket_receive_suspension_list,NX_SUCCESS);
}

static UINT suspend_caller(unsigned index,ULONG timeout)
{
    Caller *c=&callers[index];
    AnxTxContext frame;
    anx_tx_context_begin(&frame,&c->thread,0);
    CHECK(_tx_thread_identify()==&c->thread);
    CHECK(_tx_mutex_get(&ip.nx_ip_protection,TX_WAIT_FOREVER)==TX_SUCCESS);
    socket.nx_tcp_socket_receive_suspended_count++;
    _nx_tcp_socket_thread_suspend(&socket.nx_tcp_socket_receive_suspension_list,
                                 _nx_tcp_receive_cleanup,&socket,&ip.nx_ip_protection,timeout);
    CHECK(_tx_thread_identify()==&c->thread);
    CHECK(c->thread.tx_thread_state==TX_READY && !c->thread.tx_thread_suspend_cleanup);
    CHECK(!c->thread.tx_thread_timer.tx_timer_internal_list_head);
    CHECK(!_tx_thread_preempt_disable);
    anx_tx_context_end(&frame);
    return c->thread.tx_thread_suspend_status;
}

static int park(void *arg,uint64_t deadline)
{
    Caller *c=arg;
    AnxTxContext frame;
    uintptr_t saved=owner;
    CHECK(!depth && !_tx_thread_current_ptr && !_tx_thread_preempt_disable);
    c->parks++;
    if (mode==TIMEOUT && !stage++) {
        CHECK(deadline!=ANX_WAIT_FOREVER); now=deadline; return 0;
    }
    if (mode==TWO_WAITERS && !stage) {
        stage=1; owner=2;
        CHECK(suspend_caller(1,2)==NX_NO_PACKET);
        owner=saved;
        return 0;
    }
    if (mode==TWO_GATED && stage==0) {
        stage=1; owner=2;
        CHECK(suspend_caller(1,2)==TX_WAIT_ABORTED);
        CHECK(callers[0].bridge.pending_resume && !callers[1].bridge.pending_resume);
        owner=saved; return 1;
    }
    if (mode==TWO_GATED && stage==1) {
        owner=3; anx_tx_context_begin(&frame,TX_NULL,1);
        CHECK(anx_tx_expire(&callers[0].thread,callers[0].bridge.token));
        CHECK(anx_tx_expire(&callers[1].thread,callers[1].bridge.token));
        anx_tx_context_end(&frame);
        anx_tx_context_begin(&frame,&callers[2].thread,0);
        CHECK(_tx_thread_wait_abort(&callers[0].thread)==TX_SUCCESS);
        CHECK(_tx_thread_wait_abort(&callers[1].thread)==TX_SUCCESS);
        anx_tx_context_end(&frame);
        CHECK(callers[0].bridge.pending_resume && callers[1].bridge.pending_resume);
        stage=2; owner=saved; return 1;
    }
    if (mode==TWO_GATED) {
        CHECK(deadline==ANX_TX_CLEANUP_GRACE_US && c->bridge.pending_resume);
        owner=3; anx_tx_context_begin(&frame,&callers[2].thread,0);
        CHECK(_tx_mutex_get(&ip.nx_ip_protection,TX_WAIT_FOREVER)==TX_SUCCESS);
        if (stage==2) {
            CHECK(c==&callers[1]);
            _nx_tcp_receive_cleanup(&c->thread NX_CLEANUP_ARGUMENT);
            CHECK(callers[0].thread.tx_thread_suspend_cleanup==_nx_tcp_cleanup_deferred);
        } else { CHECK(stage==3 && c==&callers[0]); _nx_tcp_deferred_cleanup_check(&ip); }
        CHECK(c->bridge.pending_resume && !c->thread.tx_thread_suspend_cleanup);
        CHECK(_tx_mutex_put(&ip.nx_ip_protection)==TX_SUCCESS);
        anx_tx_context_end(&frame);
        CHECK(!c->bridge.pending_resume && c->wait.result==ANX_WAIT_READY);
        if (stage==2) CHECK(callers[0].bridge.pending_resume && callers[0].wait.result==ANX_WAIT_TIMEOUT);
        stage++; owner=saved; return 1;
    }
    if (mode==LATE_ABORT && !stage++) {
        owner=3;
        anx_tx_context_begin(&frame,TX_NULL,1);
        CHECK(anx_tx_expire(&c->thread,c->bridge.token));
        anx_tx_context_end(&frame);
        owner=2;
        anx_tx_context_begin(&frame,&callers[1].thread,0);
        CHECK(!ip.nx_ip_protection.tx_mutex_ownership_count);
        aborted=_tx_thread_wait_abort(&c->thread);
        CHECK(aborted==TX_SUCCESS && c->thread.tx_thread_state==TX_READY);
        CHECK(c->thread.tx_thread_suspend_cleanup==_nx_tcp_cleanup_deferred);
        CHECK(c->bridge.pending_resume && c->wait.result==ANX_WAIT_TIMEOUT);
        stale_token=c->bridge.token;
        CHECK(!anx_tx_detach(&c->bridge));
        anx_tx_context_end(&frame);
        CHECK(c->bridge.pending_resume && socket.nx_tcp_socket_receive_suspended_count==1);
        owner=saved;
        return 1;
    }
    if (mode==LATE_ABORT) {
        CHECK(deadline==ANX_TX_CLEANUP_GRACE_US && c->bridge.pending_resume);
        CHECK(c->bridge.pending_token==c->bridge.token);
        CHECK(stale_token!=c->bridge.token);
        CHECK(!anx_wait_complete(&c->wait,stale_token,ANX_WAIT_READY));
        CHECK(c->wait.result==ANX_WAIT_PENDING && c->bridge.pending_resume);
        if (reject_stuck_cleanup) {
            owner=3;
            anx_tx_context_begin(&frame,&callers[2].thread,0);
            CHECK(_tx_mutex_get(&ip.nx_ip_protection,TX_WAIT_FOREVER)==TX_SUCCESS);
            socket.nx_tcp_socket_id=0; /* storage retained, simulated invalidation */
            _nx_tcp_deferred_cleanup_check(&ip);
            CHECK(c->thread.tx_thread_suspend_cleanup==_nx_tcp_cleanup_deferred);
            CHECK(_tx_mutex_put(&ip.nx_ip_protection)==TX_SUCCESS);
            anx_tx_context_end(&frame); owner=saved; now=deadline; return 0;
        }
        CHECK(c->wait.result==ANX_WAIT_PENDING && c->thread.tx_thread_state==TX_READY);
    }
    owner=3;
    if (mode==EXPIRE_ARRIVAL || mode==STALE_TIMER || mode==TWO_WAITERS || mode==EXPIRE_ABORT) {
        anx_tx_context_begin(&frame,TX_NULL,1);
        if (mode==STALE_TIMER) CHECK(!anx_tx_expire(&callers[0].thread,stale_token));
        else if (mode==TWO_WAITERS) {
            CHECK(anx_tx_expire(&callers[0].thread,callers[0].bridge.token));
            CHECK(anx_tx_expire(&callers[1].thread,callers[1].bridge.token));
            CHECK(!anx_tx_expire(&callers[0].thread,callers[0].bridge.token));
            CHECK(socket.nx_tcp_socket_receive_suspended_count==2);
        } else CHECK(anx_tx_expire(&c->thread,c->bridge.token));
        anx_tx_context_end(&frame);
    }
    anx_tx_context_begin(&frame,&callers[2].thread,0);
    CHECK(_tx_thread_identify()==&callers[2].thread);
    CHECK(_tx_mutex_get(&ip.nx_ip_protection,TX_WAIT_FOREVER)==TX_SUCCESS);
    if (mode==TIMEOUT || mode==TWO_WAITERS || mode==LATE_ABORT) {
        CHECK(ip.nx_ip_events.tx_event_flags_group_current & NX_IP_TCP_CLEANUP_DEFERRED);
        CHECK(callers[0].thread.tx_thread_suspend_cleanup==_nx_tcp_cleanup_deferred);
        _nx_tcp_deferred_cleanup_check(&ip);
        if (mode==LATE_ABORT) {
            CHECK(!c->thread.tx_thread_suspend_cleanup && c->bridge.pending_resume);
            CHECK(c->wait.result==ANX_WAIT_PENDING);
        }
    } else if (mode==CLOSE) {
        socket.nx_tcp_socket_state=NX_TCP_CLOSED;
        _nx_tcp_receive_cleanup(&c->thread NX_CLEANUP_ARGUMENT);
        _nx_tcp_receive_cleanup(&c->thread NX_CLEANUP_ARGUMENT);
    } else if (mode==ABORT_WAIT || mode==EXPIRE_ABORT || mode==LATE_ABORT || mode==TWO_GATED) {
        aborted=_tx_thread_wait_abort(&c->thread);
        if (mode==EXPIRE_ABORT) {
            CHECK(c->thread.tx_thread_state==TX_READY);
            CHECK(c->thread.tx_thread_suspend_cleanup==_nx_tcp_cleanup_deferred);
            CHECK(socket.nx_tcp_socket_receive_suspended_count==1);
            /* This producer is the IP actor: drain before its boundary Permit,
             * otherwise the ready owner can return with an attached node. */
            _nx_tcp_deferred_cleanup_check(&ip);
            CHECK(!c->thread.tx_thread_suspend_cleanup);
        }
    } else {
        if (mode==DETACH_PENDING) {
            CHECK(!anx_tx_detach(&c->bridge));
        }
        resume_arrival();
        if (mode==EXPIRE_ARRIVAL) _nx_tcp_deferred_cleanup_check(&ip);
    }
    CHECK(_tx_mutex_put(&ip.nx_ip_protection)==TX_SUCCESS);
    anx_tx_context_end(&frame);
    if (mode==LATE_ABORT) CHECK(!c->bridge.pending_resume && c->wait.result==ANX_WAIT_READY);
    owner=saved;
    return 1;
}

static void init(void)
{
    AnxTxContext frame;
    unsigned i;
    CHECK(!depth);
    memset(callers,0,sizeof(callers)); memset(&ip,0,sizeof(ip)); memset(&socket,0,sizeof(socket));
    owner=1; now=0; stage=0;
    platform=(AnxTxPlatform){enter,leave,caller,panic,0};
    anx_tx_runtime_init(&platform);
    for (i=0;i<3;i++) {
        Caller *c=&callers[i]; owner=i+1;
        c->ops=(AnxWaitOps){enter,leave,clock_now,park,notify,c};
        anx_wait_init(&c->wait,&c->ops);
        CHECK(anx_tx_attach(&c->bridge,&c->thread,&c->wait,owner));
    }
    owner=1;
    anx_tx_context_begin(&frame,&callers[0].thread,0);
    CHECK(_tx_mutex_create(&ip.nx_ip_protection,(CHAR *)"IP",TX_NO_INHERIT)==TX_SUCCESS);
    CHECK(_tx_event_flags_create(&ip.nx_ip_events,(CHAR *)"IP events")==TX_SUCCESS);
    anx_tx_context_end(&frame);
    socket.nx_tcp_socket_id=NX_TCP_ID;
    socket.nx_tcp_socket_state=NX_TCP_ESTABLISHED;
    socket.nx_tcp_socket_ip_ptr=&ip;
    socket.nx_tcp_socket_created_next=&socket;
    ip.nx_ip_tcp_created_sockets_count=1;
    ip.nx_ip_tcp_created_sockets_ptr=&socket;
}

static void finish(void)
{
    unsigned i;
    CHECK(!depth && !_tx_thread_current_ptr && !_tx_thread_system_state && !_tx_thread_preempt_disable);
    CHECK(!socket.nx_tcp_socket_receive_suspension_list && !socket.nx_tcp_socket_receive_suspended_count);
    CHECK(!ip.nx_ip_protection.tx_mutex_ownership_count);
    for (i=0;i<3;i++) {
        owner=i+1;
        CHECK(anx_tx_detach(&callers[i].bridge));
    }
}

static void early(TX_MUTEX *mutex)
{
    CHECK(mutex==&ip.nx_ip_protection && _tx_thread_preempt_disable==1);
    CHECK(callers[0].thread.tx_thread_suspending);
    resume_arrival();
    CHECK(_tx_thread_preempt_disable==1);
}

int main(int argc, char **argv)
{
    unsigned scenario;
    if (argc!=1) {
        CHECK(argc==2);
        if (!strcmp(argv[1],"--reject-stuck-cleanup")) {
            init(); mode=LATE_ABORT; reject_stuck_cleanup=1;
            (void)suspend_caller(0,2); CHECK(0);
        }
        if (!strcmp(argv[1],"--reject-timer-mutex")) {
            AnxTxContext frame;
            init(); reject_timer_mutex=1;
            anx_tx_context_begin(&frame,TX_NULL,1);
            (void)_tx_mutex_get(&ip.nx_ip_protection,TX_WAIT_FOREVER);
            CHECK(0);
        }
        if (!strcmp(argv[1],"--reject-blocking-mutex")) {
            AnxTxContext frame;
            init();
            anx_tx_context_begin(&frame,&callers[0].thread,0);
            CHECK(_tx_mutex_get(&ip.nx_ip_protection,TX_NO_WAIT)==TX_SUCCESS);
            anx_tx_context_end(&frame);
            owner=3; reject_blocking_mutex=1;
            anx_tx_context_begin(&frame,&callers[2].thread,0);
            _tx_thread_preempt_disable=1;
            (void)_tx_mutex_get(&ip.nx_ip_protection,TX_WAIT_FOREVER);
            CHECK(0);
        }
        CHECK(0);
    }
    init();
    {
        AnxTxContext outer,nested;
        anx_tx_context_begin(&outer,&callers[0].thread,0);
        CHECK(_tx_mutex_get(&ip.nx_ip_protection,TX_NO_WAIT)==TX_SUCCESS);
        CHECK(_tx_mutex_get(&ip.nx_ip_protection,TX_NO_WAIT)==TX_SUCCESS);
        anx_tx_context_begin(&nested,TX_NULL,1);
        CHECK(_tx_thread_identify()==TX_NULL && _tx_thread_system_state==1);
        anx_tx_context_end(&nested);
        CHECK(_tx_thread_identify()==&callers[0].thread && !_tx_thread_system_state);
        CHECK(_tx_mutex_put(&ip.nx_ip_protection)==TX_SUCCESS);
        CHECK(ip.nx_ip_protection.tx_mutex_ownership_count==1);
        anx_tx_context_end(&outer);
        owner=3;
        anx_tx_context_begin(&outer,&callers[2].thread,0);
        CHECK(_tx_mutex_get(&ip.nx_ip_protection,TX_NO_WAIT)==TX_NOT_AVAILABLE);
        CHECK(_tx_mutex_put(&ip.nx_ip_protection)==TX_NOT_OWNED);
        anx_tx_context_end(&outer);
        owner=1;
        anx_tx_context_begin(&outer,&callers[0].thread,0);
        CHECK(_tx_mutex_put(&ip.nx_ip_protection)==TX_SUCCESS);
        anx_tx_context_end(&outer);
        CHECK(_tx_thread_identify()==TX_NULL);
    }
    finish();
    init(); anx_tx_after_mutex_put=early;
    CHECK(suspend_caller(0,2)==NX_SUCCESS);
    CHECK(!callers[0].parks && callers[0].bridge.resumes==1);
    finish();
    for (scenario=ARRIVAL;scenario<=TWO_GATED;scenario++) {
        init(); mode=scenario;
        if (mode==STALE_TIMER) {
            mode=ARRIVAL; CHECK(suspend_caller(0,2)==NX_SUCCESS);
            stale_token=callers[0].bridge.token; mode=STALE_TIMER;
        }
        UINT result=suspend_caller(0,2);
        CHECK(result==(mode==TIMEOUT || mode==TWO_WAITERS ? NX_NO_PACKET :
                       mode==CLOSE ? NX_NOT_CONNECTED : (mode==ABORT_WAIT || mode==EXPIRE_ABORT || mode==LATE_ABORT || mode==TWO_GATED) ? TX_WAIT_ABORTED : NX_SUCCESS));
        if (mode==ABORT_WAIT || mode==EXPIRE_ABORT || mode==LATE_ABORT) CHECK(aborted==TX_SUCCESS);
        CHECK(callers[0].bridge.resumes==(mode==STALE_TIMER ? 2U : 1U));
        if (mode==TIMEOUT) CHECK(callers[0].parks==2 && now==40000);
        if (mode==LATE_ABORT) CHECK(callers[0].parks==2 && !callers[0].bridge.pending_resume);
        if (mode==TWO_WAITERS) CHECK(callers[0].thread.tx_thread_suspended_next==&callers[1].thread);
        finish();
    }
    puts("research_tx_bridge_model=PASS checks=13/13 real NetX publish/resume/cleanup/deferred + ThreadX timeout/wait_abort");
    return 0;
}
