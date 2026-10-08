/* Actual pinned NX_IP helper/create/delete with synchronous boardless driver.
 * No wire/production-driver/automatic-clock claim. SPDX-License-Identifier: MIT */
#define NX_SOURCE_CODE
#include "exec_ip.h"
#include "tx_bridge_exec.h"
#include "tx_thread.h"
#include "tx_timer.h"
#include "tx_mutex.h"
#include "tx_event_flags.h"
#include "nx_ip.h"
#include "nx_packet.h"
#include "nx_system.h"
#include "nx_udp.h"
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
static AnxExecIp record;
static NX_IP ip,snapshot;
static NX_PACKET_POOL pool;
static NX_UDP_SOCKET socket;
static TX_THREAD parent;
static AnxTxThread pb;
static AnxExecWait pw;
static struct {ULONG before,bytes[2048],after;} stack;
static union {ULONG align;UBYTE bytes[4096];} arena;
static unsigned passed,attached,initialized,enabled,deferred,disabled,uninitialized,error,self_refused;
static void say(const char *s)
{
    const char *e=s;while (*e) e++;
    (void)Write(Output(),(APTR)s,(LONG)(e-s));(void)Flush(Output());
}
#define CHECK(x) do {if (!(x)) {say("research_exec_ip=FAIL " #x "\n");return 20;}} while (0)
#define CASE(s) do {passed++;say("research_exec_ip=CASE_PASS " s "\n");} while (0)
static VOID driver(NX_IP_DRIVER *d)
{
    d->nx_ip_driver_status=NX_SUCCESS;
    if (d->nx_ip_driver_ptr!=&ip || !ip.nx_ip_protection.tx_mutex_owner) error=1;
    switch (d->nx_ip_driver_command) {
    case NX_LINK_INTERFACE_ATTACH:attached++;break;
    case NX_LINK_INITIALIZE:
        initialized++;d->nx_ip_driver_interface->nx_interface_ip_mtu_size=1500;
        d->nx_ip_driver_interface->nx_interface_address_mapping_needed=NX_FALSE;break;
    case NX_LINK_ENABLE:enabled++;d->nx_ip_driver_interface->nx_interface_link_up=NX_TRUE;break;
    case NX_LINK_DEFERRED_PROCESSING:
        deferred++;
        if (anx_exec_ip_delete(&record)==NX_CALLER_ERROR && _nx_ip_delete(&ip)==NX_NOT_ENABLED)
            self_refused++;
        else error=1;
        break;
    case NX_LINK_DISABLE:
        disabled++;
        if (record.state!=ANX_IP_CLOSING || record.helper.state!=ANX_THREAD_FINISHED ||
            record.helper.wait.opened || _nx_ip_delete(&ip)!=NX_NOT_ENABLED || anx_exec_ip_event(&record,NX_IP_DRIVER_DEFERRED_EVENT)!=NX_NOT_ENABLED) error=1;
        d->nx_ip_driver_interface->nx_interface_link_up=NX_FALSE;break;
    case NX_LINK_UNINITIALIZE:uninitialized++;break;
    default:error=1;d->nx_ip_driver_status=NX_UNHANDLED_COMMAND;break;
    }
}
static int parked(void)
{
    AnxTxContext f;int done=0;
    for (unsigned i=0;i<100 && !done;i++) {
        anx_tx_context_begin(&f,&parent,0);
        done=record.helper.entered && ip.nx_ip_initialize_done && ip.nx_ip_thread.tx_thread_state==TX_EVENT_FLAG &&
             ip.nx_ip_events.tx_event_flags_group_suspended_count==1 && !ip.nx_ip_protection.tx_mutex_owner;
        if (!done && tx_thread_sleep(1)!=TX_SUCCESS) error=1;
        anx_tx_context_end(&f);
    }
    return done && !error;
}
int main(void)
{
    AnxTxContext f,tick;
    ULONG signals;
    say("research_exec_ip=START\n");
    anx_tx_runtime_init(anx_tx_exec_platform());
    CHECK(anx_exec_wait_open(&pw) && anx_tx_attach(&pb,&parent,&pw.wait,(uintptr_t)FindTask(0)));
    parent.tx_thread_priority=parent.tx_thread_user_priority=16;
    parent.tx_thread_preempt_threshold=parent.tx_thread_user_preempt_threshold=16;
    signals=FindTask(0)->tc_SigAlloc;stack.before=0x13572468;stack.after=0x89abcdef;
    anx_tx_context_begin(&f,&parent,0);_nx_system_initialize();
    CHECK(_nx_packet_pool_create(&pool,(CHAR *)"IP pool",512,arena.bytes,sizeof(arena.bytes))==NX_SUCCESS);
    memset(&ip,0xa5,sizeof(ip));snapshot=ip;
    CHECK(_nx_ip_create(&ip,(CHAR *)"bypass",0,0,&pool,driver,stack.bytes,8192,2)==NX_NOT_ENABLED &&
          !memcmp(&ip,&snapshot,sizeof(ip)) && !_nx_ip_created_count);
    anx_tx_context_end(&f);CASE("raw-constructor-bypass-refused-before-memset");
    CHECK(anx_exec_ip_create(&record,&parent,&ip,(CHAR *)"unsupported",&pool,driver,stack.bytes,8192,17)==NX_NOT_ENABLED &&
          !memcmp(&ip,&snapshot,sizeof(ip)) && !record.helper.creator);
    CASE("unsupported-creator-threshold-refused-before-reservation");
    for (unsigned cycle=0;cycle<3;cycle++) {
        unsigned old_deferred=deferred;
        CHECK(anx_exec_ip_create(&record,&parent,&ip,(CHAR *)"real IP",&pool,driver,stack.bytes,8192,2)==NX_SUCCESS);
        CHECK(parked() && attached==cycle+1 && initialized==cycle+1 && enabled==cycle+1 &&
              ip.nx_ip_thread.tx_thread_entry==_nx_ip_thread_entry && !error);
        anx_tx_context_begin(&f,&parent,0);snapshot=ip;
        CHECK(_nx_ip_delete(&ip)==NX_NOT_ENABLED && !memcmp(&ip,&snapshot,sizeof(ip)) && disabled==cycle);
        CHECK(tx_thread_terminate(&ip.nx_ip_thread)==TX_FEATURE_NOT_ENABLED);
        CHECK(_nx_udp_socket_create(&ip,&socket,(CHAR *)"live socket",NX_IP_NORMAL,NX_FRAGMENT_OKAY,64,4)==NX_SUCCESS);
        snapshot=ip;anx_tx_context_end(&f);
        CHECK(anx_exec_ip_delete(&record)==NX_SOCKETS_BOUND && !memcmp(&ip,&snapshot,sizeof(ip)) &&
              record.state==ANX_IP_LIVE && record.helper.state==ANX_THREAD_BOUND);
        anx_tx_context_begin(&f,&parent,0);
        CHECK(_nx_udp_socket_delete(&socket)==NX_SUCCESS && !ip.nx_ip_udp_created_sockets_count);
        CHECK(tx_mutex_get(&ip.nx_ip_protection,TX_NO_WAIT)==TX_SUCCESS);snapshot=ip;
        anx_tx_context_end(&f);
        CHECK(anx_exec_ip_delete(&record)==NX_NOT_ENABLED && !memcmp(&ip,&snapshot,sizeof(ip)));
        anx_tx_context_begin(&f,&parent,0);CHECK(tx_mutex_put(&ip.nx_ip_protection)==TX_SUCCESS);anx_tx_context_end(&f);
        /* Keep parent running across Permit to exercise a real READY helper
         * refusal outside a boundary. This is a short fixture scheduling seam. */
        BYTE previous=SetTaskPri(FindTask(0),2);
        anx_tx_context_begin(&f,&parent,0);
        CHECK(anx_exec_ip_event(&record,NX_IP_DRIVER_DEFERRED_EVENT)==TX_SUCCESS);
        CHECK(ip.nx_ip_thread.tx_thread_state==TX_READY);snapshot=ip;
        /* API called from a live boundary must refuse before stopping owner. */
        CHECK(anx_exec_ip_delete(&record)==NX_CALLER_ERROR && !memcmp(&ip,&snapshot,sizeof(ip)));
        anx_tx_context_end(&f);
        CHECK(anx_exec_ip_delete(&record)==NX_NOT_ENABLED && !memcmp(&ip,&snapshot,sizeof(ip)) &&
              record.helper.state==ANX_THREAD_BOUND && record.state==ANX_IP_LIVE);
        (void)SetTaskPri(FindTask(0),previous);
        CHECK(parked() && deferred==old_deferred+1 && self_refused==cycle+1 && !error);
        unsigned before_periodic=record.helper.bridge.resumes;
        anx_tx_context_begin(&tick,TX_NULL,1);
        for (unsigned i=0;i<NX_IP_PERIODIC_RATE;i++) anx_tx_timer_tick();
        CHECK(ip.nx_ip_thread.tx_thread_state==TX_READY);
        anx_tx_context_end(&tick);CHECK(parked());
        CHECK(record.helper.bridge.resumes==before_periodic+1);
        CHECK(anx_exec_ip_delete(&record)==NX_SUCCESS && !ip.nx_ip_id && record.state==ANX_IP_REAPED &&
              record.helper.state==ANX_THREAD_REAPED && !record.helper.wait.opened &&
              disabled==cycle+1 && uninitialized==cycle+1 && !error && FindTask(0)->tc_SigAlloc==signals);
        anx_tx_context_begin(&f,&parent,0);
        CHECK(anx_exec_ip_event(&record,NX_IP_DRIVER_DEFERRED_EVENT)==NX_NOT_ENABLED &&
              !_nx_ip_created_count && !_tx_thread_created_count && !_tx_mutex_created_count &&
              !_tx_event_flags_created_count && !_tx_timer_created_count && pool.nx_packet_pool_available==pool.nx_packet_pool_total);
        anx_tx_context_end(&f);
        CHECK(stack.before==0x13572468 && stack.after==0x89abcdef);
        memset(&ip,0x5a,sizeof(ip));memset(stack.bytes,0x5a,sizeof(stack.bytes));
        if (!cycle) {
            CASE("real-constructor-and-helper-synchronous-driver-startup");
            CASE("raw-delete-and-general-terminate-refused-before-mutation");
            CASE("created-socket-delete-refused-before-stop");
            CASE("owned-IP-mutex-delete-refused-without-mutation");
            CASE("ready-helper-delete-refused-and-retry-after-real-event");
            CASE("real-helper-deferred-event-and-self-delete-refusal");
            CASE("real-periodic-timer-event-manually-dispatched");
            CASE("native-helper-ack-before-unchanged-raw-IP-delete");
            CASE("closing-producer-gate-and-created-list-resource-recovery");
        }
    }
    CASE("three-real-IP-create-delete-cycles-poisoned-storage-reused");
    anx_tx_context_begin(&f,&parent,0);CHECK(_nx_packet_pool_delete(&pool)==NX_SUCCESS);anx_tx_context_end(&f);
    CHECK(anx_tx_detach(&pb) && anx_exec_wait_close(&pw));anx_tx_runtime_init(anx_tx_exec_platform());
    CASE("packet-pool-signals-stack-and-runtime-recovered");
    CHECK(passed==13);say("research_exec_ip=PASS 13/13 helpers_reaped=3 restarts=2\n");
    return 0;
}
