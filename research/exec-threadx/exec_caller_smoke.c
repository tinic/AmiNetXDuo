/* Disposable cached caller admission and retained-clock execution.
 * SPDX-License-Identifier: MIT */
#include "exec_caller.h"
#include "tx_bridge_exec.h"
#include "tx_thread.h"
#include "tx_semaphore.h"
#include "tx_timer.h"
#include <exec/execbase.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
enum {LIVE,RESERVED,REFUSE,CACHE,DEAD,FOREIGN,DEAD_SLEEP};
typedef struct {ULONG before,bytes[8192/sizeof(ULONG)],after;} Stack;
typedef struct {
    struct Task task;
    TX_THREAD *thread;
    ULONG generation,mask;
    BYTE kick;
    unsigned mode;
    volatile unsigned ready,done,release;
} Raw;
static Raw raw[17];
static Stack stacks[17],clock_stack;
static AnxExecClock clock_record;
static struct Task *parent_task;
static ULONG ack,parent_mask,parent_gen;
static TX_THREAD *parent_thread;
static BYTE ack_signal;
static unsigned passed,printed,reaped,removed_tasks;
static const char *cases[25];
static void say(const char *s)
{
    const char *end=s;while (*end) end++;
    (void)Write(Output(),(APTR)s,(LONG)(end-s));(void)Flush(Output());
}
#define CHECK(x) do {if (!(x)) {say("research_exec_caller=FAIL " #x "\n");return 20;}} while (0)
#define CHILD(x) do {if (!(x)) anx_tx_unsupported("native caller fixture child failed: " #x);} while (0)
#define CASE(s) do {cases[passed++]="research_exec_caller=CASE_PASS " s "\n";} while (0)
static void flush_cases(void)
{
    if (SysBase->TDNestCnt>=0 || SysBase->IDNestCnt>=0) anx_tx_unsupported("caller case output inside boundary");
    while (printed<passed) say(cases[printed++]);
}
static VOID finish(Raw *r)
{
    FreeSignal(r->kick);Forbid();r->done=1;Signal(parent_task,ack);RemTask(0);
    for (;;) {}
}
static VOID entry(VOID)
{
    Raw *r=FindTask(0)->tc_UserData;
    r->kick=AllocSignal(-1);CHILD(r->kick>=0);
    if (r->mode==FOREIGN) {
        ULONG before=FindTask(0)->tc_SigAlloc;
        CHILD(tx_amiga_adopt_resume(parent_thread,parent_gen)==TX_CALLER_ERROR &&
              tx_amiga_orphan_thread(parent_thread,parent_gen)==TX_CALLER_ERROR &&
              tx_amiga_discard_thread(parent_thread,parent_gen)==TX_FEATURE_NOT_ENABLED);
        tx_amiga_adopt_signal_free(parent_mask);
        CHILD(FindTask(0)->tc_SigAlloc==before);finish(r);
    }
    UINT result=tx_amiga_adopt_thread(&r->thread,&r->generation,(CHAR *)"native cached caller",16,
        r->mode==RESERVED || r->mode==REFUSE ? TX_TRUE : TX_FALSE);
    if (r->mode==REFUSE) {CHILD(result==TX_NO_MEMORY && !r->thread && !r->generation);finish(r);}
    CHILD(result==TX_SUCCESS && tx_thread_identify()==r->thread && SysBase->TDNestCnt==0 &&
          ((uintptr_t)r->thread<(uintptr_t)r->task.tc_SPLower ||
           (uintptr_t)r->thread>=(uintptr_t)r->task.tc_SPUpper));
    if (r->mode==DEAD_SLEEP) {
        r->ready=1;Signal(parent_task,ack);
        (void)tx_thread_sleep(10);
        anx_tx_unsupported("removed sleeping caller unexpectedly returned");
    }
    if (r->mode==CACHE || r->mode==DEAD) {
        if (r->mode==DEAD) {r->mask=tx_amiga_adopt_signal(r->thread);CHILD(r->mask!=0);}
        CHILD(tx_amiga_adopt_suspend(r->thread,r->generation)==TX_SUCCESS);
    } else CHILD(anx_tx_context_pause());
    Forbid();r->ready=1;Signal(parent_task,ack);Permit();
    while (!r->release) (void)Wait(1UL<<r->kick);
    if (r->mode==CACHE || r->mode==DEAD) CHILD(tx_amiga_adopt_resume(r->thread,r->generation)==TX_SUCCESS);
    else CHILD(anx_tx_context_resume());
    CHILD(tx_amiga_orphan_thread(r->thread,r->generation)==TX_SUCCESS && SysBase->TDNestCnt<0);
    finish(r);
}
static int start(unsigned i,unsigned mode)
{
    Raw *r=&raw[i];
    if (!tx_amiga_exec_task_context() || !anx_tx_runtime_idle()) return 0;
    memset(r,0,sizeof(*r));r->mode=mode;r->kick=-1;
    stacks[i].before=0x13572468;stacks[i].after=0x89abcdef;
    r->task.tc_Node.ln_Type=NT_TASK;r->task.tc_Node.ln_Name=(CHAR *)"caller fixture Task";
    r->task.tc_SPLower=stacks[i].bytes;r->task.tc_SPUpper=stacks[i].bytes+8192/sizeof(ULONG);r->task.tc_SPReg=r->task.tc_SPUpper;
    r->task.tc_UserData=r;
    r->task.tc_MemEntry.lh_Head=(struct Node *)&r->task.tc_MemEntry.lh_Tail;
    r->task.tc_MemEntry.lh_TailPred=(struct Node *)&r->task.tc_MemEntry.lh_Head;
    return AddTask(&r->task,(APTR)entry,0)!=0;
}
static void ready(unsigned i)
{
    while (!raw[i].ready) (void)Wait(ack);
}
static void release(unsigned i)
{
    Forbid();raw[i].release=1;Signal(&raw[i].task,1UL<<raw[i].kick);Permit();
}
static int join(unsigned i)
{
    while (!raw[i].done) (void)Wait(ack);
    if (tx_amiga_exec_task_alive(&raw[i].task) || stacks[i].before!=0x13572468 || stacks[i].after!=0x89abcdef) return 0;
    memset(stacks[i].bytes,0x5a,sizeof(stacks[i].bytes));reaped++;return 1;
}
static int waiting(void)
{
    AnxCallerStats stats;anx_exec_callers_stats(&stats);return stats.waiting!=0;
}
int main(void)
{
    BYTE held[32];unsigned held_count=0;BYTE bit;
    ULONG initial_signals=FindTask(0)->tc_SigAlloc,baseline;
    TX_SEMAPHORE semaphore;AnxTxContext frame;AnxCallerStats stats;uint64_t before,after;
    parent_task=FindTask(0);ack_signal=AllocSignal(-1);CHECK(ack_signal>=0);ack=1UL<<ack_signal;
    anx_tx_runtime_init(anx_tx_exec_platform());
    clock_stack.before=0x13572468;clock_stack.after=0x89abcdef;
    CHECK(anx_exec_clock_start(&clock_record,(CHAR *)"retained caller deadlines",clock_stack.bytes,8192) &&
          anx_exec_callers_start(&clock_record) && !anx_exec_callers_start(&clock_record));
    CASE("one-retained-backend-clock-pins-caller-service");
    baseline=FindTask(0)->tc_SigAlloc;
    while ((bit=AllocSignal(-1))>=0) held[held_count++]=bit;
    CHECK(tx_amiga_adopt_thread(&parent_thread,&parent_gen,(CHAR *)"signal exhaustion",10,TX_FALSE)==TX_NO_MEMORY && !parent_thread && !parent_gen);
    while (held_count) FreeSignal(held[--held_count]);
    CHECK(FindTask(0)->tc_SigAlloc==baseline);
    CASE("no-signal-admission-refused-without-publication-or-leak");
    CHECK(tx_amiga_adopt_thread(&parent_thread,&parent_gen,(CHAR *)"cached parent",10,TX_FALSE)==TX_SUCCESS);
    parent_mask=FindTask(0)->tc_SigAlloc^baseline;
    CHECK(parent_mask && !(parent_mask&(parent_mask-1)) && tx_thread_identify()==parent_thread &&
          parent_thread->tx_thread_stack_start==FindTask(0)->tc_SPLower && SysBase->TDNestCnt==0);
    CASE("existing-Task-admitted-with-one-signal-and-its-original-stack");
    CHECK(anx_exec_clock_now(&clock_record,&before) && tx_thread_sleep(3)==TX_SUCCESS && anx_exec_clock_now(&clock_record,&after) &&
          after-before>=UINT64_C(3000000)/TX_TIMER_TICKS_PER_SECOND && tx_thread_identify()==parent_thread);
    CASE("finite-ThreadX-sleep-woken-by-retained-clock-not-caller-timer-IO");
    CHECK(tx_semaphore_create(&semaphore,(CHAR *)"timeout cleanup",0)==TX_SUCCESS &&
          tx_semaphore_get(&semaphore,2)==TX_NO_INSTANCE && !semaphore.tx_semaphore_suspended_count &&
          !parent_thread->tx_thread_suspend_cleanup && tx_semaphore_delete(&semaphore)==TX_SUCCESS);
    CASE("actual-semaphore-timeout-removes-pinned-upstream-wait-node");
    CHECK(!anx_exec_clock_stop(&clock_record) && !anx_exec_callers_stop());
    CASE("clock-stop-and-registry-close-refuse-live-admissions");
    CHECK(tx_amiga_adopt_suspend(parent_thread,parent_gen)==TX_SUCCESS && SysBase->TDNestCnt<0 && !tx_thread_identify());
    for (unsigned i=0;i<12;i++) CHECK(tx_amiga_adopt_resume(parent_thread,parent_gen)==TX_SUCCESS &&
        tx_amiga_adopt_suspend(parent_thread,parent_gen)==TX_SUCCESS && FindTask(0)->tc_SigAlloc==(baseline|parent_mask));
    CASE("twelve-cached-brackets-retain-control-generation-and-single-signal");
    CHECK(tx_amiga_adopt_resume(parent_thread,parent_gen+1)==TX_THREAD_ERROR &&
          tx_amiga_adopt_resume((TX_THREAD *)1,parent_gen)==TX_THREAD_ERROR &&
          !tx_amiga_adopt_handle_valid((TX_THREAD *)1,parent_gen) && tx_amiga_adopt_handle_valid(parent_thread,parent_gen));
    CASE("forged-and-stale-generation-handles-refused-before-dereference");
    CHECK(start(0,FOREIGN) && join(0));
    CASE("foreign-owner-cannot-resume-or-retire-live-cache-or-free-its-signal");
    flush_cases();
    for (unsigned i=0;i<14;i++) {CHECK(start(i,LIVE));ready(i);}
    CHECK(start(14,RESERVED));ready(14);anx_exec_callers_stats(&stats);
    CHECK(stats.live==16 && stats.leases==16 && !stats.waiting);
    CASE("normal-pool-full-with-reserved-sixteenth-caller-admitted");
    CHECK(start(16,REFUSE) && join(16));
    CASE("reserved-full-pool-refuses-without-parking-or-signal-leak");
    CHECK(start(15,LIVE));
    while (!waiting()) Delay(1);
    CHECK(!raw[15].ready && tx_amiga_exec_task_alive(&raw[15].task));
    CASE("normal-full-pool-adopter-parks-outside-boundary-in-backend-record");
    release(0);CHECK(join(0));ready(15);anx_exec_callers_stats(&stats);
    CHECK(stats.live==16 && !stats.waiting);
    for (unsigned i=1;i<16;i++) release(i);
    for (unsigned i=1;i<16;i++) CHECK(join(i));
    anx_exec_callers_stats(&stats);CHECK(stats.live==1 && stats.leases==1 && !stats.waiting && FindTask(0)->tc_SigAlloc==(baseline|parent_mask));
    CASE("released-slot-wakes-real-adopter-and-all-seventeen-peers-retire");
    CHECK(start(0,DEAD));ready(0);
    TX_THREAD *dead_thread=raw[0].thread;ULONG dead_gen=raw[0].generation;
    Forbid();RemTask(&raw[0].task);Permit();removed_tasks++;
    CHECK(!tx_amiga_exec_task_alive(&raw[0].task));
    memset(&raw[0].task,0x5a,sizeof(raw[0].task));memset(stacks[0].bytes,0x5a,sizeof(stacks[0].bytes));
    tx_amiga_adopt_sweep_unpublished();
    CHECK(!tx_amiga_adopt_handle_valid(dead_thread,dead_gen) && tx_amiga_discard_thread(dead_thread,dead_gen)==TX_THREAD_ERROR);
    anx_exec_callers_stats(&stats);CHECK(stats.live==1 && stats.leases==1 && stats.reclaimed>=1);
    CASE("removed-dormant-caller-reclaimed-after-Task-and-stack-storage-poison");
    CHECK(start(0,DEAD_SLEEP));ready(0);
    dead_thread=raw[0].thread;dead_gen=raw[0].generation;
    Forbid();CHECK(dead_thread->tx_thread_state==TX_SLEEP && !dead_thread->tx_thread_suspend_cleanup);
    RemTask(&raw[0].task);Permit();removed_tasks++;
    CHECK(!tx_amiga_exec_task_alive(&raw[0].task));
    memset(&raw[0].task,0x5a,sizeof(raw[0].task));memset(stacks[0].bytes,0x5a,sizeof(stacks[0].bytes));
    tx_amiga_adopt_sweep_unpublished();anx_exec_callers_stats(&stats);
    CHECK(stats.dead_sleep==1 && stats.live==1 && stats.leases==1 && !tx_amiga_adopt_handle_valid(dead_thread,dead_gen));
    ULONG notifications=stats.notifications;Delay(15);anx_exec_callers_stats(&stats);
    CHECK(stats.notifications==notifications && stats.dead_sleep==1 && clock_record.state==ANX_CLOCK_RUNNING);
    CASE("removed-sleeping-caller-timer-binding-retired-no-late-wake-after-deadline");
    CHECK(clock_record.state==ANX_CLOCK_RUNNING && clock_record.wait.opened && stats.timeouts>=2);
    CASE("deadline-driver-and-timer-replies-stay-owned-by-live-backend-clock");
    CHECK(tx_amiga_adopt_resume(parent_thread,parent_gen)==TX_SUCCESS && tx_amiga_adopt_signal(parent_thread)==parent_mask &&
          tx_amiga_adopt_suspend(parent_thread,parent_gen)==TX_SUCCESS);
    TX_THREAD *stale_thread=parent_thread;ULONG stale_gen=parent_gen;
    for (unsigned i=0;i<15;i++) {CHECK(start(i,CACHE));ready(i);}
    anx_exec_callers_stats(&stats);
    CHECK(stats.live==15 && stats.leases==16 && stats.evicted==1 && (FindTask(0)->tc_SigAlloc&parent_mask));
    CASE("dormant-cache-evicted-with-separate-owner-signal-debt-retained");
    CHECK(raw[14].thread==stale_thread && raw[14].generation>stale_gen &&
          tx_amiga_adopt_resume(stale_thread,stale_gen)==TX_THREAD_ERROR &&
          tx_amiga_discard_thread(stale_thread,stale_gen)==TX_THREAD_ERROR &&
          tx_amiga_adopt_handle_valid(raw[14].thread,raw[14].generation));
    CASE("recycled-control-protected-by-new-generation-from-old-handle");
    tx_amiga_adopt_signal_free(parent_mask);CHECK(FindTask(0)->tc_SigAlloc==baseline);
    unsigned old_bit=0;while (!(parent_mask&(1UL<<old_bit))) old_bit++;
    CHECK(AllocSignal((LONG)old_bit)==(BYTE)old_bit);
    tx_amiga_adopt_signal_free(parent_mask);CHECK(FindTask(0)->tc_SigAlloc&parent_mask);FreeSignal((LONG)old_bit);
    anx_exec_callers_stats(&stats);CHECK(stats.leases==15);
    CASE("evicted-signal-freed-exactly-once-double-free-cannot-hit-reallocated-bit");
    for (unsigned i=0;i<15;i++) release(i);
    for (unsigned i=0;i<15;i++) CHECK(join(i));
    anx_exec_callers_stats(&stats);CHECK(!stats.live && !stats.leases && !stats.waiting && FindTask(0)->tc_SigAlloc==baseline);
    CASE("all-fifteen-cached-peers-resume-close-and-recover-pool");
    CHECK(tx_amiga_adopt_thread(&parent_thread,&parent_gen,(CHAR *)"reused parent",10,TX_FALSE)==TX_SUCCESS &&
          parent_thread==stale_thread && parent_gen>stale_gen && tx_thread_sleep(1)==TX_SUCCESS &&
          tx_amiga_orphan_thread(parent_thread,parent_gen)==TX_SUCCESS && !tx_amiga_adopt_handle_valid(stale_thread,stale_gen));
    CASE("caller-record-and-private-wait-generations-survive-poisoned-peer-reuse");
    CHECK(FindTask(0)->tc_SigAlloc==baseline && anx_exec_callers_stop());
    CASE("registry-service-closes-only-after-controls-leases-and-waiters-drain");
    anx_exec_caller_generation=(ULONG)-1;
    CHECK(anx_exec_callers_start(&clock_record) &&
          tx_amiga_adopt_thread(&parent_thread,&parent_gen,(CHAR *)"exhausted generation",10,TX_FALSE)==TX_NOT_DONE &&
          anx_exec_callers_stop() && FindTask(0)->tc_SigAlloc==baseline);
    CASE("injected-generation-exhaustion-refuses-without-wrap-or-publication");
    anx_tx_context_begin(&frame,TX_NULL,0);CHECK(anx_exec_clock_stop(&clock_record));anx_tx_context_end(&frame);
    CHECK(anx_exec_clock_join(&clock_record) && clock_stack.before==0x13572468 && clock_stack.after==0x89abcdef);
    memset(clock_stack.bytes,0x5a,sizeof(clock_stack.bytes));
    CHECK(anx_exec_clock_start(&clock_record,(CHAR *)"restarted deadlines",clock_stack.bytes,8192) &&
          anx_exec_callers_start(&clock_record) && anx_exec_caller_generation==(ULONG)-1 &&
          tx_amiga_adopt_thread(&parent_thread,&parent_gen,(CHAR *)"still exhausted",10,TX_FALSE)==TX_NOT_DONE && anx_exec_callers_stop());
    anx_tx_context_begin(&frame,TX_NULL,0);CHECK(anx_exec_clock_stop(&clock_record));anx_tx_context_end(&frame);
    CHECK(anx_exec_clock_join(&clock_record));
    CASE("generation-exhaustion-persists-across-clock-and-caller-service-restart");
    (void)SetSignal(0,ack);FreeSignal(ack_signal);
    CHECK(FindTask(0)->tc_SigAlloc==initial_signals && !clock_record.wait.opened && !clock_record.service && !anx_tx_admission_check &&
          !_tx_semaphore_created_count && !_tx_timer_created_count && passed==24 && reaped==33 && removed_tasks==2);
    anx_tx_runtime_init(anx_tx_exec_platform());
    CASE("all-native-peers-signals-clock-IO-and-runtime-recovered");flush_cases();
    say("research_exec_caller=PASS 25/25 raw_reaped=33 removed_dormant=1 removed_sleep=1 clocks_reaped=2\n");return 0;
}
