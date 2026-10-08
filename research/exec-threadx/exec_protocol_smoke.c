/* Real pinned IPv4 UDP/TCP loopback traffic, research backend only.
 * Disposable deterministic PRNG; no entropy, external wire or speed verdict.
 * SPDX-License-Identifier: MIT */
#include "exec_ip.h"
#include "tx_bridge_exec.h"
#include "tx_thread.h"
#include "tx_timer.h"
#include "tx_mutex.h"
#include "tx_event_flags.h"
#include "nx_ip.h"
#include "nx_packet.h"
#include "nx_tcp.h"
#include "nx_udp.h"
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
static AnxExecIp record;
static AnxExecClock clock_record;
static NX_IP ip;
static NX_PACKET_POOL pool;
static NX_UDP_SOCKET udp[2];
static NX_TCP_SOCKET tcp[2];
static TX_THREAD parent;
static AnxTxThread pb;
static AnxExecWait pw;
static struct {ULONG before,bytes[2048],after;} stacks[2];
static union {ULONG align;UBYTE bytes[65536];} arena;
static UBYTE payload[2048],received[2048];
static unsigned passed,errors,enabled,disabled,passive_closes;
static ULONG rng=1;
/* Deliberately repeatable fixture input, no production random replacement. */
int anx_research_rand(void) {rng=rng*1664525UL+1013904223UL;return (int)(rng&0x7fffffffUL);}
void anx_research_srand(unsigned int seed) {rng=(ULONG)seed+1;}
ULONG _nx_amiga_handshake_millis(VOID)
{
    anx_tx_require_context(0);
    if (clock_record.state!=ANX_CLOCK_RUNNING || !clock_record.wait.opened)
        anx_tx_unsupported("handshake clock unavailable");
    return (ULONG)(clock_record.wait.ops.clock(clock_record.wait.ops.context)/1000);
}
static void say(const char *s)
{
    const char *end=s;while (*end) end++;
    (void)Write(Output(),(APTR)s,(LONG)(end-s));(void)Flush(Output());
}
#define CHECK(x) do {if (!(x)) {say("research_exec_protocol=FAIL " #x "\n");return 20;}} while (0)
#define CASE(s) do {passed++;say("research_exec_protocol=CASE_PASS " s "\n");} while (0)
static VOID passive_close(NX_TCP_SOCKET *s)
{
    /* Actual IP helper calls this normal task callback after peer FIN. It
     * sends the passive FIN through unchanged NetX without blocking itself. */
    if (s!=&tcp[1] || tx_thread_identify()!=&ip.nx_ip_thread ||
        s->nx_tcp_socket_state!=NX_TCP_CLOSE_WAIT || nx_tcp_socket_disconnect(s,50)!=NX_SUCCESS)
        errors++;
    else passive_closes++;
}
static VOID driver(NX_IP_DRIVER *d)
{
    d->nx_ip_driver_status=NX_SUCCESS;
    switch (d->nx_ip_driver_command) {
    case NX_LINK_INTERFACE_ATTACH:break;
    case NX_LINK_INITIALIZE:
        d->nx_ip_driver_interface->nx_interface_ip_mtu_size=1500;
        d->nx_ip_driver_interface->nx_interface_address_mapping_needed=NX_FALSE;break;
    case NX_LINK_ENABLE:enabled++;d->nx_ip_driver_interface->nx_interface_link_up=NX_TRUE;break;
    case NX_LINK_DISABLE:
        disabled++;
        if (record.state!=ANX_IP_CLOSING || record.helper.state!=ANX_THREAD_FINISHED ||
            clock_record.state!=ANX_CLOCK_REAPED || record.helper.wait.opened || clock_record.wait.opened ||
            clock_record.wait.timer_sends!=clock_record.wait.timer_reaps ||
            ip.nx_ip_udp_packet_receive!=_nx_udp_packet_receive || ip.nx_ip_tcp_packet_receive!=_nx_tcp_packet_receive ||
            ip.nx_ip_periodic_timer.tx_timer_internal.tx_timer_internal_list_head ||
            ip.nx_ip_fast_periodic_timer.tx_timer_internal.tx_timer_internal_list_head ||
            anx_exec_ip_event(&record,NX_IP_FAST_EVENT)!=NX_NOT_ENABLED) errors++;
        d->nx_ip_driver_interface->nx_interface_link_up=NX_FALSE;break;
    case NX_LINK_UNINITIALIZE:break;
    /* NetX's internal loopback must bypass every physical packet-send. */
    default:errors++;d->nx_ip_driver_status=NX_UNHANDLED_COMMAND;break;
    }
}
static int parked(void)
{
    AnxTxContext f;int ready=0;
    for (unsigned i=0;i<200 && !ready;i++) {
        anx_tx_context_begin(&f,&parent,0);
        ready=record.helper.entered && ip.nx_ip_initialize_done &&
            ip.nx_ip_thread.tx_thread_state==TX_EVENT_FLAG && !ip.nx_ip_protection.tx_mutex_owner &&
            !ip.nx_ip_deferred_received_packet_head && !ip.nx_ip_tcp_queue_head &&
            pool.nx_packet_pool_available==pool.nx_packet_pool_total;
        if (!ready && tx_thread_sleep(1)!=TX_SUCCESS) errors++;
        anx_tx_context_end(&f);
    }
    return ready && !errors;
}
static int data_matches(NX_PACKET *packet,ULONG size,ULONG offset)
{
    ULONG copied=0;
    if (!packet || packet->nx_packet_length!=size || offset>sizeof(payload)-size) return 0;
    return nx_packet_data_retrieve(packet,received,&copied)==NX_SUCCESS && copied==size &&
           !memcmp(received,payload+offset,size);
}
static VOID rogue_udp(NX_IP *i,NX_PACKET *p)
{
    (void)i;(void)p;anx_tx_unsupported("unknown UDP fixture callback executed");
}
int main(void)
{
    AnxTxContext f;
    ULONG signals;
    NX_PACKET *packet,*out;
    say("research_exec_protocol=START\n");
    anx_tx_runtime_init(anx_tx_exec_platform());
    CHECK(anx_exec_wait_open(&pw) && anx_tx_attach(&pb,&parent,&pw.wait,(uintptr_t)FindTask(0)));
    parent.tx_thread_priority=parent.tx_thread_user_priority=16;
    parent.tx_thread_preempt_threshold=parent.tx_thread_user_preempt_threshold=16;
    signals=FindTask(0)->tc_SigAlloc;
    for (unsigned i=0;i<2;i++) {stacks[i].before=0x13572468;stacks[i].after=0x89abcdef;}
    for (unsigned i=0;i<sizeof(payload);i++) payload[i]=(UBYTE)(i^(i>>3)^0x5a);
    anx_tx_context_begin(&f,&parent,0);nx_system_initialize();
    CHECK(nx_packet_pool_create(&pool,(CHAR *)"protocol pool",1536,arena.bytes,sizeof(arena.bytes))==NX_SUCCESS);
    anx_tx_context_end(&f);
    for (unsigned cycle=0;cycle<2;cycle++) {
        CHECK(anx_exec_ip_create(&record,&parent,&ip,(CHAR *)"protocol IP",&pool,driver,stacks[0].bytes,8192,2)==NX_SUCCESS);
        CHECK(parked() && enabled==cycle+1);
        CHECK(anx_exec_ip_clock_start(&record,&clock_record,(CHAR *)"protocol clock",stacks[1].bytes,8192)==NX_SUCCESS);
        if (!cycle) CASE("real-IP-helper-and-automatic-clock-started");
        anx_tx_context_begin(&f,&parent,0);
        CHECK(nx_udp_enable(&ip)==NX_SUCCESS);
        for (unsigned i=0;i<2;i++) {
            CHECK(nx_udp_socket_create(&ip,&udp[i],(CHAR *)"UDP",NX_IP_NORMAL,NX_FRAGMENT_OKAY,64,4)==NX_SUCCESS);
            CHECK(nx_udp_socket_bind(&udp[i],9000+i,NX_NO_WAIT)==NX_SUCCESS);
        }
        CHECK(ip.nx_ip_udp_created_sockets_count==2 && ip.nx_ip_udp_packet_receive==_nx_udp_packet_receive);
        anx_tx_context_end(&f);
        if (!cycle) CASE("actual-UDP-enable-and-two-bound-sockets");
        CHECK(anx_exec_ip_delete(&record)==NX_SOCKETS_BOUND && record.state==ANX_IP_LIVE &&
              clock_record.state==ANX_CLOCK_RUNNING && record.helper.state==ANX_THREAD_BOUND);
        if (!cycle) CASE("live-sockets-refuse-IP-retirement-before-stop");
        unsigned before_parks=pb.parks;
        const ULONG sizes[]={129,513,1300};
        for (unsigned j=0;j<3;j++) {
            anx_tx_context_begin(&f,&parent,0);
            CHECK(nx_packet_allocate(&pool,&packet,NX_UDP_PACKET,NX_NO_WAIT)==NX_SUCCESS);
            CHECK(nx_packet_data_append(packet,payload,sizes[j],&pool,NX_NO_WAIT)==NX_SUCCESS);
            CHECK(nx_udp_socket_send(&udp[0],packet,IP_ADDRESS(192,0,2,1),9001)==NX_SUCCESS);
            CHECK(ip.nx_ip_deferred_received_packet_head && udp[1].nx_udp_socket_receive_count==0);
            CHECK(nx_udp_socket_receive(&udp[1],&out,50)==NX_SUCCESS && data_matches(out,sizes[j],0));
            CHECK(nx_packet_release(out)==NX_SUCCESS && !udp[1].nx_udp_socket_receive_suspended_count &&
                  !udp[1].nx_udp_socket_receive_suspension_list && !parent.tx_thread_suspend_cleanup);
            anx_tx_context_end(&f);
        }
        CHECK(pb.parks>=before_parks+3);
        if (!cycle) CASE("UDP-three-payloads-real-IP-checksum-path-and-blocked-receive");
        anx_tx_context_begin(&f,&parent,0);
        CHECK(nx_packet_allocate(&pool,&packet,NX_UDP_PACKET,NX_NO_WAIT)==NX_SUCCESS);
        CHECK(nx_packet_data_append(packet,payload,129,&pool,NX_NO_WAIT)==NX_SUCCESS);
        ULONG bad_before=ip.nx_ip_udp_checksum_errors;
        CHECK(nx_udp_socket_send(&udp[0],packet,IP_ADDRESS(192,0,2,1),9001)==NX_SUCCESS);
        /* Mutate the real vendor-generated queued copy, before the helper
         * can run; leave the original generated IP/UDP checksums intact. */
        packet=ip.nx_ip_deferred_received_packet_head;
        CHECK(packet && !packet->nx_packet_next && packet->nx_packet_length>28);
        packet->nx_packet_append_ptr[-1]^=1;
        out=0;
        CHECK(nx_udp_socket_receive(&udp[1],&out,4)==NX_NO_PACKET && !out &&
              ip.nx_ip_udp_checksum_errors==bad_before+1 && !parent.tx_thread_suspend_cleanup);
        for (unsigned i=0;i<2;i++) CHECK(nx_udp_socket_unbind(&udp[i])==NX_SUCCESS && nx_udp_socket_delete(&udp[i])==NX_SUCCESS);
        anx_tx_context_end(&f);
        CHECK(parked() && !ip.nx_ip_udp_created_sockets_count);
        if (!cycle) {CASE("corrupted-UDP-checksum-dropped-and-real-receive-timeout-cleaned");CASE("UDP-unbind-delete-and-packet-pool-recovered");}
        anx_tx_context_begin(&f,&parent,0);
        CHECK(nx_tcp_enable(&ip)==NX_SUCCESS && ip.nx_ip_fast_periodic_timer_created && _tx_timer_created_count==2);
        CHECK(nx_tcp_socket_create(&ip,&tcp[0],(CHAR *)"client",NX_IP_NORMAL,NX_FRAGMENT_OKAY,64,4096,0,0)==NX_SUCCESS);
        CHECK(nx_tcp_socket_create(&ip,&tcp[1],(CHAR *)"server",NX_IP_NORMAL,NX_FRAGMENT_OKAY,64,4096,0,passive_close)==NX_SUCCESS);
        CHECK(nx_tcp_server_socket_listen(&ip,9100,&tcp[1],4,0)==NX_SUCCESS);
        CHECK(nx_tcp_client_socket_bind(&tcp[0],9101,NX_NO_WAIT)==NX_SUCCESS);
        before_parks=pb.parks;
        CHECK(nx_tcp_client_socket_connect(&tcp[0],IP_ADDRESS(192,0,2,1),9100,NX_NO_WAIT)==NX_IN_PROGRESS);
        CHECK(nx_tcp_server_socket_accept(&tcp[1],50)==NX_SUCCESS);
        CHECK(tcp[0].nx_tcp_socket_state==NX_TCP_ESTABLISHED && tcp[1].nx_tcp_socket_state==NX_TCP_ESTABLISHED &&
              !tcp[1].nx_tcp_socket_connect_suspended_thread && !parent.tx_thread_suspend_cleanup && pb.parks>before_parks);
        anx_tx_context_end(&f);
        if (!cycle) {CASE("actual-TCP-enable-and-automatic-fast-timer");CASE("real-TCP-SYN-SYNACK-ACK-and-blocked-server-accept");}
        for (unsigned direction=0;direction<2;direction++) {
            anx_tx_context_begin(&f,&parent,0);
            CHECK(nx_packet_allocate(&pool,&packet,NX_TCP_PACKET,NX_NO_WAIT)==NX_SUCCESS);
            CHECK(nx_packet_data_append(packet,payload,193,&pool,NX_NO_WAIT)==NX_SUCCESS);
            CHECK(nx_tcp_socket_send(&tcp[direction],packet,50)==NX_SUCCESS);
            CHECK(nx_tcp_socket_receive(&tcp[1-direction],&out,50)==NX_SUCCESS && data_matches(out,193,0));
            CHECK(nx_packet_release(out)==NX_SUCCESS && !parent.tx_thread_suspend_cleanup);
            anx_tx_context_end(&f);
        }
        if (!cycle) CASE("TCP-bidirectional-data-through-real-IP-helper");
        anx_tx_context_begin(&f,&parent,0);
        CHECK(nx_packet_allocate(&pool,&packet,NX_TCP_PACKET,NX_NO_WAIT)==NX_SUCCESS);
        CHECK(nx_packet_data_append(packet,payload,sizeof(payload),&pool,NX_NO_WAIT)==NX_SUCCESS && packet->nx_packet_next);
        CHECK(nx_tcp_socket_send(&tcp[0],packet,50)==NX_SUCCESS);
        ULONG offset=0;
        while (offset<sizeof(payload)) {
            CHECK(nx_tcp_socket_receive(&tcp[1],&out,50)==NX_SUCCESS);
            ULONG length=out->nx_packet_length;
            CHECK(length && length<=sizeof(payload)-offset && data_matches(out,length,offset));
            offset+=length;CHECK(nx_packet_release(out)==NX_SUCCESS);
        }
        CHECK(!tcp[1].nx_tcp_socket_receive_suspended_count && !tcp[1].nx_tcp_socket_receive_suspension_list);
        anx_tx_context_end(&f);
        if (!cycle) CASE("TCP-chained-2048-byte-send-segmented-and-reassembled");
        anx_tx_context_begin(&f,&parent,0);out=0;
        CHECK(nx_tcp_socket_receive(&tcp[1],&out,4)==NX_NO_PACKET && !out && !parent.tx_thread_suspend_cleanup &&
              !tcp[1].nx_tcp_socket_receive_suspended_count && !tcp[1].nx_tcp_socket_receive_suspension_list);
        /* Run actual TCP fast and one-second processing while established. */
        ULONG tick=clock_record.ticks;before_parks=record.helper.bridge.resumes;
        CHECK(tx_thread_sleep(NX_IP_PERIODIC_RATE+2)==TX_SUCCESS && clock_record.ticks>=tick+NX_IP_PERIODIC_RATE &&
              record.helper.bridge.resumes>before_parks && !ip.nx_ip_tcp_checksum_errors && !ip.nx_ip_receive_checksum_errors);
        CHECK(nx_tcp_socket_disconnect(&tcp[0],100)==NX_SUCCESS && passive_closes==cycle+1 && !errors);
        CHECK(nx_tcp_client_socket_unbind(&tcp[0])==NX_SUCCESS);
        CHECK(nx_tcp_server_socket_unaccept(&tcp[1])==NX_SUCCESS && nx_tcp_server_socket_unlisten(&ip,9100)==NX_SUCCESS);
        for (unsigned i=0;i<2;i++) CHECK(nx_tcp_socket_delete(&tcp[i])==NX_SUCCESS);
        anx_tx_context_end(&f);
        CHECK(parked());
        if (!cycle) {
            CASE("TCP-receive-timeout-real-deferred-cleanup-completed");
            CASE("automatic-TCP-fast-and-periodic-maintenance-with-live-connection");
            CASE("orderly-TCP-FIN-exchange-and-blocked-active-disconnect");
            CASE("TCP-unbind-unaccept-unlisten-delete-and-ACK-packets-recovered");
        }
        BYTE priority=SetTaskPri(FindTask(0),2);
        anx_tx_context_begin(&f,&parent,0);ip.nx_ip_udp_packet_receive=rogue_udp;anx_tx_context_end(&f);
        CHECK(anx_exec_ip_delete(&record)==NX_NOT_ENABLED && record.state==ANX_IP_LIVE && clock_record.state==ANX_CLOCK_RUNNING);
        anx_tx_context_begin(&f,&parent,0);ip.nx_ip_udp_packet_receive=_nx_udp_packet_receive;
        CHECK(nx_packet_allocate(&pool,&packet,0,NX_NO_WAIT)==NX_SUCCESS);anx_tx_context_end(&f);
        CHECK(anx_exec_ip_delete(&record)==NX_NOT_ENABLED && record.state==ANX_IP_LIVE && record.helper.state==ANX_THREAD_BOUND);
        anx_tx_context_begin(&f,&parent,0);CHECK(nx_packet_release(packet)==NX_SUCCESS);anx_tx_context_end(&f);
        CHECK(anx_exec_ip_delete(&record)==NX_SUCCESS && !errors && disabled==cycle+1 &&
              record.state==ANX_IP_REAPED && clock_record.state==ANX_CLOCK_REAPED && record.helper.state==ANX_THREAD_REAPED &&
              !ip.nx_ip_id && !ip.nx_ip_fast_periodic_timer_created && FindTask(0)->tc_SigAlloc==signals);
        (void)SetTaskPri(FindTask(0),priority);
        anx_tx_context_begin(&f,&parent,0);
        CHECK(!_tx_timer_created_count && !_tx_thread_created_count && !_tx_mutex_created_count && !_tx_event_flags_created_count &&
              pool.nx_packet_pool_available==pool.nx_packet_pool_total);
        ULONG stopped=clock_record.ticks;anx_tx_context_end(&f);
        memset(&ip,0x5a,sizeof(ip));memset(udp,0x5a,sizeof(udp));memset(tcp,0x5a,sizeof(tcp));
        for (unsigned i=0;i<2;i++) memset(stacks[i].bytes,0x5a,sizeof(stacks[i].bytes));
        anx_tx_context_begin(&f,&parent,0);CHECK(tx_thread_sleep(3)==TX_SUCCESS && clock_record.ticks==stopped);anx_tx_context_end(&f);
        for (unsigned i=0;i<2;i++) CHECK(stacks[i].before==0x13572468 && stacks[i].after==0x89abcdef);
        if (!cycle) {
            CASE("unknown-protocol-handler-refused-before-owner-stop");
            CASE("outstanding-packet-ownership-refused-before-owner-stop");
            CASE("known-handlers-retained-and-both-real-IP-timers-retired");
        }
    }
    CASE("two-complete-protocol-IP-clock-cycles-poisoned-storage-reused");
    anx_tx_context_begin(&f,&parent,0);CHECK(nx_packet_pool_delete(&pool)==NX_SUCCESS);anx_tx_context_end(&f);
    CHECK(anx_tx_detach(&pb) && anx_exec_wait_close(&pw));anx_tx_runtime_init(anx_tx_exec_platform());
    CASE("packet-pool-signals-stacks-and-runtime-recovered");
    CHECK(passed==19 && !errors);
    say("research_exec_protocol=PASS 19/19 helpers_reaped=2 clocks_reaped=2 restarts=1\n");
    return 0;
}
