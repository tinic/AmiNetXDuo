/* Native second-task experiment with unchanged pinned NetX suspension code.
 * SPDX-License-Identifier: MIT */
#define NX_SOURCE_CODE
#include "tx_bridge_exec.h"
#include "netx_resume.h"
#include "exec_wait.h"
#include "nx_api.h"
#include "nx_tcp.h"
#include "nx_udp.h"
#include "nx_ip.h"
#include "tx_thread.h"
#include <proto/exec.h>
#include <proto/dos.h>
#include <clib/alib_protos.h>

enum { JOB_NONE, JOB_ARRIVAL, JOB_TIMEOUT, JOB_CLOSE, JOB_ABORT, JOB_EXPIRE_ARRIVAL, JOB_LATE_ABORT,
       JOB_UDP_ARRIVAL, JOB_UDP_TIMEOUT, JOB_UDP_ABORT, JOB_SLEEP,
       JOB_RX_ABORT_PACKET, JOB_RX_ABORT_PACKET_LOCKED };
static TX_THREAD owner_thread,worker_thread,abort_thread;
static AnxTxThread owner_bridge,worker_bridge,abort_bridge;
static AnxExecWait owner_wait,worker_wait,abort_wait;
static NX_IP ip;
static NX_TCP_SOCKET socket;
static NX_UDP_SOCKET udp;
static NX_PACKET packet,*received;
static ULONG packet_data[8];
static struct Task *parent,*worker,*abort_task;
static ULONG ack;
static volatile unsigned job,phase,stop,worker_ready,worker_done,abort_ready,abort_done;
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

static void udp_arrival(void)
{
    CHECK(udp.nx_udp_socket_receive_suspended_count==1);
    TX_THREAD *t=udp.nx_udp_socket_receive_suspension_list;
    CHECK(t==&owner_thread && t->tx_thread_suspended_next==t);
    udp.nx_udp_socket_receive_suspension_list=NX_NULL;
    udp.nx_udp_socket_receive_suspended_count--;
    t->tx_thread_suspend_cleanup=TX_NULL;
    *((NX_PACKET **)t->tx_thread_additional_suspend_info)=&packet;
    t->tx_thread_suspend_status=NX_SUCCESS;
    _tx_thread_preempt_disable++;
    _tx_thread_system_resume(t);
}

/* Already decoded TCP payload fixture. State/header/wire processing is outside
 * this smoke. An aborted receiver must not be selected for its ownership. */
static void tcp_packet_arrival(void)
{
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
        arrival();
    }
}

static void abort_entry(void)
{
    CHECK(anx_exec_wait_open(&abort_wait));
    CHECK(anx_tx_attach(&abort_bridge,&abort_thread,&abort_wait.wait,(uintptr_t)FindTask(0)));
    Forbid(); abort_ready=1; Signal(parent,ack); Permit();
    for (;;) {
        unsigned next,step,stopping;
        uint32_t token=anx_wait_begin(&abort_wait.wait,20000,0,0,0,0);
        CHECK(token && anx_exec_wait_run(&abort_wait,token)==ANX_WAIT_TIMEOUT);
        Forbid(); next=job; step=phase; stopping=stop; Permit();
        if (stopping) break;
        if ((next!=JOB_LATE_ABORT && next<JOB_RX_ABORT_PACKET) || step!=1) continue;
        AnxTxContext frame;
        anx_tx_context_begin(&frame,&abort_thread,0);
        CHECK(_tx_thread_identify()==&abort_thread);
        CHECK(!ip.nx_ip_protection.tx_mutex_ownership_count);
        if (next==JOB_RX_ABORT_PACKET_LOCKED)
            CHECK(_tx_mutex_get(&ip.nx_ip_protection,TX_WAIT_FOREVER)==TX_SUCCESS);
        abort_result=_tx_thread_wait_abort(&owner_thread);
        CHECK(abort_result==TX_SUCCESS && owner_thread.tx_thread_state==TX_READY);
        if (next==JOB_LATE_ABORT) {
            CHECK(owner_thread.tx_thread_suspend_cleanup==_nx_tcp_cleanup_deferred);
            CHECK(owner_bridge.pending_resume && socket.nx_tcp_socket_receive_suspended_count==1);
        } else {
            CHECK(!owner_thread.tx_thread_suspend_cleanup && !owner_bridge.pending_resume);
            CHECK(!socket.nx_tcp_socket_receive_suspended_count && !received);
            CHECK(owner_thread.tx_thread_suspend_status==TX_WAIT_ABORTED);
        }
        if (next==JOB_RX_ABORT_PACKET_LOCKED) {
            CHECK(ip.nx_ip_protection.tx_mutex_ownership_count==1);
            CHECK(ip.nx_ip_protection.tx_mutex_owner==&abort_thread);
            CHECK(_tx_mutex_put(&ip.nx_ip_protection)==TX_SUCCESS);
        }
        phase=2;
        anx_tx_context_end(&frame);
    }
    CHECK(anx_tx_detach(&abort_bridge));
    CHECK(anx_exec_wait_close(&abort_wait));
    Forbid(); abort_done=1; Signal(parent,ack); RemTask(0);
    for (;;) {}
}

