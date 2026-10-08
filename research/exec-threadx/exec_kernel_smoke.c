/* Real kernel/production hooks on a disposable boardless guest.
 * SPDX-License-Identifier: MIT */
#include "exec_netstack.h"
#include "tx_bridge_exec.h"
#include "tx_thread.h"
#include "tx_timer.h"
#include <exec/execbase.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
static TX_TIMER probe;
static TX_MUTEX mutex;
static TX_SEMAPHORE semaphore;
static TX_THREAD worker_thread;
static AnxExecThread worker_record;
static ULONG worker_stack[2048];
static struct {ULONG before,bytes[2048],after;} raw_stack;
static struct Task raw_task,*parent;
static BYTE parent_bit;
static ULONG parent_ack;
static unsigned raw_mode;
static volatile unsigned raw_ready,raw_done,worker_ran;
static UINT raw_result;
static unsigned callbacks,marked_refused,query_ok,samples,passed,printed;
static const char *cases[22];
static struct SignalSemaphore lock,collision;
static void say(const char *s) {const char *e=s;while (*e)e++;(void)Write(Output(),(APTR)s,(LONG)(e-s));(void)Flush(Output());}
#define CHECK(x) do {if (!(x)) {say("research_exec_kernel=FAIL " #x "\n");return 20;}} while (0)
#define REQUIRE(x) do {if (!(x)) anx_tx_unsupported("kernel fixture child: " #x);} while (0)
#define CASE(s) do {cases[passed++]="research_exec_kernel=CASE_PASS " s "\n";if (SysBase->TDNestCnt<0 && SysBase->IDNestCnt<0) flush();} while (0)
static void flush(void) {if (SysBase->TDNestCnt>=0 || SysBase->IDNestCnt>=0) anx_tx_unsupported("kernel fixture output under protection");while (printed<passed)say(cases[printed++]);}
static VOID sample(VOID) {samples++;REQUIRE(!_tx_thread_system_state);}
static VOID timer_probe(ULONG input)
{
    UINT active;ULONG left,reload;
    callbacks++;if (input==42 && _tx_thread_system_state && !tx_thread_identify() &&
        tx_timer_info_get(&probe,0,&active,&left,&reload,0)==TX_SUCCESS && active && left==2 && reload==2) query_ok++;
    if (tx_amiga_kernel_stop()==TX_CALLER_ERROR) marked_refused++;
}
static VOID thread_entry(ULONG input) {REQUIRE(input==77 && tx_thread_identify()==&worker_thread);worker_ran++;}
static VOID raw_entry(VOID)
{
    if (raw_mode==2) {
        BYTE kick=AllocSignal(-1);REQUIRE(kick>=0);
        Forbid();raw_task.tc_UserData=(APTR)(ULONG)(1UL<<kick);raw_ready=1;Signal(parent,parent_ack);Permit();
        (void)Wait(1UL<<kick);
        TX_THREAD *t;ULONG gen;REQUIRE(tx_amiga_adopt_thread(&t,&gen,(CHAR *)"hook peer",16,TX_FALSE)==TX_SUCCESS);
        REQUIRE(tx_semaphore_put(&semaphore)==TX_SUCCESS && tx_amiga_orphan_thread(t,gen)==TX_SUCCESS);
        FreeSignal(kick);raw_result=TX_SUCCESS;
    } else raw_result=raw_mode ? tx_amiga_kernel_start() : tx_amiga_kernel_stop();
    Forbid();raw_done=1;Signal(parent,parent_ack);RemTask(0);for (;;) {}
}
static int raw_start(unsigned mode)
{
    memset(&raw_task,0,sizeof(raw_task));raw_ready=raw_done=0;raw_mode=mode;raw_result=TX_NOT_DONE;
    raw_stack.before=0x13572468;raw_stack.after=0x89abcdef;
    raw_task.tc_Node.ln_Type=NT_TASK;raw_task.tc_Node.ln_Name=(CHAR *)"kernel requester";
    raw_task.tc_SPLower=raw_stack.bytes;raw_task.tc_SPUpper=raw_stack.bytes+2048;raw_task.tc_SPReg=raw_task.tc_SPUpper;
    raw_task.tc_MemEntry.lh_Head=(struct Node *)&raw_task.tc_MemEntry.lh_Tail;
    raw_task.tc_MemEntry.lh_TailPred=(struct Node *)&raw_task.tc_MemEntry.lh_Head;
    return AddTask(&raw_task,(APTR)raw_entry,0)!=0;
}
static int raw_join(void)
{
    while (!raw_done) (void)Wait(parent_ack);
    return !tx_amiga_exec_task_alive(&raw_task) && raw_stack.before==0x13572468 && raw_stack.after==0x89abcdef;
}
int main(void)
{
    TX_THREAD *caller;ULONG gen,first_gen,signals;AnxKernelSnapshot snapshot;
    TX_AMIGA_TICK_STATS ticks;
    say("research_exec_kernel=START\n");parent=FindTask(0);signals=parent->tc_SigAlloc;
    CHECK(tx_amiga_kernel_stop()==TX_SUCCESS && !tx_amiga_kernel_running());CASE("stopped-kernel-idempotent-no-owners");
    Forbid();CHECK(tx_amiga_kernel_start()==TX_CALLER_ERROR && tx_amiga_kernel_stop()==TX_CALLER_ERROR);Permit();
    Disable();CHECK(tx_amiga_kernel_start()==TX_CALLER_ERROR);Enable();CASE("external-protection-refuses-kernel-blocking");
    parent_bit=AllocSignal(-1);CHECK(parent_bit>=0);parent_ack=1UL<<parent_bit;
    CHECK(tx_amiga_kernel_start()==TX_SUCCESS && tx_amiga_kernel_running());
    anx_exec_kernel_snapshot(&snapshot);
    CHECK(snapshot.manager!=parent && snapshot.clock!=parent && snapshot.manager!=snapshot.clock &&
        snapshot.manager_stack && snapshot.clock_stack && tx_amiga_exec_task_alive(snapshot.manager) &&
        tx_amiga_exec_task_alive(snapshot.clock));CASE("stable-management-and-clock-owners-created");
    CHECK(tx_amiga_kernel_start()==TX_SUCCESS);AnxKernelSnapshot same;anx_exec_kernel_snapshot(&same);
    CHECK(same.manager_stack==snapshot.manager_stack && same.clock_stack==snapshot.clock_stack);CASE("duplicate-start-retains-existing-owners");
    CHECK(tx_amiga_stack_in_use(snapshot.manager_stack,1) && tx_amiga_stack_in_use(snapshot.clock_stack,8192) &&
        tx_amiga_stack_in_use((UBYTE *)snapshot.clock_stack+8192,1) && !tx_amiga_stack_in_use(&probe,sizeof(probe)));
    CASE("actual-owned-stacks-and-endpoints-visible");
    ami_netstack_baton_release();ami_netstack_baton_acquire();CHECK(!ami_baton_stats.bs_Live);CASE("plain-Exec-hooks-preserve-unadmitted-context");
    InitSemaphore(&lock);ami_netstack_health_set_sblock(&lock);ami_netstack_health_publish();ami_netstack_health_publish();
    AmiHealthMark *mark=(AmiHealthMark *)FindSemaphore((STRPTR)AMI_HEALTH_NAME);
    CHECK(mark && mark->hm_Magic==AMI_HEALTH_MAGIC && mark->hm_Version==AMI_HEALTH_VERSION &&
        mark->hm_SbLock==&lock && mark->hm_Tick==tx_amiga_tick_stats_live() && mark->hm_Baton==&ami_baton_stats);
    CASE("health-publishes-real-counters-and-preserves-master-lock");
    ami_netstack_health_unpublish();InitSemaphore(&collision);collision.ss_Link.ln_Name=(CHAR *)AMI_HEALTH_NAME;
    Forbid();AddSemaphore(&collision);Permit();ami_netstack_health_publish();ami_netstack_health_unpublish();
    CHECK(FindSemaphore((STRPTR)AMI_HEALTH_NAME)==&collision);Forbid();RemSemaphore(&collision);Permit();
    ami_netstack_health_publish();CASE("health-name-collision-never-removes-foreign-anchor");
    CHECK(tx_amiga_adopt_thread(&caller,&gen,(CHAR *)"kernel caller",16,TX_FALSE)==TX_SUCCESS);first_gen=gen;
    CHECK(anx_tx_holder==parent && tx_thread_sleep(2)==TX_SUCCESS && tx_amiga_kernel_stop()==TX_CALLER_ERROR);
    CHECK(tx_amiga_adopt_suspend(caller,gen)==TX_SUCCESS);CASE("kernel-admission-and-real-caller-deadline");
    CHECK(raw_start(0) && raw_join() && raw_result==TX_NOT_DONE && tx_amiga_kernel_running());
    CHECK(tx_amiga_adopt_resume(caller,gen)==TX_SUCCESS && tx_amiga_orphan_thread(caller,gen)==TX_SUCCESS);
    CASE("foreign-stop-refuses-dormant-cached-caller");
    CHECK(raw_start(2));while (!raw_ready) (void)Wait(parent_ack);
    CHECK(tx_amiga_adopt_thread(&caller,&gen,(CHAR *)"hook caller",16,TX_FALSE)==TX_SUCCESS);
    CHECK(tx_mutex_create(&mutex,(CHAR *)"retained hook mutex",TX_NO_INHERIT)==TX_SUCCESS && tx_mutex_get(&mutex,TX_NO_WAIT)==TX_SUCCESS);
    CHECK(tx_semaphore_create(&semaphore,(CHAR *)"peer progress",0)==TX_SUCCESS);
    AnxTxContext nested;anx_tx_context_begin(&nested,caller,0);ami_netstack_baton_set_sampler(sample);
    ami_netstack_baton_release();ami_netstack_baton_release();
    CHECK(SysBase->TDNestCnt<0 && !anx_tx_holder && ami_baton_stats.bs_Live==1);
    Signal(&raw_task,(ULONG)raw_task.tc_UserData);CHECK(raw_join() && raw_result==TX_SUCCESS);
    ami_netstack_baton_acquire();CHECK(SysBase->TDNestCnt<0 && ami_baton_stats.bs_Live==1);
    ami_netstack_baton_acquire();CHECK(SysBase->TDNestCnt==1 && anx_tx_holder==parent && !ami_baton_stats.bs_Live &&
        mutex.tx_mutex_owner==caller && tx_semaphore_get(&semaphore,TX_NO_WAIT)==TX_SUCCESS && samples>=2);
    anx_tx_context_end(&nested);
    CHECK(tx_mutex_put(&mutex)==TX_SUCCESS && tx_mutex_delete(&mutex)==TX_SUCCESS && tx_semaphore_delete(&semaphore)==TX_SUCCESS);
    CHECK(tx_amiga_orphan_thread(caller,gen)==TX_SUCCESS);CASE("production-nested-hooks-restore-mutex-and-independent-peer-progress");
    CHECK(tx_amiga_adopt_thread(&caller,&gen,(CHAR *)"timer caller",16,TX_FALSE)==TX_SUCCESS);
    CHECK(tx_timer_create(&probe,(CHAR *)"query probe",timer_probe,42,2,2,TX_AUTO_ACTIVATE)==TX_SUCCESS);
    UINT active;ULONG left,reload;CHAR *name;TX_TIMER *next;
    CHECK(tx_timer_info_get(&probe,&name,&active,&left,&reload,&next)==TX_SUCCESS && active && left==2 && reload==2 && next==&probe);
    CHECK(tx_timer_info_get((TX_TIMER *)1,0,0,0,0,0)==TX_TIMER_ERROR);
    CHECK(tx_thread_sleep(5)==TX_SUCCESS && callbacks && callbacks==marked_refused && callbacks==query_ok);
    CHECK(tx_timer_deactivate(&probe)==TX_SUCCESS && tx_timer_info_get(&probe,0,&active,&left,0,0)==TX_SUCCESS && !active && left);
    CHECK(tx_amiga_orphan_thread(caller,gen)==TX_SUCCESS);CASE("actual-flat-timer-query-and-marked-callback-guards");
    CHECK(raw_start(0) && raw_join() && raw_result==TX_NOT_DONE);CASE("stop-refuses-inactive-registered-timer");
    CHECK(tx_amiga_adopt_thread(&caller,&gen,(CHAR *)"timer deletion",16,TX_FALSE)==TX_SUCCESS &&
        tx_timer_delete(&probe)==TX_SUCCESS && tx_amiga_orphan_thread(caller,gen)==TX_SUCCESS);
    CHECK(anx_exec_thread_prepare(&worker_record,&worker_thread,(CHAR *)"kernel worker",worker_stack,8192));
    CHECK(tx_amiga_stack_in_use(worker_stack,8192) && raw_start(0) && raw_join() && raw_result==TX_NOT_DONE);
    CASE("prepared-worker-stack-reservation-prevents-stop");
    CHECK(tx_amiga_adopt_thread(&caller,&gen,(CHAR *)"worker create",16,TX_FALSE)==TX_SUCCESS);
    CHECK(tx_thread_create(&worker_thread,(CHAR *)"kernel worker",thread_entry,77,worker_stack,8192,16,16,TX_NO_TIME_SLICE,TX_AUTO_START)==TX_SUCCESS);
    CHECK(tx_amiga_orphan_thread(caller,gen)==TX_SUCCESS && anx_exec_thread_wait(&worker_record) && worker_ran==1);
    CHECK(raw_start(0) && raw_join() && raw_result==TX_NOT_DONE);CASE("finished-public-worker-still-pins-kernel-until-delete");
    CHECK(tx_amiga_adopt_thread(&caller,&gen,(CHAR *)"worker delete",16,TX_FALSE)==TX_SUCCESS &&
        tx_thread_delete(&worker_thread)==TX_SUCCESS && tx_amiga_orphan_thread(caller,gen)==TX_SUCCESS);
    CHECK(!tx_amiga_stack_in_use(worker_stack,8192));CASE("actual-public-delete-releases-reservation-and-stack-guard");
    tx_amiga_tick_stats(&ticks);CHECK(ticks.tx_amiga_tick_delivered>0 && ticks.tx_amiga_tick_wakeups>0 &&
        ticks.tx_amiga_tick_eclock_hz && ticks.tx_amiga_tick_unit==UNIT_MICROHZ);
    CASE("tick-statistics-describe-real-retained-clock");
    ami_netstack_health_unpublish();CHECK(!FindSemaphore((STRPTR)AMI_HEALTH_NAME));
    CHECK(raw_start(0) && raw_join() && raw_result==TX_SUCCESS && !tx_amiga_kernel_running());ami_netstack_baton_reset();
    anx_exec_kernel_snapshot(&snapshot);CHECK(!snapshot.manager_stack && !snapshot.clock_stack &&
        !tx_amiga_exec_task_alive(snapshot.manager) && !tx_amiga_exec_task_alive(snapshot.clock) && anx_tx_runtime_resettable());
    CASE("different-requester-closes-owning-manager-clock-and-stacks");
    CHECK(raw_start(1) && raw_join() && raw_result==TX_SUCCESS && tx_amiga_kernel_running());
    CHECK(tx_amiga_adopt_thread(&caller,&gen,(CHAR *)"restart caller",16,TX_FALSE)==TX_SUCCESS && gen>first_gen);
    CHECK(tx_amiga_adopt_suspend(caller,gen)==TX_SUCCESS);CASE("requester-exit-preserves-stable-manager-and-generation");
    anx_exec_kernel_snapshot(&snapshot);Forbid();RemTask(snapshot.manager);Permit();
    CHECK(!tx_amiga_kernel_running() && tx_amiga_kernel_stop()==TX_NOT_DONE && tx_amiga_exec_task_alive(snapshot.clock));
    CASE("removed-manager-recovery-refuses-live-cache-without-forcing");
    CHECK(tx_amiga_adopt_resume(caller,gen)==TX_SUCCESS && tx_amiga_orphan_thread(caller,gen)==TX_SUCCESS);
    CHECK(tx_amiga_kernel_stop()==TX_SUCCESS && !anx_tx_admission_check && anx_tx_runtime_resettable());
    CHECK(!tx_amiga_exec_task_alive(snapshot.clock));CASE("empty-domain-recovers-dead-manager-and-clock-owner-pins");
    CHECK(tx_amiga_kernel_start()==TX_SUCCESS && tx_amiga_kernel_stop()==TX_SUCCESS && anx_tx_runtime_resettable());
    ami_netstack_baton_reset();ami_netstack_health_set_sblock(0);FreeSignal(parent_bit);
    CHECK(parent->tc_SigAlloc==signals && !tx_amiga_zombie_tasks() && !tx_amiga_zombie_tasks_live());
    CASE("restart-after-recovery-recovers-all-native-signals-and-resources");flush();
    CHECK(passed==22);say("research_exec_kernel=PASS 22/22 cycles=3 manager_removed=1\n");return 0;
}
