/* Disposable real Exec IO inside retained ThreadX contexts. SPDX-License-Identifier: MIT */
#include "exec_thread.h"
#include "tx_bridge_exec.h"
#include "tx_thread.h"
#include "tx_semaphore.h"
#include <exec/execbase.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
typedef struct {ULONG before,bytes[8192/sizeof(ULONG)],after;} Stack;
static Stack stack;
static AnxExecThread worker;
static TX_THREAD parent,thread;
static AnxTxThread bridge;
static AnxExecWait wait;
static TX_SEMAPHORE progress;
static struct timerequest *parent_timer;
static volatile unsigned ready,errors,advanced,observed_pending;
static unsigned passed,reaped;
static const char *cases[14];
static unsigned printed;
static void say(const char *s)
{
    const char *end=s;while (*end) end++;
    (void)Write(Output(),(APTR)s,(LONG)(end-s));(void)Flush(Output());
}
#define CHECK(x) do {if (!(x)) {say("research_exec_pause=FAIL " #x "\n");return 20;}} while (0)
#define CASE(s) do {cases[passed++]="research_exec_pause=CASE_PASS " s "\n";} while (0)
static void flush_cases(void)
{
    if (SysBase->TDNestCnt>=0 || SysBase->IDNestCnt>=0)
        anx_tx_unsupported("case output inside protected context");
    while (printed<passed) say(cases[printed++]);
}
#define CHILD(x) do {if (!(x)) {errors++;anx_tx_unsupported("Exec pause child contract failed");}} while (0)
static VOID entry(ULONG input)
{
    AnxTxContext nested;
    CHILD(input==42 && tx_thread_identify()==&thread && SysBase->TDNestCnt==0);
    anx_tx_context_begin(&nested,&thread,0);
    CHILD(anx_tx_context_pause() && anx_tx_context_pause() && SysBase->TDNestCnt<0 &&
          !tx_thread_identify() && worker.bridge.exec_wait_nesting==2 && worker.bridge.paused_depth==2);
    ready=1;Signal(wait.owner,1UL<<wait.signal);
    (void)Wait(1UL<<worker.wait.signal);
    /* Give the parent time to block on its real 500 ms timer request. This is a
     * separate owner-owned request, fully reaped by DoIO before re-entry. */
    worker.wait.timer->tr_node.io_Command=TR_ADDREQUEST;
    worker.wait.timer->tr_time.tv_secs=0;worker.wait.timer->tr_time.tv_micro=20000;
    CHILD(DoIO((struct IORequest *)worker.wait.timer)==0 && SysBase->TDNestCnt<0);
    CHILD(anx_tx_context_resume() && SysBase->TDNestCnt<0 && !tx_thread_identify());
    CHILD(anx_tx_context_resume() && SysBase->TDNestCnt==1 && tx_thread_identify()==&thread &&
          !worker.bridge.exec_wait_nesting && !worker.bridge.paused_frame);
    observed_pending=CheckIO((struct IORequest *)parent_timer)==0;
    CHILD(observed_pending && tx_semaphore_put(&progress)==TX_SUCCESS);
    advanced++;anx_tx_context_end(&nested);
    CHILD(SysBase->TDNestCnt==0 && tx_thread_identify()==&thread);
}
int main(void)
{
    AnxTxContext frames[3],marked;
    ULONG initial_signals=FindTask(0)->tc_SigAlloc;
    anx_tx_runtime_init(anx_tx_exec_platform());
    CHECK(anx_exec_wait_open(&wait) && anx_tx_attach(&bridge,&parent,&wait.wait,(uintptr_t)FindTask(0)));
    CHECK(!anx_tx_context_pause() && !anx_tx_context_resume() && SysBase->TDNestCnt<0);
    CASE("outside-and-unmatched-brackets-refused");
    flush_cases();
    parent_timer=(struct timerequest *)CreateIORequest(wait.port,sizeof(*parent_timer));
    CHECK(parent_timer && OpenDevice(TIMERNAME,UNIT_MICROHZ,(struct IORequest *)parent_timer,0)==0);
    stack.before=0x13572468;stack.after=0x89abcdef;
    for (unsigned cycle=0;cycle<2;cycle++) {
        ready=0;advanced=0;observed_pending=0;
        anx_tx_context_begin(&frames[0],&parent,0);
        CHECK(tx_semaphore_create(&progress,(CHAR *)"real progress",0)==TX_SUCCESS);
        anx_tx_context_end(&frames[0]);
        CHECK(anx_exec_thread_prepare(&worker,&thread,(CHAR *)"Exec IO owner",stack.bytes,8192));
        anx_tx_context_begin(&frames[0],&parent,0);
        CHECK(tx_thread_create(&thread,(CHAR *)"Exec IO owner",entry,42,stack.bytes,8192,16,16,0,TX_AUTO_START)==TX_SUCCESS);
        anx_tx_context_end(&frames[0]);
        while (!ready) (void)Wait(1UL<<wait.signal);
        CHECK(worker.state==ANX_THREAD_BOUND && worker.bridge.exec_wait_nesting==2 && !errors);
        if (!cycle) CASE("created-worker-pauses-its-automatic-and-nested-contexts");
        for (unsigned i=0;i<3;i++) anx_tx_context_begin(&frames[i],&parent,0);
        Forbid();CHECK(!anx_tx_context_pause() && SysBase->TDNestCnt==3);Permit();
        if (!cycle) CASE("external-Forbid-refused-without-dropping-levels");
        Disable();CHECK(!anx_tx_context_pause() && SysBase->TDNestCnt==2);Enable();
        if (!cycle) CASE("disabled-interrupts-refused-without-dropping-levels");
        anx_tx_context_begin(&marked,TX_NULL,1);
        CHECK(!anx_tx_context_pause() && SysBase->TDNestCnt==3);anx_tx_context_end(&marked);
        if (!cycle) CASE("marked-callback-refused");
        _tx_thread_preempt_disable=1;CHECK(!anx_tx_context_pause() && SysBase->TDNestCnt==2);_tx_thread_preempt_disable=0;
        if (!cycle) CASE("unrestored-ThreadX-critical-section-refused");
        CHECK(anx_tx_context_pause() && SysBase->TDNestCnt<0 && !_tx_thread_current_ptr &&
              !_tx_thread_system_state && anx_tx_runtime_idle() && bridge.paused_depth==3);
        if (!cycle) CASE("attached-caller-drops-entire-three-level-context-chain");
        CHECK(!anx_tx_detach(&bridge) && anx_tx_context_resume());
        /* resume restored all three levels; pause again before actual IO. */
        CHECK(SysBase->TDNestCnt==2 && anx_tx_context_pause() && SysBase->TDNestCnt<0);
        if (!cycle) CASE("paused-owner-retained-until-balanced-restore");
        flush_cases();
        parent_timer->tr_node.io_Command=TR_ADDREQUEST;
        parent_timer->tr_time.tv_secs=0;parent_timer->tr_time.tv_micro=500000;
        SendIO((struct IORequest *)parent_timer);
        Signal(&worker.task,1UL<<worker.wait.signal);
        CHECK(WaitIO((struct IORequest *)parent_timer)==0 && SysBase->TDNestCnt<0 && !tx_thread_identify());
        CHECK(advanced==1 && observed_pending && !errors);
        if (!cycle) CASE("worker-enters-ThreadX-and-signals-while-parent-real-WaitIO-is-pending");
        parent_timer->tr_time.tv_secs=0;parent_timer->tr_time.tv_micro=1000;
        CHECK(DoIO((struct IORequest *)parent_timer)==0 && SysBase->TDNestCnt<0 && !tx_thread_identify());
        if (!cycle) CASE("attached-owner-real-DoIO-returns-before-reentry");
        CHECK(anx_tx_context_resume() && SysBase->TDNestCnt==2 && tx_thread_identify()==&parent &&
              !bridge.paused_depth && !bridge.paused_frame && !bridge.exec_wait_nesting);
        if (!cycle) CASE("exact-owner-and-three-protection-levels-restored-after-DoIO");
        CHECK(tx_semaphore_get(&progress,TX_NO_WAIT)==TX_SUCCESS && tx_semaphore_get(&progress,TX_NO_WAIT)==TX_NO_INSTANCE);
        for (unsigned i=3;i;i--) anx_tx_context_end(&frames[i-1]);
        flush_cases();
        CHECK(anx_exec_thread_wait(&worker));
        anx_tx_context_begin(&frames[0],&parent,0);
        CHECK(tx_thread_delete(&thread)==TX_SUCCESS && tx_semaphore_delete(&progress)==TX_SUCCESS);
        anx_tx_context_end(&frames[0]);reaped++;
        CHECK(worker.state==ANX_THREAD_REAPED && !worker.wait.opened && !errors &&
              stack.before==0x13572468 && stack.after==0x89abcdef);
        memset(&thread,0x5a,sizeof(thread));memset(stack.bytes,0x5a,sizeof(stack.bytes));
        if (!cycle) CASE("worker-IO-close-native-ACK-public-delete-before-poisoned-reuse");
    }
    CASE("two-real-IO-cycles-reuse-record-and-stack");
    CloseDevice((struct IORequest *)parent_timer);DeleteIORequest((struct IORequest *)parent_timer);
    CHECK(anx_tx_detach(&bridge) && anx_exec_wait_close(&wait) && !_tx_thread_created_count &&
          !_tx_semaphore_created_count && FindTask(0)->tc_SigAlloc==initial_signals);
    anx_tx_runtime_init(anx_tx_exec_platform());
    CASE("all-records-signals-IO-and-runtime-recovered");
    flush_cases();
    CHECK(passed==14 && reaped==2);
    say("research_exec_pause=PASS 14/14 workers_reaped=2 restarts=1\n");return 0;
}
