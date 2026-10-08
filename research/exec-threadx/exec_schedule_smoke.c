/* Public self-suspend/resume/yield on a disposable boardless Exec guest.
 * SPDX-License-Identifier: MIT */
#include "exec_kernel.h"
#include "tx_bridge_exec.h"
#include "tx_thread.h"
#include <exec/execbase.h>
#include <proto/exec.h>
#include <proto/dos.h>
static TX_THREAD worker_thread,*parent_thread;
static AnxExecThread worker;
static struct {ULONG before,bytes[2048],after;} worker_stack;
static TX_MUTEX mutex;
static TX_SEMAPHORE progress;
static struct Task *parent;
static ULONG parent_gen;
static volatile unsigned phase,peer_progress,pin_checks,worker_resumes;
static unsigned passed,printed;
static const char *cases[27];
static void say(const char *s) {const char *e=s;while (*e)e++;(void)Write(Output(),(APTR)s,(LONG)(e-s));(void)Flush(Output());}
#define CHECK(x) do {if (!(x)) {say("research_exec_schedule=FAIL " #x "\n");return 20;}} while (0)
#define REQUIRE(x) do {if (!(x)) anx_tx_unsupported("schedule child: " #x);} while (0)
#define CASE(s) do {cases[passed++]="research_exec_schedule=CASE_PASS " s "\n";} while (0)
static void flush(void) {if (SysBase->TDNestCnt>=0 || SysBase->IDNestCnt>=0) anx_tx_unsupported("schedule output under protection");while (printed<passed)say(cases[printed++]);}
static VOID entry(ULONG value)
{
    REQUIRE(value==77 && tx_thread_identify()==&worker_thread && SysBase->TDNestCnt==0);
    for (unsigned round=0;round<2;round++) {
        REQUIRE(parent_thread->tx_thread_state==TX_SUSPENDED &&
            !parent_thread->tx_thread_timer.tx_timer_internal_remaining_ticks &&
            !parent_thread->tx_thread_timer.tx_timer_internal_list_head);
        REQUIRE(tx_thread_suspend(parent_thread)==TX_SUCCESS && tx_thread_resume(parent_thread)==TX_SUCCESS &&
            tx_thread_resume(parent_thread)==TX_RESUME_ERROR);
        phase=round*2+1;
        REQUIRE(tx_thread_suspend(&worker_thread)==TX_SUCCESS && !worker.bridge.explicit_suspend &&
            tx_thread_identify()==&worker_thread && SysBase->TDNestCnt==0);
        worker_resumes++;
        REQUIRE(worker_thread.tx_thread_state==TX_READY && tx_mutex_get(&mutex,TX_NO_WAIT)==TX_NOT_AVAILABLE);
        REQUIRE(parent_thread->tx_thread_state==TX_READY &&
            tx_thread_suspend(parent_thread)==TX_FEATURE_NOT_ENABLED); /* retained yield bracket */
        REQUIRE(tx_semaphore_put(&progress)==TX_SUCCESS);peer_progress++;phase=round*2+2;
        REQUIRE(tx_thread_suspend(&worker_thread)==TX_SUCCESS && !worker.bridge.explicit_suspend);
        worker_resumes++;
    }
}
int main(void)
{
    ULONG signals;BYTE original_priority;AnxTxContext nested,marked;
    say("research_exec_schedule=START\n");parent=FindTask(0);signals=parent->tc_SigAlloc;original_priority=parent->tc_Node.ln_Pri;
    CHECK(original_priority==0 && tx_amiga_kernel_start()==TX_SUCCESS);CASE("real-kernel-clock-and-registry-started");
    worker_stack.before=0x13572468;worker_stack.after=0x89abcdef;
    CHECK(anx_exec_thread_prepare(&worker,&worker_thread,(CHAR *)"suspend peer",worker_stack.bytes,8192));
    CASE("worker-prepared-with-owner-IO-and-stack-reservation");
    CHECK(tx_amiga_adopt_thread(&parent_thread,&parent_gen,(CHAR *)"schedule caller",16,TX_FALSE)==TX_SUCCESS);
    CASE("actual-cached-caller-admitted-for-public-services");
    CHECK(tx_thread_create(&worker_thread,(CHAR *)"suspend peer",entry,77,worker_stack.bytes,8192,16,16,0,TX_DONT_START)==TX_SUCCESS);
    CASE("public-DONT_START-creation-keeps-worker-parked");
    CHECK(tx_thread_suspend((TX_THREAD *)1)==TX_THREAD_ERROR && tx_thread_resume((TX_THREAD *)1)==TX_THREAD_ERROR);
    CASE("unregistered-pointer-rejected-before-dereference");
    CHECK(tx_thread_suspend(&worker_thread)==TX_FEATURE_NOT_ENABLED && worker_thread.tx_thread_state==TX_SUSPENDED && !worker.entered);
    CASE("foreign-initial-suspension-refused-without-fabricated-state");
    anx_tx_context_begin(&marked,TX_NULL,1);
    CHECK(tx_thread_suspend(parent_thread)==TX_CALLER_ERROR && tx_thread_resume(&worker_thread)==TX_CALLER_ERROR);
    anx_tx_context_end(&marked);CASE("marked-public-schedule-calls-refused");
    Forbid();CHECK(tx_thread_suspend(parent_thread)==TX_CALLER_ERROR && !parent_thread->tx_thread_suspending);Permit();
    CASE("external-Forbid-refused-before-suspension-mutation");
    Disable();CHECK(tx_thread_suspend(parent_thread)==TX_CALLER_ERROR && !parent_thread->tx_thread_suspending);Enable();
    CASE("disabled-interrupts-refused-before-suspension-mutation");
    _tx_thread_preempt_disable=1;CHECK(tx_thread_suspend(parent_thread)==TX_SUSPEND_ERROR);_tx_thread_preempt_disable=0;
    CASE("unbalanced-ThreadX-preemption-refused");
    anx_tx_context_begin(&nested,parent_thread,0);CHECK(tx_thread_suspend(parent_thread)==TX_CALLER_ERROR);
    CASE("nested-self-suspension-refused-with-context-retained");
    tx_thread_relinquish();CHECK(SysBase->TDNestCnt==1 && tx_thread_identify()==parent_thread && parent->tc_Node.ln_Pri==original_priority);
    anx_tx_context_end(&nested);CASE("public-nested-relinquish-restores-exact-owner-and-native-priority");
    CHECK(anx_tx_relinquish(0)==TX_PTR_ERROR);CASE("missing-yield-operation-refused");
    CHECK(tx_thread_resume(&worker_thread)==TX_SUCCESS && tx_thread_resume(&worker_thread)==TX_RESUME_ERROR && !worker.entered);
    CASE("initial-public-resume-preserved-without-starting-under-Forbid");
    CHECK(tx_thread_suspend(&worker_thread)==TX_FEATURE_NOT_ENABLED && worker_thread.tx_thread_state==TX_READY && !worker_thread.tx_thread_delayed_suspend);
    CASE("foreign-READY-suspension-refused-before-mutation");
    for (unsigned round=0;round<2;round++) {
        CHECK(tx_thread_suspend(parent_thread)==TX_SUCCESS && phase==round*2+1 &&
            tx_thread_identify()==parent_thread && parent_thread->tx_thread_state==TX_READY && SysBase->TDNestCnt==0);
        if (!round) CASE("cached-caller-self-suspend-parks-until-real-public-resume");
        CHECK(worker.bridge.explicit_suspend && worker_thread.tx_thread_state==TX_SUSPENDED &&
            !worker_thread.tx_thread_timer.tx_timer_internal_remaining_ticks &&
            !worker_thread.tx_thread_timer.tx_timer_internal_list_head && !anx_tx_quiescent(&worker.bridge));
        if (!round) CASE("worker-explicit-zero-tick-suspension-is-indefinite-and-not-on-timer-list");
        uint32_t old_token=worker.bridge.token;
        CHECK(tx_mutex_create(&mutex,(CHAR *)"yield retained",TX_NO_INHERIT)==TX_SUCCESS && tx_mutex_get(&mutex,0)==TX_SUCCESS &&
            tx_semaphore_create(&progress,(CHAR *)"yield peer progress",0)==TX_SUCCESS);
        if (!round) CASE("real-mutex-and-semaphore-created-before-yield");
        CHECK(tx_thread_resume(&worker_thread)==TX_SUCCESS && worker.bridge.explicit_suspend && !anx_tx_quiescent(&worker.bridge));pin_checks++;
        if (!round) CASE("resumed-worker-retains-returning-call-pin-until-owner-return");
        tx_thread_relinquish();
        CHECK(peer_progress==round+1 && phase==round*2+2 && tx_semaphore_get(&progress,0)==TX_SUCCESS &&
            worker.task.tc_Node.ln_Pri==parent->tc_Node.ln_Pri && parent->tc_Node.ln_Pri==original_priority);
        if (!round) CASE("public-relinquish-allows-actual-equal-priority-peer-progress");
        CHECK(mutex.tx_mutex_owner==parent_thread && tx_thread_identify()==parent_thread && SysBase->TDNestCnt==0);
        if (!round) CASE("public-relinquish-retains-mutex-current-owner-and-protection");
        CHECK(worker.bridge.explicit_suspend && worker_thread.tx_thread_state==TX_SUSPENDED && worker.bridge.token>old_token &&
            worker.bridge.wait->result==ANX_WAIT_PENDING && worker_resumes==round*2+1);
        if (!round) CASE("worker-second-suspension-has-fresh-real-wait-token");
        CHECK(tx_mutex_put(&mutex)==TX_SUCCESS && tx_mutex_delete(&mutex)==TX_SUCCESS && tx_semaphore_delete(&progress)==TX_SUCCESS);
        CHECK(tx_thread_resume(&worker_thread)==TX_SUCCESS);
        if (round) CASE("two-actual-caller-and-four-worker-suspend-resume-cycles");
    }
    CHECK(tx_amiga_orphan_thread(parent_thread,parent_gen)==TX_SUCCESS && anx_exec_thread_wait(&worker) && worker_resumes==4);
    CASE("normal-worker-finish-closes-owner-IO-and-removes-native-Task");flush();
    CHECK(tx_amiga_adopt_thread(&parent_thread,&parent_gen,(CHAR *)"schedule delete",16,TX_FALSE)==TX_SUCCESS &&
        tx_thread_resume(&worker_thread)==TX_RESUME_ERROR && tx_thread_delete(&worker_thread)==TX_SUCCESS);
    CASE("public-delete-after-real-native-finish-releases-worker-reservation");
    CHECK(worker_stack.before==0x13572468 && worker_stack.after==0x89abcdef && pin_checks==2);
    CASE("both-stack-canaries-and-return-lifetime-pins-verified");
    CHECK(tx_amiga_orphan_thread(parent_thread,parent_gen)==TX_SUCCESS && tx_amiga_kernel_stop()==TX_SUCCESS &&
        anx_tx_runtime_resettable() && parent->tc_SigAlloc==signals && parent->tc_Node.ln_Pri==original_priority);
    CASE("kernel-close-recovers-all-native-signals-and-original-priority");flush();
    CHECK(passed==27);say("research_exec_schedule=PASS 27/27 caller_suspends=2 worker_suspends=4 equal_peer_progress=2\n");return 0;
}
