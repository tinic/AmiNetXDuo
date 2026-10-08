/* Actual manager-owned creation, reservation and deferred storage retirement.
 * Pinned IP constructor/helper/delete, controlled boardless driver and fixture
 * PRNG only; no full-library socket/entropy/ISR or performance claim.
 * SPDX-License-Identifier: MIT */
#include "exec_kernel.h"
#include "tx_bridge_exec.h"
#include "tx_thread.h"
#include "tx_timer.h"
#include "nx_ip.h"
#include <exec/execbase.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
static TX_THREAD control,before,*caller;
static ULONG generation;
static TX_EVENT_FLAGS_GROUP events;
static struct {ULONG lo,bytes[1024],hi;} small;
static struct {ULONG lo,bytes[2048],hi;} large;
static struct Task *parent;
static volatile unsigned entries,errors;
static UINT expected_priority;
static APTR expected_stack;
static ULONG expected_size;
static unsigned passed,printed;
static const char *cases[40];
static NX_IP ip;
static NX_PACKET_POOL pool;
static union {ULONG align;UBYTE bytes[65536];} arena;
static unsigned driver_started,driver_stopped;
static ULONG rng=1;
int anx_research_rand(void) {rng=rng*1664525UL+1013904223UL;return (int)(rng&0x7fffffffUL);}
void anx_research_srand(unsigned int seed) {rng=(ULONG)seed+1;}
ULONG _nx_amiga_handshake_millis(VOID) {return (ULONG)((uint64_t)tx_time_get()*1000/TX_TIMER_TICKS_PER_SECOND);}
UINT anx_nx_original_ip_create(NX_IP *,CHAR *,ULONG,ULONG,NX_PACKET_POOL *,VOID (*)(NX_IP_DRIVER *),VOID *,ULONG,UINT);
UINT anx_nx_original_ip_delete(NX_IP *);
static void say(const char *s) {const char *e=s;while (*e)e++;(void)Write(Output(),(APTR)s,(LONG)(e-s));(void)Flush(Output());}
#define CHECK(x) do {if (!(x)) {say("research_exec_managed=FAIL " #x "\n");return 20;}} while (0)
#define CASE(s) do {cases[passed++]="research_exec_managed=CASE_PASS " s "\n";} while (0)
static void flush(void) {if (SysBase->TDNestCnt>=0 || SysBase->IDNestCnt>=0) anx_tx_unsupported("managed output under protection");while (printed<passed)say(cases[printed++]);}
static VOID child(ULONG input)
{
    ULONG local=0;AnxManagedSnapshot s;
    if (input!=77 || tx_thread_identify()!=&control || !anx_exec_thread_managed_snapshot(&control,&s) ||
        s.manager==parent || s.client!=parent || s.native_size!=8192 || s.public_stack!=expected_stack ||
        s.public_size!=expected_size || control.tx_thread_stack_start!=expected_stack ||
        control.tx_thread_stack_size!=expected_size || control.tx_thread_priority!=expected_priority ||
        (uintptr_t)&local<(uintptr_t)s.native_stack || (uintptr_t)&local>=(uintptr_t)s.native_stack+s.native_size ||
        control.tx_thread_amiga_task!=FindTask(0) || tx_thread_delete(&control)!=TX_CALLER_ERROR) errors++;
    entries++;
    if (tx_event_flags_set(&events,1,TX_OR)!=TX_SUCCESS ||
        tx_event_flags_get(&events,2,TX_OR_CLEAR,&local,TX_WAIT_FOREVER)!=TX_SUCCESS || local!=2) errors++;
}
static VOID driver(NX_IP_DRIVER *d)
{
    d->nx_ip_driver_status=NX_SUCCESS;
    switch (d->nx_ip_driver_command) {
    case NX_LINK_INTERFACE_ATTACH:break;
    case NX_LINK_INITIALIZE:d->nx_ip_driver_interface->nx_interface_ip_mtu_size=1500;
        d->nx_ip_driver_interface->nx_interface_address_mapping_needed=NX_FALSE;break;
    case NX_LINK_ENABLE:driver_started++;d->nx_ip_driver_interface->nx_interface_link_up=NX_TRUE;break;
    case NX_LINK_DISABLE:driver_stopped++;d->nx_ip_driver_interface->nx_interface_link_up=NX_FALSE;break;
    case NX_LINK_UNINITIALIZE:break;
    default:d->nx_ip_driver_status=NX_UNHANDLED_COMMAND;break;
    }
}
static UINT create(UINT start)
{ return tx_thread_create(&control,(CHAR *)"managed child",child,77,expected_stack,expected_size,expected_priority,expected_priority,0,start); }
int main(void)
{
    ULONG signals,held_signals,count,bytes,actual;BYTE held[32],bit;unsigned held_count=0;
    UINT old;AnxTxContext nested,marked;AnxManagedSnapshot s;AnxKernelSnapshot kernel;
    say("research_exec_managed=START\n");parent=FindTask(0);signals=parent->tc_SigAlloc;
    small.lo=large.lo=0x13572468;small.hi=large.hi=0x89abcdef;
    for (unsigned i=0;i<1024;i++) small.bytes[i]=0x55555555;
    expected_stack=small.bytes;expected_size=4096;expected_priority=16;
    CHECK(tx_amiga_kernel_start()==TX_SUCCESS);anx_exec_kernel_snapshot(&kernel);
    CASE("real-stable-management-and-clock-started");
    CHECK(tx_amiga_adopt_thread(&caller,&generation,(CHAR *)"managed caller",16,TX_FALSE)==TX_SUCCESS);
    held_signals=parent->tc_SigAlloc;memset(&control,0xa5,sizeof(control));before=control;
    CASE("actual-cached-caller-admitted-for-unchanged-public-create");
    anx_tx_context_begin(&marked,TX_NULL,1);CHECK(create(TX_AUTO_START)==TX_CALLER_ERROR);anx_tx_context_end(&marked);
    Forbid();CHECK(create(TX_AUTO_START)==TX_CALLER_ERROR);Permit();
    Disable();CHECK(create(TX_AUTO_START)==TX_CALLER_ERROR);Enable();
    _tx_thread_preempt_disable=1;CHECK(create(TX_AUTO_START)==TX_CALLER_ERROR);_tx_thread_preempt_disable=0;
    CHECK(!memcmp(&before,&control,sizeof(control)));CASE("marked-and-external-protection-refused-before-publication");
    CHECK(tx_thread_preemption_change(caller,8,&old)==TX_SUCCESS && old==16 && create(TX_AUTO_START)==TX_CALLER_ERROR);
    CHECK(tx_thread_preemption_change(caller,16,&old)==TX_SUCCESS && old==8);
    CASE("unreserved-raised-threshold-refused-with-guard-intact");
    while ((bit=AllocSignal(-1))>=0) held[held_count++]=bit;
    CHECK(create(TX_AUTO_START)==TX_NO_MEMORY && !memcmp(&before,&control,sizeof(control)) && tx_amiga_kernel_running());
    while (held_count) FreeSignal(held[--held_count]);
    CHECK(parent->tc_SigAlloc==held_signals);
    CASE("real-command-ACK-exhaustion-rolls-back-with-kernel-UP");
    CHECK(tx_thread_create(&control,(CHAR *)"bad size",child,77,small.bytes,512,16,16,0,TX_AUTO_START)==TX_SIZE_ERROR &&
        tx_thread_create(&control,(CHAR *)"bad priority",child,77,small.bytes,4096,32,32,0,TX_AUTO_START)==TX_PRIORITY_ERROR &&
        tx_thread_create(&control,(CHAR *)"bad threshold",child,77,small.bytes,4096,16,17,0,TX_AUTO_START)==TX_THRESH_ERROR &&
        !memcmp(&before,&control,sizeof(control)));
    CASE("invalid-creation-parameters-refuse-before-control-publication");
    CHECK(anx_exec_thread_reserve(&control,(CHAR *)"managed child",small.bytes,4096)==TX_SUCCESS);
    CHECK(anx_exec_thread_managed_snapshot(&control,&s) && s.state==ANX_THREAD_PREPARED && !s.entered && s.io_opened &&
        s.manager==kernel.manager && s.client==parent && s.public_size==4096 && s.native_size==8192 && s.native_stack!=small.bytes);
    CASE("manager-reservation-opens-worker-owned-IO-without-control-mutation");
    CHECK(!memcmp(&before,&control,sizeof(control)) && tx_amiga_stack_in_use(small.bytes,4096) && tx_amiga_stack_in_use(s.native_stack,8192));
    CASE("separate-public-and-native-stack-ranges-retained-and-visible");
    CHAR *saved_name=parent->tc_Node.ln_Name;parent->tc_Node.ln_Name=(CHAR *)"changed client identity";
    CHECK(create(TX_AUTO_START)==TX_CALLER_ERROR && anx_exec_thread_unreserve(&control)==TX_CALLER_ERROR &&
        !memcmp(&before,&control,sizeof(control)));
    parent->tc_Node.ln_Name=saved_name;CASE("client-stamp-mismatch-refuses-binding-and-cancellation-with-storage-retained");
    CHECK(anx_exec_thread_unreserve(&control)==TX_SUCCESS && !memcmp(&before,&control,sizeof(control)) && !anx_exec_thread_managed_snapshot(&control,&s));
    anx_exec_thread_managed_resources(&count,&bytes);CHECK(!count && !bytes && parent->tc_SigAlloc==held_signals);
    CASE("unused-reservation-cancels-real-IO-Task-ACK-and-owned-storage");
    CHECK(tx_event_flags_create(&events,(CHAR *)"managed events")==TX_SUCCESS);
    for (unsigned round=0;round<4;round++) {
        expected_stack=round==2 ? (APTR)large.bytes : (APTR)small.bytes;expected_size=round==2 ? 8192 : 4096;
        expected_priority=round==3 ? 2 : 16;
        if (round==3) {
            CHECK(anx_exec_thread_reserve(&control,(CHAR *)"managed child",expected_stack,expected_size)==TX_SUCCESS);
            CHECK(tx_thread_preemption_change(caller,2,&old)==TX_SUCCESS && old==16);
            anx_tx_context_begin(&nested,caller,0);CHECK(create(TX_DONT_START)==TX_SUCCESS && SysBase->TDNestCnt==1 && entries==round);
            anx_tx_context_end(&nested);CHECK(tx_thread_preemption_change(caller,16,&old)==TX_SUCCESS && old==2);
            CASE("one-shot-reservation-binds-under-raised-threshold-and-nested-bracket");
            CHECK(tx_thread_sleep(2)==TX_SUCCESS && entries==round && control.tx_thread_state==TX_SUSPENDED);
            CHECK(tx_thread_resume(&control)==TX_SUCCESS);CASE("DONT_START-gate-survives-manager-reservation-and-real-public-resume");
        } else CHECK(create(TX_AUTO_START)==TX_SUCCESS && entries==round);
        CHECK(anx_exec_thread_managed_snapshot(&control,&s) && s.state==ANX_THREAD_BOUND && s.io_opened &&
            (round==2 ? s.native_stack==large.bytes && s.owned_bytes==sizeof(AnxExecThread) :
                        s.native_stack!=small.bytes && s.owned_bytes==sizeof(AnxExecThread)+8200));
        if (!round) CASE("unreserved-public-create-prepares-binds-and-publishes-before-entry");
        if (round==2) CASE("8192-public-stack-reused-without-second-native-allocation");
        CHECK(tx_thread_delete(&control)==TX_DELETE_ERROR);
        CHECK(tx_event_flags_get(&events,1,TX_OR_CLEAR,&actual,50)==TX_SUCCESS && actual==1 && entries==round+1 && !errors);
        if (!round) CASE("actual-entry-on-native-stack-with-public-layout-and-client-authority");
        CHECK(tx_event_flags_set(&events,2,TX_OR)==TX_SUCCESS && tx_thread_sleep(2)==TX_SUCCESS);
        CHECK(anx_exec_thread_managed_snapshot(&control,&s) && s.state==ANX_THREAD_FINISHED && !s.io_opened &&
            !tx_amiga_exec_task_alive(s.task) && control.tx_thread_state==TX_COMPLETED);
        if (!round) CASE("actual-completion-closes-IO-and-removes-native-Task-before-delete");
        _tx_thread_preempt_disable=1;CHECK(tx_thread_delete(&control)==TX_SUCCESS && !_tx_thread_created_count && !control.tx_thread_id);
        _tx_thread_preempt_disable=0;
        anx_exec_thread_managed_resources(&count,&bytes);CHECK(count==1 && bytes==s.owned_bytes);
        memset(&control,0xa5,sizeof(control));CHECK(!anx_exec_thread_managed_snapshot(&control,&s) && !tx_amiga_stack_in_use(expected_stack,1));
        if (!round) CASE("nonblocking-protected-delete-releases-public-storage-before-private-reap");
        CHECK(tx_thread_sleep(1)==TX_SUCCESS);anx_exec_thread_managed_resources(&count,&bytes);CHECK(!count && !bytes);
        if (!round) CASE("manager-drain-reaps-own-ACK-canaries-stack-record-and-runtime-hold");
    }
    CHECK(entries==4 && !errors && large.lo==0x13572468 && large.hi==0x89abcdef);
    for (unsigned i=0;i<1024;i++) CHECK(small.bytes[i]==0x55555555);
    CHECK(small.lo==0x13572468 && small.hi==0x89abcdef && tx_event_flags_delete(&events)==TX_SUCCESS);
    CASE("four-native-lifecycle-cycles-preserve-client-stack-and-all-canaries");
    nx_system_initialize();CHECK(nx_packet_pool_create(&pool,(CHAR *)"managed IP pool",1536,arena.bytes,sizeof(arena.bytes))==NX_SUCCESS);
    CHAR *name=(CHAR *)"managed IP";
    CHECK(anx_exec_thread_reserve(&ip.nx_ip_thread,name,small.bytes,4096)==TX_SUCCESS);
    CHECK(anx_nx_original_ip_create(&ip,name,IP_ADDRESS(192,0,2,1),0xffffff00UL,&pool,driver,small.bytes,4096,2)==NX_SUCCESS);
    CHECK(anx_exec_thread_managed_snapshot(&ip.nx_ip_thread,&s) && s.state==ANX_THREAD_BOUND && !s.entered &&
        caller->tx_thread_preempt_threshold==caller->tx_thread_priority && _tx_thread_created_count==1);
    CASE("actual-pinned-IP-constructor-consumes-reservation-across-raised-threshold");
    CHECK(nx_ip_status_check(&ip,NX_IP_INITIALIZE_DONE,&actual,50)==NX_SUCCESS && driver_started==1);
    CASE("actual-IP-helper-initializes-through-controlled-boardless-driver");
    CHECK(tx_timer_deactivate(&ip.nx_ip_periodic_timer)==TX_SUCCESS && tx_thread_sleep(1)==TX_SUCCESS);
    CHECK(anx_exec_thread_managed_stop_event(&ip.nx_ip_thread,&ip.nx_ip_events) && tx_thread_sleep(1)==TX_SUCCESS);
    CHECK(anx_exec_thread_managed_snapshot(&ip.nx_ip_thread,&s) && s.state==ANX_THREAD_FINISHED && !s.io_opened &&
        !tx_amiga_exec_task_alive(s.task) && ip.nx_ip_thread.tx_thread_state==TX_TERMINATED);
    CASE("quiesced-IP-helper-private-stop-closes-owner-IO-and-truly-removes-Task");
    CHECK(anx_nx_original_ip_delete(&ip)==NX_SUCCESS && driver_stopped==1 && !ip.nx_ip_id && !ip.nx_ip_thread.tx_thread_id &&
        !_tx_thread_created_count && !_tx_thread_preempt_disable);
    CASE("actual-pinned-IP-delete-calls-nonblocking-managed-delete-under-preemption-disable");
    memset(&ip,0xa5,sizeof(ip));CHECK(tx_thread_sleep(1)==TX_SUCCESS);
    anx_exec_thread_managed_resources(&count,&bytes);CHECK(!count && !bytes && nx_packet_pool_delete(&pool)==NX_SUCCESS);
    CASE("IP-control-reclaimed-before-private-drain-with-all-resources-recovered");
    CHECK(tx_amiga_orphan_thread(caller,generation)==TX_SUCCESS);flush();
    CHECK(tx_amiga_kernel_stop()==TX_SUCCESS && anx_tx_runtime_resettable() && parent->tc_SigAlloc==signals &&
        !anx_exec_managed_prepare && !anx_exec_managed_retire && !anx_exec_managed_notify);
    CASE("kernel-close-recovers-signals-records-stacks-and-installed-hooks");flush();
    CHECK(passed==25);say("research_exec_managed=PASS 25/25 worker_cycles=4 actual_ip_cycle=1\n");return 0;
}
