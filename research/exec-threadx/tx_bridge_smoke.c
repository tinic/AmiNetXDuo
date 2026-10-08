/* Native second-task experiment with unchanged pinned NetX suspension code.
 * SPDX-License-Identifier: MIT */
#define NX_SOURCE_CODE
#include "tx_bridge_exec.h"
#include "exec_wait.h"
#include "nx_api.h"
#include "nx_tcp.h"
#include "nx_ip.h"
#include "tx_thread.h"
#include <proto/exec.h>
#include <proto/dos.h>
#include <clib/alib_protos.h>

enum { JOB_NONE, JOB_ARRIVAL, JOB_TIMEOUT, JOB_CLOSE, JOB_ABORT, JOB_EXPIRE_ARRIVAL };
static TX_THREAD owner_thread,worker_thread;
static AnxTxThread owner_bridge,worker_bridge;
static AnxExecWait owner_wait,worker_wait;
static NX_IP ip;
static NX_TCP_SOCKET socket;
static struct Task *parent,*worker;
static ULONG ack;
static volatile unsigned job,stop,worker_ready,worker_done;
static UINT abort_result;

static void say(const char *s)
{
    const char *end=s;
    while (*end) end++;
    (void)Write(Output(),(APTR)s,(LONG)(end-s));
    (void)Flush(Output());
}

#define CHECK(c) do { if (!(c)) { anx_tx_exec_platform()->panic(0,#c); } } while (0)

static void arrival(void)
{
    CHECK(socket.nx_tcp_socket_receive_suspended_count==1);
    socket.nx_tcp_socket_receive_suspended_count--;
    _nx_tcp_socket_thread_resume(&socket.nx_tcp_socket_receive_suspension_list,NX_SUCCESS);
}

static void worker_entry(void)
{
    CHECK(anx_exec_wait_open(&worker_wait));
    CHECK(anx_tx_attach(&worker_bridge,&worker_thread,&worker_wait.wait,(uintptr_t)FindTask(0)));
    Forbid(); worker_ready=1; Signal(parent,ack); Permit();
    for (;;) {
        unsigned next, stopping;
        uint32_t token=anx_wait_begin(&worker_wait.wait,20000,0,0,0,0);
        CHECK(token && anx_exec_wait_run(&worker_wait,token)==ANX_WAIT_TIMEOUT);
        Forbid(); next=job; stopping=stop; Permit();
        if (stopping) break;
        if (!next) continue;
        if (next==JOB_TIMEOUT) {
            ULONG events;
            Forbid(); events=ip.nx_ip_events.tx_event_flags_group_current; Permit();
            if (!(events & NX_IP_TCP_CLEANUP_DEFERRED)) continue;
        }
        if (next==JOB_EXPIRE_ARRIVAL) {
            AnxTxContext timer;
            anx_tx_context_begin(&timer,TX_NULL,1);
            CHECK(anx_tx_expire(&owner_thread,owner_bridge.token));
            CHECK(!anx_tx_expire(&owner_thread,owner_bridge.token));
            anx_tx_context_end(&timer);
        }
        AnxTxContext frame;
        anx_tx_context_begin(&frame,&worker_thread,0);
        CHECK(_tx_thread_identify()==&worker_thread);
        CHECK(_tx_mutex_get(&ip.nx_ip_protection,TX_WAIT_FOREVER)==TX_SUCCESS);
        if (next==JOB_TIMEOUT) _nx_tcp_deferred_cleanup_check(&ip);
        else if (next==JOB_CLOSE) {
            socket.nx_tcp_socket_state=NX_TCP_CLOSED;
            _nx_tcp_receive_cleanup(&owner_thread NX_CLEANUP_ARGUMENT);
            _nx_tcp_receive_cleanup(&owner_thread NX_CLEANUP_ARGUMENT);
        } else if (next==JOB_ABORT) abort_result=_tx_thread_wait_abort(&owner_thread);
        else {
            arrival();
            if (next==JOB_EXPIRE_ARRIVAL) _nx_tcp_deferred_cleanup_check(&ip);
        }
        job=JOB_NONE;
        CHECK(_tx_mutex_put(&ip.nx_ip_protection)==TX_SUCCESS);
        anx_tx_context_end(&frame);
    }
    CHECK(anx_tx_detach(&worker_bridge));
    CHECK(anx_exec_wait_close(&worker_wait));
    /* No task can run after Signal until RemTask discards this Forbid.
     * The parent may then release the executable; this task is already gone. */
    Forbid(); worker_done=1; Signal(parent,ack); RemTask(0);
    for (;;) {}
}

