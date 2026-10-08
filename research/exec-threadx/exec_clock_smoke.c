/* Automatic clock and real IP retirement on a disposable boardless guest.
 * No wire or performance claim. SPDX-License-Identifier: MIT */
#define NX_SOURCE_CODE
#include "exec_ip.h"
#include "tx_bridge_exec.h"
#include "tx_timer.h"
#include "tx_thread.h"
#include "tx_mutex.h"
#include "tx_event_flags.h"
#include "nx_ip.h"
#include "nx_packet.h"
#include "nx_system.h"
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#define PERIOD (1000000UL/TX_TIMER_TICKS_PER_SECOND)
static AnxExecIp record;
static AnxExecClock clock_record,second_clock;
static NX_IP ip;
static NX_PACKET_POOL pool;
static TX_THREAD parent;
static AnxTxThread pb;
static AnxExecWait pw;
static TX_TIMER probe;
static struct {ULONG before,bytes[2048],after;} stacks[2];
static union {ULONG align;UBYTE bytes[4096];} arena;
static unsigned passed,errors,enabled,disabled,uninitialized,foreign_refused;
static unsigned callbacks,marked_refused;
static ULONG stop_ticks;
static void say(const char *s)
{
    const char *end=s;while (*end) end++;
    (void)Write(Output(),(APTR)s,(LONG)(end-s));(void)Flush(Output());
}
#define CHECK(x) do {if (!(x)) {say("research_exec_clock=FAIL " #x "\n");return 20;}} while (0)
#define CASE(s) do {passed++;say("research_exec_clock=CASE_PASS " s "\n");} while (0)
static VOID timer_probe(ULONG input)
{
    callbacks++;
    if (input!=42 || !_tx_thread_system_state || tx_thread_identify()!=TX_NULL) errors++;
    if (!anx_exec_clock_stop(&clock_record)) marked_refused++;else errors++;
}
static VOID driver(NX_IP_DRIVER *d)
{
    d->nx_ip_driver_status=NX_SUCCESS;
    switch (d->nx_ip_driver_command) {
    case NX_LINK_INTERFACE_ATTACH:break;
    case NX_LINK_INITIALIZE:
        d->nx_ip_driver_interface->nx_interface_ip_mtu_size=1500;
        d->nx_ip_driver_interface->nx_interface_address_mapping_needed=NX_FALSE;break;
    case NX_LINK_ENABLE:
        enabled++;d->nx_ip_driver_interface->nx_interface_link_up=NX_TRUE;break;
    case NX_LINK_DEFERRED_PROCESSING:
        if (!anx_exec_clock_can_stop(&clock_record) && !anx_exec_clock_stop(&clock_record))
            foreign_refused++;
        else errors++;
        break;
    case NX_LINK_DISABLE:
        disabled++;
        if (record.state!=ANX_IP_CLOSING || clock_record.state!=ANX_CLOCK_REAPED ||
            clock_record.wait.opened || clock_record.wait.timer_sends!=clock_record.wait.timer_reaps ||
            record.helper.state!=ANX_THREAD_FINISHED || record.helper.wait.opened ||
            clock_record.ticks!=stop_ticks || anx_exec_ip_event(&record,NX_IP_PERIODIC_EVENT)!=NX_NOT_ENABLED)
            errors++;
        d->nx_ip_driver_interface->nx_interface_link_up=NX_FALSE;break;
    case NX_LINK_UNINITIALIZE:uninitialized++;break;
    default:errors++;d->nx_ip_driver_status=NX_UNHANDLED_COMMAND;break;
    }
}
static int is_parked(void)
{
    return record.helper.entered && ip.nx_ip_initialize_done &&
        ip.nx_ip_thread.tx_thread_state==TX_EVENT_FLAG &&
        ip.nx_ip_events.tx_event_flags_group_suspended_count==1 && !ip.nx_ip_protection.tx_mutex_owner;
}
static int progress(ULONG ticks,unsigned resumes)
{
    AnxTxContext f;int done=0;
    for (unsigned i=0;i<200 && !done;i++) {
        anx_tx_context_begin(&f,&parent,0);
        done=is_parked() && clock_record.ticks>=ticks && record.helper.bridge.resumes>=resumes;
        if (!done && tx_thread_sleep(1)!=TX_SUCCESS) errors++;
        anx_tx_context_end(&f);
    }
    return done && !errors;
}
int main(void)
{
    AnxTxContext f;
    ULONG signals;
    BYTE held[32],bit;unsigned nheld=0;
    say("research_exec_clock=START\n");
    anx_tx_runtime_init(anx_tx_exec_platform());
    CHECK(anx_exec_wait_open(&pw) && anx_tx_attach(&pb,&parent,&pw.wait,(uintptr_t)FindTask(0)));
    parent.tx_thread_priority=parent.tx_thread_user_priority=16;
    parent.tx_thread_preempt_threshold=parent.tx_thread_user_preempt_threshold=16;
    signals=FindTask(0)->tc_SigAlloc;
    for (unsigned i=0;i<2;i++) {stacks[i].before=0x13572468;stacks[i].after=0x89abcdef;}
    CHECK(!anx_exec_clock_start(0,(CHAR *)"null",stacks[1].bytes,8192));
    CHECK(!anx_exec_clock_start(&clock_record,(CHAR *)"unaligned",(UBYTE *)stacks[1].bytes+1,8192));
    CHECK(!anx_exec_clock_start(&clock_record,(CHAR *)"overlap",&clock_record,8192));
    CHECK(clock_record.state==ANX_CLOCK_EMPTY && FindTask(0)->tc_SigAlloc==signals);
    CASE("invalid-clock-storage-refused-before-publication");
    while ((bit=AllocSignal(-1))>=0) held[nheld++]=bit;
    CHECK(!anx_exec_clock_start(&clock_record,(CHAR *)"no ACK",stacks[1].bytes,8192));
    CHECK(!clock_record.creator && clock_record.state==ANX_CLOCK_EMPTY);
    while (nheld) FreeSignal(held[--nheld]);
    CHECK(FindTask(0)->tc_SigAlloc==signals);
    CASE("clock-ACK-allocation-failure-no-leak");
    anx_tx_context_begin(&f,&parent,0);
    CHECK(!anx_exec_clock_start(&clock_record,(CHAR *)"boundary",stacks[1].bytes,8192));
    CHECK(!anx_exec_clock_join(&clock_record));
    _nx_system_initialize();
    CHECK(_nx_packet_pool_create(&pool,(CHAR *)"clock pool",512,arena.bytes,sizeof(arena.bytes))==NX_SUCCESS);
    anx_tx_context_end(&f);
    CASE("clock-blocking-lifecycle-boundary-refused");
    for (unsigned cycle=0;cycle<3;cycle++) {
        CHECK(anx_exec_ip_create(&record,&parent,&ip,(CHAR *)"clock IP",&pool,driver,stacks[0].bytes,8192,2)==NX_SUCCESS);
        CHECK(progress(0,0) && enabled==cycle+1);
        CHECK(anx_exec_ip_clock_start(&record,&clock_record,(CHAR *)"overlap IP",&ip,8192)==NX_PTR_ERROR);
        CHECK(anx_exec_ip_clock_start(&record,(AnxExecClock *)&record,(CHAR *)"overlap record",stacks[1].bytes,8192)==NX_PTR_ERROR);
        CHECK(!record.clock && !clock_record.creator);
        CHECK(anx_exec_ip_clock_start(&record,&clock_record,(CHAR *)"Exec clock",stacks[1].bytes,8192)==NX_SUCCESS);
        CHECK(record.clock==&clock_record && clock_record.started && clock_record.state==ANX_CLOCK_RUNNING &&
              clock_record.task.tc_SPLower==stacks[1].bytes && clock_record.stack_size==8192 &&
              clock_record.wait.owner==&clock_record.task && clock_record.wait.opened);
        CHECK(!anx_exec_clock_join(&clock_record));
        CHECK(!anx_exec_clock_start(&second_clock,(CHAR *)"duplicate domain",stacks[0].bytes,8192) &&
              second_clock.state==ANX_CLOCK_EMPTY && !second_clock.creator);
        CHECK(anx_exec_ip_clock_start(&record,&clock_record,(CHAR *)"duplicate IP",stacks[1].bytes,8192)==NX_NOT_ENABLED);
        anx_tx_context_begin(&f,&parent,0);
        unsigned before=record.helper.bridge.resumes;
        uint64_t phase=clock_record.next-(uint64_t)clock_record.ticks*PERIOD;
        anx_tx_context_end(&f);
        /* Only the automatic real periodic callback can wake this parked IP. */
        CHECK(progress(NX_IP_PERIODIC_RATE,before+1));
        anx_tx_context_begin(&f,&parent,0);
        CHECK(_tx_timer_system_clock>=clock_record.ticks &&
              clock_record.next==phase+(uint64_t)clock_record.ticks*PERIOD);
        CHECK(tx_timer_create(&probe,(CHAR *)"marked probe",timer_probe,42,2,2,TX_AUTO_ACTIVATE)==TX_SUCCESS);
        CHECK(anx_exec_ip_event(&record,NX_IP_DRIVER_DEFERRED_EVENT)==TX_SUCCESS);
        before=record.helper.bridge.resumes;
        ULONG start_ticks=clock_record.ticks,start_catchup=clock_record.catchup_batches;
        anx_tx_context_end(&f);
        CHECK(progress(start_ticks+4,before) && foreign_refused==cycle+1 && callbacks && callbacks==marked_refused);
        /* Deliberate fixture stall: real EClock continues during a 250ms
         * Forbid. No foreign blocking IO is performed inside the boundary. */
        anx_tx_context_begin(&f,&parent,0);
        start_ticks=clock_record.ticks;
        uint64_t now=clock_record.wait.ops.clock(clock_record.wait.ops.context);
        while (clock_record.wait.ops.clock(clock_record.wait.ops.context)-now<250000) {}
        anx_tx_context_end(&f);
        CHECK(progress(start_ticks+12,0));
        anx_tx_context_begin(&f,&parent,0);
        CHECK(clock_record.catchup_batches>start_catchup &&
              clock_record.next==phase+(uint64_t)clock_record.ticks*PERIOD);
        CHECK(tx_timer_deactivate(&probe)==TX_SUCCESS && tx_timer_delete(&probe)==TX_SUCCESS);
        anx_tx_context_end(&f);
        CHECK(progress(clock_record.ticks,0));
        /* A higher creator priority keeps the exact pending-IO observation
         * stable across Permit until delete closes the clock gate. */
        BYTE priority=SetTaskPri(FindTask(0),2);
        anx_tx_context_begin(&f,&parent,0);
        CHECK(is_parked() && clock_record.wait.wait.result==ANX_WAIT_PENDING &&
              clock_record.wait.timer_sends==clock_record.wait.timer_reaps+1 &&
              anx_exec_clock_can_stop(&clock_record));
        stop_ticks=clock_record.ticks;anx_tx_context_end(&f);
        CHECK(anx_exec_ip_delete(&record)==NX_SUCCESS);
        (void)SetTaskPri(FindTask(0),priority);
        CHECK(record.state==ANX_IP_REAPED && clock_record.state==ANX_CLOCK_REAPED &&
              record.helper.state==ANX_THREAD_REAPED && !ip.nx_ip_id && !errors &&
              disabled==cycle+1 && uninitialized==cycle+1 && FindTask(0)->tc_SigAlloc==signals);
        ULONG sends=clock_record.wait.timer_sends;unsigned old_callbacks=callbacks;
        anx_tx_context_begin(&f,&parent,0);
        CHECK(!anx_exec_clock_stop(&clock_record) && !_tx_timer_created_count && !_nx_ip_created_count &&
              !_tx_mutex_created_count && !_tx_event_flags_created_count && !_tx_thread_created_count &&
              pool.nx_packet_pool_available==pool.nx_packet_pool_total);
        ULONG time=_tx_timer_system_clock;anx_tx_context_end(&f);
        CHECK(!anx_exec_clock_join(&clock_record));
        memset(&ip,0x5a,sizeof(ip));memset(stacks[0].bytes,0x5a,sizeof(stacks[0].bytes));
        memset(stacks[1].bytes,0x5a,sizeof(stacks[1].bytes));
        anx_tx_context_begin(&f,&parent,0);CHECK(tx_thread_sleep(3)==TX_SUCCESS);
        CHECK(_tx_timer_system_clock==time && clock_record.ticks==stop_ticks &&
              sends==clock_record.wait.timer_sends && callbacks==old_callbacks);
        anx_tx_context_end(&f);
        for (unsigned i=0;i<2;i++) CHECK(stacks[i].before==0x13572468 && stacks[i].after==0x89abcdef);
        if (!cycle) {
            CASE("IP-clock-storage-disjointness-before-publication");
            CASE("automatic-clock-native-owner-and-stack-metadata");
            CASE("single-domain-clock-and-live-join-refused");
            CASE("unchanged-periodic-callback-wakes-real-IP-helper-automatically");
            CASE("marked-callback-context-and-stop-refusal");
            CASE("foreign-helper-clock-stop-refused");
            CASE("elapsed-clock-backlog-caught-up-without-phase-drift");
            CASE("pending-timer-IO-stop-drained-before-raw-IP-delete");
            CASE("native-clock-and-helper-ACK-before-IP-storage-release");
            CASE("closed-clock-no-late-ticks-or-callbacks-after-storage-poison");
            CASE("double-stop-and-join-refused");
        }
    }
    CASE("three-IP-clock-restarts-signals-and-stacks-recovered");
    anx_tx_context_begin(&f,&parent,0);CHECK(_nx_packet_pool_delete(&pool)==NX_SUCCESS);anx_tx_context_end(&f);
    CHECK(anx_tx_detach(&pb) && anx_exec_wait_close(&pw));anx_tx_runtime_init(anx_tx_exec_platform());
    CASE("packet-pool-and-held-runtime-fully-recovered");
    CHECK(passed==16 && !errors);say("research_exec_clock=PASS 16/16 clocks_reaped=3 helpers_reaped=3 restarts=2\n");
    return 0;
}