static void worker_entry(void)
{
    CHECK(anx_exec_wait_open(&worker_wait));
    CHECK(anx_tx_attach(&worker_bridge,&worker_thread,&worker_wait.wait,(uintptr_t)FindTask(0)));
    Forbid(); worker_ready=1; Signal(parent,ack); Permit();
    for (;;) {
        unsigned next, step, stopping;
        uint32_t token=anx_wait_begin(&worker_wait.wait,20000,0,0,0,0);
        CHECK(token && anx_exec_wait_run(&worker_wait,token)==ANX_WAIT_TIMEOUT);
        Forbid(); next=job; step=phase; stopping=stop; Permit();
        if (stopping) break;
        if (!next || next==JOB_UDP_TIMEOUT) continue;
        if ((next==JOB_LATE_ABORT || next>=JOB_RX_ABORT_PACKET) && step!=2) {
            if (!step) {
                AnxTxContext timer;
                anx_tx_context_begin(&timer,TX_NULL,1);
                CHECK(anx_tx_expire(&owner_thread,owner_bridge.token));
                phase=1;
                anx_tx_context_end(&timer);
            }
            continue;
        }
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
        if (next==JOB_TIMEOUT || next==JOB_LATE_ABORT) {
            _nx_tcp_deferred_cleanup_check(&ip);
            if (next==JOB_LATE_ABORT) {
                CHECK(!owner_thread.tx_thread_suspend_cleanup && owner_bridge.pending_resume);
                phase=3;
            }
        } else if (next>=JOB_RX_ABORT_PACKET) {
            CHECK(owner_thread.tx_thread_suspend_status==TX_WAIT_ABORTED && !received);
            CHECK(!owner_thread.tx_thread_suspend_cleanup && !owner_bridge.pending_resume);
            tcp_packet_arrival();
            _nx_tcp_deferred_cleanup_check(&ip);
            CHECK(socket.nx_tcp_socket_receive_queue_head==&packet && !received);
            phase=3;
        }
        else if (next==JOB_CLOSE) {
            socket.nx_tcp_socket_state=NX_TCP_CLOSED;
            _nx_tcp_receive_cleanup(&owner_thread NX_CLEANUP_ARGUMENT);
            _nx_tcp_receive_cleanup(&owner_thread NX_CLEANUP_ARGUMENT);
        } else if (next==JOB_ABORT || next==JOB_UDP_ABORT) abort_result=_tx_thread_wait_abort(&owner_thread);
        else if (next==JOB_UDP_ARRIVAL) udp_arrival();
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
    abort_task=CreateTask((CONST_STRPTR)"research abort caller",0,(APTR)abort_entry,8192);
    CHECK(abort_task!=0);
    while (!worker_ready || !abort_ready) Wait(ack);
    say("research_tx_bridge=WORKER_READY\n");
    for (scenario=JOB_ARRIVAL;scenario<=JOB_RX_ABORT_PACKET_LOCKED;scenario++) {
        unsigned before=owner_bridge.resumes;
        anx_tx_context_begin(&frame,&owner_thread,0);
        if (scenario==JOB_SLEEP) {
            CHECK(_tx_thread_sleep(2)==TX_SUCCESS && owner_bridge.resumes==before+1);
            CHECK(owner_thread.tx_thread_state==TX_READY && !owner_thread.tx_thread_timer.tx_timer_internal_list_head);
            anx_tx_context_end(&frame);
            passed++; say("research_tx_bridge=CASE_PASS\n"); continue;
        }
        socket.nx_tcp_socket_state=NX_TCP_ESTABLISHED;
        ip.nx_ip_events.tx_event_flags_group_current=0;
        phase=0; job=scenario;
        if (scenario>=JOB_UDP_ARRIVAL && scenario<=JOB_UDP_ABORT) {
            udp.nx_udp_socket_id=NX_UDP_ID;
            udp.nx_udp_socket_bound_next=&udp;
            udp.nx_udp_socket_ip_ptr=&ip;
            udp.nx_udp_socket_disable_checksum=NX_TRUE;
            packet.nx_packet_prepend_ptr=(UCHAR *)packet_data;
            packet.nx_packet_length=sizeof(NX_UDP_HEADER)+3;
            packet.nx_packet_append_ptr=packet.nx_packet_prepend_ptr+packet.nx_packet_length;
            packet.nx_packet_ip_version=NX_IP_VERSION_V4;
            packet.nx_packet_address.nx_packet_interface_ptr=&ip.nx_ip_interface[0];
            UINT result=_nx_udp_socket_receive(&udp,&received,scenario==JOB_UDP_TIMEOUT ? 2 : TX_WAIT_FOREVER);
            CHECK(result==(scenario==JOB_UDP_TIMEOUT ? NX_NO_PACKET : scenario==JOB_UDP_ABORT ? TX_WAIT_ABORTED : NX_SUCCESS));
            if (scenario==JOB_UDP_ARRIVAL) CHECK(received==&packet && packet.nx_packet_length==3 && packet.nx_packet_prepend_ptr==(UCHAR *)packet_data+sizeof(NX_UDP_HEADER));
            else CHECK(received==NX_NULL);
            CHECK(!udp.nx_udp_socket_receive_suspension_list && !udp.nx_udp_socket_receive_suspended_count);
            CHECK(!ip.nx_ip_events.tx_event_flags_group_current);
            if (scenario==JOB_UDP_TIMEOUT) job=JOB_NONE; /* no IP callback for direct cleanup */
        } else {
            if (scenario>=JOB_RX_ABORT_PACKET) {
                CHECK(anx_tx_set_resume_cleanup(&owner_bridge,anx_netx_receive_abort_cleanup));
                received=NX_NULL;
                owner_thread.tx_thread_additional_suspend_info=&received;
                packet.nx_packet_prepend_ptr=(UCHAR *)packet_data;
                packet.nx_packet_length=3;
            }
            CHECK(_tx_mutex_get(&ip.nx_ip_protection,TX_WAIT_FOREVER)==TX_SUCCESS);
            socket.nx_tcp_socket_receive_suspended_count++;
            _nx_tcp_socket_thread_suspend(&socket.nx_tcp_socket_receive_suspension_list,
                _nx_tcp_receive_cleanup,&socket,&ip.nx_ip_protection,
                scenario==JOB_TIMEOUT ? 2 : (scenario==JOB_EXPIRE_ARRIVAL || scenario==JOB_LATE_ABORT || scenario>=JOB_RX_ABORT_PACKET) ? 50 : TX_WAIT_FOREVER);
            CHECK(!socket.nx_tcp_socket_receive_suspension_list && !socket.nx_tcp_socket_receive_suspended_count);
            CHECK(owner_thread.tx_thread_suspend_status==(scenario==JOB_TIMEOUT ? NX_NO_PACKET :
                  scenario==JOB_CLOSE ? NX_NOT_CONNECTED : (scenario==JOB_ABORT || scenario==JOB_LATE_ABORT || scenario>=JOB_RX_ABORT_PACKET) ? TX_WAIT_ABORTED : NX_SUCCESS));
            if (scenario==JOB_LATE_ABORT) CHECK(phase==3 && !owner_bridge.pending_resume);
            if (scenario>=JOB_RX_ABORT_PACKET) {
                /* Synchronous abort can wake this task before the later packet
                 * producer. Wait outside the serialized boundary for it. */
                CHECK(!received);
                anx_tx_context_end(&frame);
                for (unsigned attempt=0;attempt<50;attempt++) {
                    unsigned done;
                    Forbid(); done=phase==3 && job==JOB_NONE; Permit();
                    if (done) break;
                    uint32_t token=anx_wait_begin(&owner_wait.wait,20000,0,0,0,0);
                    CHECK(token && anx_exec_wait_run(&owner_wait,token)==ANX_WAIT_TIMEOUT);
                }
                anx_tx_context_begin(&frame,&owner_thread,0);
                CHECK(phase==3 && job==JOB_NONE && !received);
                CHECK(socket.nx_tcp_socket_receive_queue_head==&packet && socket.nx_tcp_socket_receive_queue_tail==&packet);
                CHECK(socket.nx_tcp_socket_receive_queue_count==1 && packet.nx_packet_length==3);
                CHECK(_tx_mutex_get(&ip.nx_ip_protection,TX_WAIT_FOREVER)==TX_SUCCESS);
                socket.nx_tcp_socket_receive_queue_head=NX_NULL;
                socket.nx_tcp_socket_receive_queue_tail=NX_NULL;
                socket.nx_tcp_socket_receive_queue_count--;
                CHECK(_tx_mutex_put(&ip.nx_ip_protection)==TX_SUCCESS);
                CHECK(anx_tx_set_resume_cleanup(&owner_bridge,0));
            }
        }
        CHECK(_tx_thread_identify()==&owner_thread);
        CHECK(!owner_thread.tx_thread_suspend_cleanup && !owner_thread.tx_thread_timer.tx_timer_internal_list_head);
        CHECK(owner_thread.tx_thread_state==TX_READY && owner_bridge.resumes==before+1);
        if (scenario==JOB_ABORT || scenario==JOB_LATE_ABORT || scenario==JOB_UDP_ABORT || scenario>=JOB_RX_ABORT_PACKET) CHECK(abort_result==TX_SUCCESS);
        CHECK(!job && !_tx_thread_preempt_disable);
        anx_tx_context_end(&frame);
        passed++;
        say("research_tx_bridge=CASE_PASS\n");
    }
    Forbid(); stop=1; Permit();
    while (!worker_done || !abort_done) Wait(ack);
    CHECK(anx_tx_detach(&owner_bridge));
    CHECK(anx_exec_wait_close(&owner_wait));
    FreeSignal(bit);
    CHECK(!_tx_thread_current_ptr && !_tx_thread_system_state && !_tx_thread_preempt_disable);
    say(passed==12 ? "research_tx_bridge=PASS checks=12/12 workers_reaped=2\n" : "research_tx_bridge=FAIL\n");
    return passed==12 ? 0 : 20;
}