int main(void)
{
    AnxTxContext frame;
    BYTE bit;
    unsigned scenario,passed=0;
    say("research_tx_bridge=START\n");
    parent=FindTask(0);
    bit=AllocSignal(-1);
    if (bit<0) return 20;
    ack=1UL<<bit;
    anx_tx_runtime_init(anx_tx_exec_platform());
    CHECK(anx_exec_wait_open(&owner_wait));
    CHECK(anx_tx_attach(&owner_bridge,&owner_thread,&owner_wait.wait,(uintptr_t)parent));
    anx_tx_context_begin(&frame,&owner_thread,0);
    CHECK(_tx_mutex_create(&ip.nx_ip_protection,(CHAR *)"IP",TX_NO_INHERIT)==TX_SUCCESS);
    CHECK(_tx_event_flags_create(&ip.nx_ip_events,(CHAR *)"events")==TX_SUCCESS);
    anx_tx_context_end(&frame);
    socket.nx_tcp_socket_id=NX_TCP_ID;
    socket.nx_tcp_socket_ip_ptr=&ip;
    socket.nx_tcp_socket_created_next=&socket;
    ip.nx_ip_tcp_created_sockets_count=1;
    ip.nx_ip_tcp_created_sockets_ptr=&socket;
    worker=CreateTask((CONST_STRPTR)"research IP producer",0,(APTR)worker_entry,8192);
    CHECK(worker!=0);
    while (!worker_ready) Wait(ack);
    say("research_tx_bridge=WORKER_READY\n");
    for (scenario=JOB_ARRIVAL;scenario<=JOB_EXPIRE_ARRIVAL;scenario++) {
        unsigned before=owner_bridge.resumes;
        anx_tx_context_begin(&frame,&owner_thread,0);
        socket.nx_tcp_socket_state=NX_TCP_ESTABLISHED;
        ip.nx_ip_events.tx_event_flags_group_current=0;
        CHECK(_tx_mutex_get(&ip.nx_ip_protection,TX_WAIT_FOREVER)==TX_SUCCESS);
        socket.nx_tcp_socket_receive_suspended_count++;
        job=scenario;
        _nx_tcp_socket_thread_suspend(&socket.nx_tcp_socket_receive_suspension_list,
            _nx_tcp_receive_cleanup,&socket,&ip.nx_ip_protection,
            scenario==JOB_TIMEOUT ? 2 : scenario==JOB_EXPIRE_ARRIVAL ? 50 : TX_WAIT_FOREVER);
        CHECK(_tx_thread_identify()==&owner_thread);
        CHECK(!socket.nx_tcp_socket_receive_suspension_list && !socket.nx_tcp_socket_receive_suspended_count);
        CHECK(!owner_thread.tx_thread_suspend_cleanup && !owner_thread.tx_thread_timer.tx_timer_internal_list_head);
        CHECK(owner_thread.tx_thread_state==TX_READY && owner_bridge.resumes==before+1);
        CHECK(owner_thread.tx_thread_suspend_status==(scenario==JOB_TIMEOUT ? NX_NO_PACKET :
              scenario==JOB_CLOSE ? NX_NOT_CONNECTED : scenario==JOB_ABORT ? TX_WAIT_ABORTED : NX_SUCCESS));
        if (scenario==JOB_ABORT) CHECK(abort_result==TX_SUCCESS);
        CHECK(!job && !_tx_thread_preempt_disable);
        anx_tx_context_end(&frame);
        passed++;
        say("research_tx_bridge=CASE_PASS\n");
    }
    Forbid(); stop=1; Permit();
    while (!worker_done) Wait(ack);
    CHECK(anx_tx_detach(&owner_bridge));
    CHECK(anx_exec_wait_close(&owner_wait));
    FreeSignal(bit);
    CHECK(!_tx_thread_current_ptr && !_tx_thread_system_state && !_tx_thread_preempt_disable);
    say(passed==5 ? "research_tx_bridge=PASS checks=5/5 worker_reaped=1\n" : "research_tx_bridge=FAIL\n");
    return passed==5 ? 0 : 20;
}
