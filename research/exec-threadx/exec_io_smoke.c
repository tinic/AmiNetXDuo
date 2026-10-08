/* Concurrent real UDP waiters and task-level simulated driver IO ownership.
 * No ISR/device/wire, entropy or performance claim. SPDX-License-Identifier: MIT */
#include "exec_ip.h"
#include "tx_bridge_exec.h"
#include "tx_thread.h"
#include "tx_timer.h"
#include "tx_mutex.h"
#include "tx_event_flags.h"
#include "nx_ip.h"
#include "nx_packet.h"
#include "nx_udp.h"
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
static AnxExecIp record;
static AnxExecClock clock_record;
static AnxExecIpIo io;
static NX_IP ip;
static NX_PACKET_POOL pool;
static NX_UDP_SOCKET sockets[2];
static TX_THREAD parent,threads[3];
static AnxTxThread pb;
static AnxExecWait pw;
static AnxExecThread workers[3];
static TX_EVENT_FLAGS_GROUP commands;
static struct {ULONG before,bytes[2048],after;} stacks[5];
static union {ULONG align;UBYTE bytes[32768];} arena;
static NX_PACKET *held[64];
static UBYTE payload[193];
static ULONG generation,previous_generation,operation,rng=1;
static volatile unsigned errors,enabled,disabled,busy_closes,completions,done[2];
static UINT result[2];
static unsigned mode[2],tag[2],passed,reaped;
#define GO 1UL
#define CLOSE 2UL
#define PROBE 4UL
#define READY 0x100UL
#define DONE 0x200UL
int anx_research_rand(void) {rng=rng*1664525UL+1013904223UL;return (int)(rng&0x7fffffffUL);}
void anx_research_srand(unsigned int seed) {rng=(ULONG)seed+1;}
ULONG _nx_amiga_handshake_millis(VOID)
{
    anx_tx_require_context(0);
    if (clock_record.state!=ANX_CLOCK_RUNNING || !clock_record.wait.opened)
        anx_tx_unsupported("IO fixture handshake clock unavailable");
    return (ULONG)(clock_record.wait.ops.clock(clock_record.wait.ops.context)/1000);
}
static void say(const char *s)
{
    const char *e=s;while (*e) e++;
    (void)Write(Output(),(APTR)s,(LONG)(e-s));(void)Flush(Output());
}
#define CHECK(x) do {if (!(x)) {say("research_exec_io=FAIL " #x "\n");return 20;}} while (0)
#define CASE(s) do {passed++;say("research_exec_io=CASE_PASS " s "\n");} while (0)
static VOID driver(NX_IP_DRIVER *d)
{
    d->nx_ip_driver_status=NX_SUCCESS;
    switch (d->nx_ip_driver_command) {
    case NX_LINK_INTERFACE_ATTACH:break;
    case NX_LINK_INITIALIZE:
        d->nx_ip_driver_interface->nx_interface_ip_mtu_size=1500;
        d->nx_ip_driver_interface->nx_interface_address_mapping_needed=NX_FALSE;break;
    case NX_LINK_ENABLE:enabled++;d->nx_ip_driver_interface->nx_interface_link_up=NX_TRUE;break;
    case NX_LINK_PACKET_SEND:
    case NX_LINK_PACKET_BROADCAST:
        /* Unchanged UDP send hands a real IPv4 packet to the driver. It
         * remains owned here until a separately scheduled native task replies. */
        if (anx_exec_ip_io_accept(&io,generation,d->nx_ip_driver_packet,&operation)!=NX_SUCCESS) {
            errors++;d->nx_ip_driver_status=NX_NOT_ENABLED;
            (void)nx_packet_transmit_release(d->nx_ip_driver_packet);
        }
        break;
    case NX_LINK_DISABLE:
        disabled++;
        if (record.state!=ANX_IP_CLOSING || io.open || io.pending || record.io ||
            record.helper.state!=ANX_THREAD_FINISHED || clock_record.state!=ANX_CLOCK_REAPED ||
            _tx_thread_created_count!=1 || record.helper.wait.opened) errors++;
        d->nx_ip_driver_interface->nx_interface_link_up=NX_FALSE;break;
    case NX_LINK_UNINITIALIZE:break;
    default:errors++;d->nx_ip_driver_status=NX_UNHANDLED_COMMAND;break;
    }
}
static VOID driver_owner(ULONG input)
{
    ULONG flags;
    NX_PACKET *rx,*tx;
    if (input || anx_exec_ip_io_open(&io,&record)!=NX_SUCCESS) {errors++;return;}
    generation=io.generation;
    if (previous_generation && anx_exec_ip_io_receive(&io,previous_generation,(NX_PACKET *)1)!=NX_NOT_ENABLED)
        errors++;
    if (tx_event_flags_set(&commands,READY,TX_OR)!=TX_SUCCESS) {errors++;return;}
    for (;;) {
        if (tx_event_flags_get(&commands,GO|CLOSE|PROBE,TX_OR_CLEAR,&flags,TX_WAIT_FOREVER)!=TX_SUCCESS) {
            errors++;return;
        }
        if (flags==PROBE) {
            if (!io.pending || anx_exec_ip_io_close(&io,generation)!=NX_NOT_ENABLED || !io.open) errors++;
            else busy_closes++;
        } else if (flags==GO) {
            tx=io.pending;
            /* Hairpin an unchanged broadcast datagram through the simulated
             * receive side; copy before TX completion frees the original. */
            if (!tx || nx_packet_copy(tx,&rx,&pool,NX_NO_WAIT)!=NX_SUCCESS) {errors++;return;}
            ULONG old_operation=operation,new_operation=0;
            if (anx_exec_ip_io_complete(&io,generation,old_operation,tx)!=NX_SUCCESS || io.pending ||
                anx_exec_ip_io_complete(&io,generation,old_operation,tx)!=NX_PTR_ERROR) {errors++;return;}
            /* Actual pool LIFO reuses the just-freed address under this one
             * boundary. A stale completion must not free the new ownership. */
            NX_PACKET *reused=0;
            if (nx_packet_allocate(&pool,&reused,0,NX_NO_WAIT)!=NX_SUCCESS || reused!=tx ||
                anx_exec_ip_io_accept(&io,generation,reused,&new_operation)!=NX_SUCCESS ||
                new_operation==old_operation ||
                anx_exec_ip_io_complete(&io,generation,old_operation,tx)!=NX_NOT_ENABLED || io.pending!=reused ||
                anx_exec_ip_io_complete(&io,generation,new_operation,reused)!=NX_SUCCESS) {errors++;return;}
            if (anx_exec_ip_io_receive(&io,generation,rx)!=NX_SUCCESS) {errors++;return;}
            completions++;
        } else if (flags==CLOSE) {
            if (anx_exec_ip_io_close(&io,generation)!=NX_SUCCESS ||
                anx_exec_ip_io_close(&io,generation)!=NX_NOT_ENABLED ||
                anx_exec_ip_io_receive(&io,generation,(NX_PACKET *)1)!=NX_NOT_ENABLED ||
                anx_exec_ip_io_complete(&io,generation,operation,(NX_PACKET *)1)!=NX_NOT_ENABLED) errors++;
            if (tx_event_flags_set(&commands,DONE,TX_OR)!=TX_SUCCESS) errors++;
            return;
        } else {errors++;return;}
        if (tx_event_flags_set(&commands,DONE,TX_OR)!=TX_SUCCESS) {errors++;return;}
    }
}
static VOID consumer(ULONG input)
{
    NX_PACKET *packet=0;
    UBYTE data[193];ULONG copied=0;
    if (input>1 || tx_thread_identify()!=&threads[input+1]) {errors++;return;}
    if (mode[input]==2) {
        result[input]=nx_packet_allocate(&pool,&packet,NX_UDP_PACKET,TX_WAIT_FOREVER);
        if (result[input]!=NX_SUCCESS || !packet || nx_packet_release(packet)!=NX_SUCCESS) errors++;
    } else {
        result[input]=nx_udp_socket_receive(&sockets[1],&packet,TX_WAIT_FOREVER);
        if (mode[input]==1) {
            /* Public wait-abort returns TX_WAIT_ABORTED via unchanged ThreadX
             * and UDP cleanup. An error output conveys no packet ownership. */
            if (result[input]!=TX_WAIT_ABORTED) errors++;
        } else if (result[input]!=NX_SUCCESS || !packet || packet->nx_packet_length!=sizeof(data) ||
                   nx_packet_data_retrieve(packet,data,&copied)!=NX_SUCCESS || copied!=sizeof(data)) errors++;
        else {
            for (unsigned j=0;j<sizeof(data);j++) if (data[j]!=(UBYTE)(j^tag[input])) errors++;
            if (nx_packet_release(packet)!=NX_SUCCESS) errors++;
        }
    }
    if (threads[input+1].tx_thread_suspend_cleanup) errors++;
    done[input]++;
}
static int start(unsigned index)
{
    AnxTxContext f;UINT status;
    if (!anx_exec_thread_prepare(&workers[index],&threads[index],(CHAR *)"IO worker",stacks[index+2].bytes,8192)) return 0;
    anx_tx_context_begin(&f,&parent,0);
    status=tx_thread_create(&threads[index],(CHAR *)"IO worker",index?consumer:driver_owner,
                           index?index-1:0,stacks[index+2].bytes,8192,16,16,0,TX_AUTO_START);
    anx_tx_context_end(&f);return status==TX_SUCCESS;
}
static int reap(unsigned index)
{
    AnxTxContext f;UINT status;
    if (!anx_exec_thread_wait(&workers[index]) || workers[index].wait.opened ||
        threads[index].tx_thread_state!=TX_COMPLETED || workers[index].bridge.thread) return 0;
    anx_tx_context_begin(&f,&parent,0);status=tx_thread_delete(&threads[index]);anx_tx_context_end(&f);
    if (status==TX_SUCCESS) reaped++;
    return status==TX_SUCCESS && workers[index].state==ANX_THREAD_REAPED;
}
/* Poll actual wait nodes, not a guessed scheduling delay. */
static int await(unsigned kind,unsigned count)
{
    AnxTxContext f;int ready=0;
    for (unsigned i=0;i<200 && !ready;i++) {
        anx_tx_context_begin(&f,&parent,0);
        if (kind==0) ready=ip.nx_ip_initialize_done && ip.nx_ip_thread.tx_thread_state==TX_EVENT_FLAG &&
            !ip.nx_ip_protection.tx_mutex_owner && !ip.nx_ip_deferred_received_packet_head &&
            pool.nx_packet_pool_available==pool.nx_packet_pool_total;
        if (kind==1) ready=sockets[1].nx_udp_socket_receive_suspended_count==count;
        if (kind==2) ready=pool.nx_packet_pool_suspended_count==count;
        if (kind==3) ready=done[count]!=0;
        if (!ready && tx_thread_sleep(1)!=TX_SUCCESS) errors++;
        anx_tx_context_end(&f);
    }
    return ready && !errors;
}
static int event(ULONG bits)
{
    AnxTxContext f;ULONG flags;UINT status;
    anx_tx_context_begin(&f,&parent,0);
    status=tx_event_flags_get(&commands,bits,TX_AND_CLEAR,&flags,100);
    anx_tx_context_end(&f);return status==TX_SUCCESS && (flags&bits)==bits && !errors;
}
static int command(ULONG bits)
{
    AnxTxContext f;UINT status;
    anx_tx_context_begin(&f,&parent,0);status=tx_event_flags_set(&commands,bits,TX_OR);anx_tx_context_end(&f);
    return status==TX_SUCCESS && event(DONE);
}
static int send(unsigned value)
{
    AnxTxContext f;NX_PACKET *packet;int ok;
    for (unsigned i=0;i<sizeof(payload);i++) payload[i]=(UBYTE)(i^value);
    anx_tx_context_begin(&f,&parent,0);
    ok=nx_packet_allocate(&pool,&packet,NX_UDP_PACKET,NX_NO_WAIT)==NX_SUCCESS &&
       nx_packet_data_append(packet,payload,sizeof(payload),&pool,NX_NO_WAIT)==NX_SUCCESS &&
       nx_udp_socket_send(&sockets[0],packet,IP_ADDRESS(192,0,2,255),9001)==NX_SUCCESS && io.pending==packet;
    anx_tx_context_end(&f);return ok && !errors;
}
int main(void)
{
    AnxTxContext f;ULONG signals;NX_PACKET *packet;
    say("research_exec_io=START\n");anx_tx_runtime_init(anx_tx_exec_platform());
    CHECK(anx_exec_wait_open(&pw) && anx_tx_attach(&pb,&parent,&pw.wait,(uintptr_t)FindTask(0)));
    parent.tx_thread_priority=parent.tx_thread_user_priority=16;
    parent.tx_thread_preempt_threshold=parent.tx_thread_user_preempt_threshold=16;
    signals=FindTask(0)->tc_SigAlloc;
    for (unsigned i=0;i<5;i++) {stacks[i].before=0x13572468;stacks[i].after=0x89abcdef;}
    anx_tx_context_begin(&f,&parent,0);nx_system_initialize();
    CHECK(nx_packet_pool_create(&pool,(CHAR *)"IO pool",1536,arena.bytes,sizeof(arena.bytes))==NX_SUCCESS);
    anx_tx_context_end(&f);
    for (unsigned cycle=0;cycle<2;cycle++) {
        done[0]=done[1]=0;mode[0]=mode[1]=0;tag[0]=131;tag[1]=217;
        CHECK(anx_exec_ip_create(&record,&parent,&ip,(CHAR *)"IO IP",&pool,driver,stacks[0].bytes,8192,2)==NX_SUCCESS && await(0,0));
        CHECK(anx_exec_ip_clock_start(&record,&clock_record,(CHAR *)"IO clock",stacks[1].bytes,8192)==NX_SUCCESS);
        anx_tx_context_begin(&f,&parent,0);
        CHECK(nx_udp_enable(&ip)==NX_SUCCESS && tx_event_flags_create(&commands,(CHAR *)"IO control")==TX_SUCCESS);
        for (unsigned i=0;i<2;i++) CHECK(nx_udp_socket_create(&ip,&sockets[i],(CHAR *)"IO UDP",NX_IP_NORMAL,NX_FRAGMENT_OKAY,64,4)==NX_SUCCESS &&
                                       nx_udp_socket_bind(&sockets[i],9000+i,NX_NO_WAIT)==NX_SUCCESS);
        anx_tx_context_end(&f);
        if (!cycle) CASE("actual-IP-helper-clock-and-UDP-sockets-started");
        CHECK(start(0) && event(READY) && io.open && io.domain==&record && io.owner==&threads[0] && record.io==&io);
        CHECK(generation==cycle+1);
        CHECK(!io.pending && pool.nx_packet_pool_available==pool.nx_packet_pool_total &&
              anx_exec_ip_delete(&record)==NX_NOT_ENABLED && record.state==ANX_IP_LIVE);
        if (!cycle) CASE("native-driver-owner-registered-and-generation-published");
        CHECK(start(1) && await(1,1) && start(2) && await(1,2));
        anx_tx_context_begin(&f,&parent,0);
        CHECK(sockets[1].nx_udp_socket_receive_suspension_list==&threads[1] && threads[1].tx_thread_suspended_next==&threads[2]);
        anx_tx_context_end(&f);
        if (!cycle) CASE("two-real-UDP-consumers-parked-in-FIFO-order");
        CHECK(send(tag[0]));
        anx_tx_context_begin(&f,&parent,0);
        CHECK(!done[0] && !done[1] && io.pending && sockets[1].nx_udp_socket_receive_suspended_count==2);
        CHECK(anx_exec_ip_io_close(&io,generation)==NX_CALLER_ERROR && io.open);
        CHECK(anx_exec_ip_io_receive(&io,generation,(NX_PACKET *)1)==NX_CALLER_ERROR);
        CHECK(anx_exec_ip_io_accept(&io,generation-1,(NX_PACKET *)1,&operation)==NX_NOT_ENABLED);
        CHECK(nx_packet_allocate(&pool,&packet,0,NX_NO_WAIT)==NX_SUCCESS);
        CHECK(anx_exec_ip_io_accept(&io,generation,packet,&operation)==NX_NOT_ENABLED && nx_packet_release(packet)==NX_SUCCESS);
        anx_tx_context_end(&f);
        CHECK(anx_exec_ip_delete(&record)==NX_NOT_ENABLED && record.state==ANX_IP_LIVE && clock_record.state==ANX_CLOCK_RUNNING);
        CHECK(command(PROBE) && busy_closes==cycle+1 && io.pending);
        if (!cycle) CASE("in-flight-TX-foreign-owner-busy-slot-and-IP-close-refused");
        CHECK(command(GO) && await(3,0) && !done[1] && sockets[1].nx_udp_socket_receive_suspended_count==1);
        if (!cycle) CASE("async-TX-completion-address-reuse-token-and-RX-handoff-wake-first-consumer");
        CHECK(send(tag[1]) && command(GO) && await(3,1) && !io.pending);
        if (!cycle) CASE("second-driver-completion-wakes-second-FIFO-consumer-with-exact-payload");
        CHECK(reap(1) && reap(2) && result[0]==NX_SUCCESS && result[1]==NX_SUCCESS && await(0,0));
        if (!cycle) CASE("both-consumers-native-ACK-and-public-delete-before-storage-reuse");
        done[0]=0;mode[0]=1;
        CHECK(start(1) && await(1,1));
        anx_tx_context_begin(&f,&parent,0);
        CHECK(tx_thread_wait_abort(&threads[1])==TX_SUCCESS && !sockets[1].nx_udp_socket_receive_suspended_count &&
              !sockets[1].nx_udp_socket_receive_suspension_list && !threads[1].tx_thread_suspend_cleanup && !done[0]);
        CHECK(tx_thread_wait_abort(&threads[1])==TX_WAIT_ABORT_ERROR);
        anx_tx_context_end(&f);
        if (!cycle) CASE("public-wait-abort-removes-real-UDP-waiter-before-resume");
        CHECK(reap(1) && done[0]==1 && result[0]==TX_WAIT_ABORTED && !errors);
        if (!cycle) CASE("cancelled-consumer-native-ACK-and-private-timer-IO-recovered");
        unsigned count=0;
        anx_tx_context_begin(&f,&parent,0);
        CHECK(pool.nx_packet_pool_total<=64);
        while (count<pool.nx_packet_pool_total) CHECK(nx_packet_allocate(&pool,&held[count++],0,NX_NO_WAIT)==NX_SUCCESS);
        CHECK(!pool.nx_packet_pool_available);
        anx_tx_context_end(&f);
        done[0]=0;mode[0]=2;
        CHECK(start(1) && await(2,1));
        if (!cycle) CASE("real-packet-pool-exhaustion-parks-native-allocation-waiter");
        anx_tx_context_begin(&f,&parent,0);
        CHECK(nx_packet_release(held[--count])==NX_SUCCESS && !pool.nx_packet_pool_suspended_count &&
              !pool.nx_packet_pool_suspension_list && !pool.nx_packet_pool_available && !done[0]);
        anx_tx_context_end(&f);
        CHECK(reap(1) && done[0]==1 && result[0]==NX_SUCCESS);
        anx_tx_context_begin(&f,&parent,0);
        while (count) CHECK(nx_packet_release(held[--count])==NX_SUCCESS);
        CHECK(pool.nx_packet_pool_available==pool.nx_packet_pool_total);
        anx_tx_context_end(&f);
        if (!cycle) CASE("actual-pool-release-hands-ownership-to-waiter-and-all-packets-recover");
        CHECK(command(CLOSE) && !io.open && !record.io && !io.pending);
        if (!cycle) CASE("owner-closes-idle-driver-lease-and-rejects-late-operations");
        CHECK(reap(0) && _tx_thread_created_count==1 && completions==2*(cycle+1));
        if (!cycle) CASE("driver-native-ACK-and-public-delete-before-IP-stop");
        anx_tx_context_begin(&f,&parent,0);
        for (unsigned i=0;i<2;i++) CHECK(nx_udp_socket_unbind(&sockets[i])==NX_SUCCESS && nx_udp_socket_delete(&sockets[i])==NX_SUCCESS);
        CHECK(tx_event_flags_delete(&commands)==TX_SUCCESS);
        anx_tx_context_end(&f);
        CHECK(await(0,0));
        if (!cycle) CASE("sockets-and-driver-control-retired-with-pool-fully-returned");
        BYTE priority=SetTaskPri(FindTask(0),2);
        CHECK(anx_exec_ip_delete(&record)==NX_SUCCESS && !errors && disabled==cycle+1 &&
              record.state==ANX_IP_REAPED && clock_record.state==ANX_CLOCK_REAPED &&
              record.helper.state==ANX_THREAD_REAPED && FindTask(0)->tc_SigAlloc==signals);
        (void)SetTaskPri(FindTask(0),priority);
        if (!cycle) CASE("real-IP-delete-after-all-driver-application-and-clock-owners-retire");
        memset(&ip,0x5a,sizeof(ip));memset(sockets,0x5a,sizeof(sockets));memset(threads,0x5a,sizeof(threads));
        for (unsigned i=0;i<5;i++) memset(stacks[i].bytes,0x5a,sizeof(stacks[i].bytes));
        anx_tx_context_begin(&f,&parent,0);
        CHECK(anx_exec_ip_io_receive(&io,generation,(NX_PACKET *)1)==NX_NOT_ENABLED &&
              anx_exec_ip_io_complete(&io,generation,operation,(NX_PACKET *)1)==NX_NOT_ENABLED &&
              anx_exec_ip_io_accept(&io,generation,(NX_PACKET *)1,&operation)==NX_NOT_ENABLED &&
              anx_exec_ip_event(&record,NX_IP_RECEIVE_EVENT)==NX_NOT_ENABLED);
        CHECK(!_tx_thread_created_count && !_tx_event_flags_created_count && !_tx_mutex_created_count && !_tx_timer_created_count);
        CHECK(tx_thread_sleep(3)==TX_SUCCESS);
        anx_tx_context_end(&f);
        for (unsigned i=0;i<5;i++) CHECK(stacks[i].before==0x13572468 && stacks[i].after==0x89abcdef);
        previous_generation=generation;
        if (!cycle) CASE("late-completion-and-RX-refused-after-IP-and-worker-storage-poison");
    }
    CASE("two-complete-concurrent-driver-IP-cycles-with-stale-generation-rejection");
    anx_tx_context_begin(&f,&parent,0);CHECK(nx_packet_pool_delete(&pool)==NX_SUCCESS);anx_tx_context_end(&f);
    CHECK(anx_tx_detach(&pb) && anx_exec_wait_close(&pw));anx_tx_runtime_init(anx_tx_exec_platform());
    CASE("all-native-workers-signals-stacks-pool-and-runtime-recovered");
    CHECK(passed==18 && reaped==10 && !errors);
    say("research_exec_io=PASS 18/18 workers_reaped=10 helpers_reaped=2 clocks_reaped=2 restarts=1\n");return 0;
}
