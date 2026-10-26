#!/usr/bin/env python3
"""Exercise the exact native clock, lifetime guards and reader fail-open path."""
from pathlib import Path
import hashlib,json,os,re,shlex,subprocess,sys
root=Path(__file__).resolve().parents[3]
build=Path(sys.argv[1]).resolve();build.mkdir(parents=True,exist_ok=True)
source=(root/'src/sana2/sana2_rx.c').read_text()
port=(root/'port/threadx-amiga/src/tx_initialize_low_level.c').read_text()
def function(text,signature):
    start=text.index(signature);end=text.index('{',start)+1;depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]+'\n'
prelude=r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef void VOID;typedef uint32_t ULONG;typedef int32_t LONG;typedef int8_t BYTE;typedef int BOOL;typedef unsigned UINT;
#define TRUE 1
#define FALSE 0
#define TX_FALSE 0U
#define TX_TRUE 1U
#define TX_AMIGA_HANDOFF_PRIORITY 1
#define V6_ECLOCK 1
#define V6_TIME_US 6000
#define V6_GATED 1
#define NT_MSGPORT 4
#define PA_IGNORE 2
struct Node {struct Node *ln_Succ,*ln_Pred;char *ln_Name;BYTE ln_Type;};
struct List {struct Node *lh_Head,*lh_Tail,*lh_TailPred;};
struct Task {struct {BYTE ln_Pri;}tc_Node;};
struct MsgPort {struct Node mp_Node;BYTE mp_Flags;struct Task *mp_SigTask;struct List mp_MsgList;};
struct Device {int unused;};
struct EClockVal {ULONG ev_hi,ev_lo;};
static struct {UINT tr_live;} _tx_amiga_tick_run;
static struct Device device,*_tx_amiga_timer_base;
static struct Task task,other;
static ULONG clock_now,rate;
static unsigned forbid_depth,inject,pending,reads,unsafe_reads,fail_at,ports;
static VOID ami_v6_tick(VOID);
static VOID h_tick(VOID){forbid_depth++;ami_v6_tick();forbid_depth--;}
static VOID Forbid(VOID){forbid_depth++;}
static VOID Permit(VOID){forbid_depth--;if(!forbid_depth&&pending){pending=0;clock_now++;h_tick();}}
static struct Task *FindTask(VOID *unused){(void)unused;return &task;}
static BYTE SetTaskPri(struct Task *t,LONG pri){BYTE old=t->tc_Node.ln_Pri;t->tc_Node.ln_Pri=(BYTE)pri;return old;}
static VOID AddPort(struct MsgPort *p){(void)p;ports++;}
static VOID RemPort(struct MsgPort *p){(void)p;ports--;}
static VOID (*_tx_amiga_tick_hook)(VOID);
static ULONG ReadEClock(struct EClockVal *out){
 ULONG answer=rate;reads++;if(!forbid_depth)unsafe_reads++;
 out->ev_hi=0;out->ev_lo=clock_now;
 if(fail_at==reads)answer=0;
 if(inject){inject=0;if(forbid_depth)pending=1;else{clock_now++;h_tick();}}
 return answer;
}
'''
config_start=source.index('#if defined(V6_ECLOCK_STRIDE)')
config=source[config_start:source.index('/* ---- #353',config_start)]
start=source.index('static struct\n{',source.index('/* ---- #353'))
end=source.index('#ifdef V6_GATED',start)
data=source[start:end]  # Includes the actual native state, conversion and failure handler.
macro_start=source.index('#if defined(V6_FRAMES)',end)
macro_end=source.index('#ifdef V6_ECLOCK\n/* Readers can start',macro_start)
macros=source[macro_start:macro_end]
live_provider=function(port,'UINT tx_amiga_eclock_live_locked(VOID)')
provider=live_provider+function(port,'ULONG tx_amiga_eclock_read_locked(ULONG *lo)')
signatures=['static BOOL ami_v6_clock_try_init(VOID)',
 'static BOOL ami_v6_has_victim(VOID)','static ULONG ami_v6_clock(VOID)',
 'static VOID ami_v6_charge_start(VOID)','static VOID ami_v6_charge_stop(VOID)',
 'static VOID ami_v6_charge_checkpoint(ULONG frames)',
 'static VOID ami_v6_tick(VOID)','static VOID ami_v6_settle(BOOL overrun)',
 'static VOID ami_v6_bind(struct Task *me)','static VOID ami_v6_unbind(struct Task *me)']
functions={s:function(source,s) for s in signatures}
checkpoint_key='static VOID ami_v6_charge_checkpoint(ULONG frames)'
# The frame arm charges frames; this time-only optimization is not compiled.
functions[checkpoint_key]='#ifndef V6_FRAMES\n'+functions[checkpoint_key]+'#endif\n'
fixture='static BOOL victim(VOID){return TRUE;}\nstatic BOOL (*ami_sana2_budget_victim_hook)(VOID)=victim;\n'
main=r'''
static unsigned checks,failures;
static void check(int ok,const char *why){checks++;if(!ok){failures++;printf("FAIL %s\n",why);}}
static void reset(void){
 memset(&ami_v6,0,sizeof ami_v6);memset(ami_v6_clock_stats,0,sizeof ami_v6_clock_stats);
 memset(&ami_v6_port,0,sizeof ami_v6_port);memset(&ami_v6_clock_port,0,sizeof ami_v6_clock_port);
 ami_v6_refilled=clock_now=0;rate=709379;forbid_depth=inject=pending=reads=unsafe_reads=fail_at=ports=0;
 _tx_amiga_timer_base=&device;_tx_amiga_tick_run.tr_live=1;_tx_amiga_tick_hook=0;
 task.tc_Node.ln_Pri=0;other.tc_Node.ln_Pri=0;
}
static void bind(void){ami_v6_bind(&task);}
int main(void){
 ULONG out=123,hz;
 reset();_tx_amiga_tick_run.tr_live=0;
 Forbid();hz=tx_amiga_eclock_read_locked(&out);Permit();
 check(!hz&&out==123&&!reads,"not-live base refused without touching output");
 _tx_amiga_tick_run.tr_live=1;_tx_amiga_timer_base=0;
 Forbid();hz=tx_amiga_eclock_read_locked(&out);Permit();
 check(!hz&&out==123&&!reads,"missing device refused");
 _tx_amiga_timer_base=&device;
 Forbid();hz=tx_amiga_eclock_read_locked(0);Permit();
 check(!hz&&!reads,"null output refused");
 rate=0;Forbid();hz=tx_amiga_eclock_read_locked(&out);Permit();
 check(!hz&&out==123&&reads==1,"zero rate leaves output unchanged");
 rate=709379;clock_now=0xfffffff0U;
 Forbid();hz=tx_amiga_eclock_read_locked(&out);Permit();
 check(hz==709379&&out==0xfffffff0U&&!unsafe_reads,"valid raw reading protected");
 const ULONG rates[]={0,1,1000,709379,715909,0xffffffffU};
 const ULONG periods[]={1,6,19,1000};
 for(unsigned i=0;i<sizeof rates/sizeof *rates;i++)for(unsigned j=0;j<sizeof periods/sizeof *periods;j++)
  check(ami_v6_ticks(rates[i],periods[j])==(ULONG)(((uint64_t)rates[i]*periods[j]+999)/1000),"threshold equals wide arithmetic oracle");
 check(ami_v6_ticks(709379,6)==4257&&ami_v6_ticks(709379,19)==13479,"PAL thresholds");
 check(ami_v6_ticks(715909,6)==4296&&ami_v6_ticks(715909,19)==13603,"NTSC thresholds");
 reset();_tx_amiga_tick_run.tr_live=0;bind();
 check(ami_v6.task==&task&&ports==2&&_tx_amiga_tick_hook==ami_v6_tick&&task.tc_Node.ln_Pri==1&&!ami_v6_clock_stats[3]&&ami_v6_clock_stats[4]==2&&ami_v6_clock_stats[5]==1,"unavailable bind publishes waiting and preserves progress");
 check(!forbid_depth,"unavailable bind balances exclusion");
 _tx_amiga_tick_run.tr_live=1;clock_now=1234;h_tick();
 check(!ami_v6_clock_stats[4]&&!ami_v6_clock_stats[3]&&ami_v6_clock_stats[5]==1&&ami_v6_refilled==1234&&ports==2,"first live tick activates startup-waiting clock");
 reset();_tx_amiga_tick_run.tr_live=0;bind();_tx_amiga_tick_run.tr_live=1;clock_now=1234;ami_v6_charge_start();
 check(!ami_v6_clock_stats[4]&&ami_v6.charging&&ami_v6.t0==1234&&ami_v6_refilled==1234,"charge start can activate waiting clock without blocking");
 clock_now=1244;ami_v6_charge_stop();check(ami_v6.used==10,"post-startup charge uses valid new origin");
 reset();_tx_amiga_tick_run.tr_live=0;bind();
 for(unsigned i=1;i<AMI_V6_STARTUP_TRIES;i++)ami_v6_charge_start();
 check(ami_v6_clock_stats[5]==AMI_V6_STARTUP_TRIES&&ami_v6_clock_stats[3]==1&&ami_v6_clock_stats[4]==1&&task.tc_Node.ln_Pri==1,"startup retry exhaustion latches visible high-priority fallback");
 reset();rate=0;bind();
 check(ami_v6.task==&task&&ports==2&&!ami_v6_clock_stats[3]&&ami_v6_clock_stats[4]==2&&ami_v6_clock_stats[5]==1&&task.tc_Node.ln_Pri==1,"zero-rate bind waits for first valid reading at high priority");
 reset();clock_now=100000;bind();
 check(ami_v6.task==&task&&ports==2&&_tx_amiga_tick_hook==ami_v6_tick,"bind publishes both protected ports and hook");
 check(ami_v6_clock_stats[0]==709379&&ami_v6_clock_stats[1]==4257&&ami_v6_clock_stats[2]==13479,"bind uses measured raw tick thresholds");
 check(ami_v6_refilled==100000&&ami_v6.level==1&&task.tc_Node.ln_Pri==1,"bind establishes period and high priority");
 ami_v6_bind(&other);check(ports==2&&ami_v6.task==&task&&!forbid_depth,"second reader does not replace bound reader");
 clock_now=100100;ami_v6_charge_start();clock_now=100200;ami_v6_charge_stop();
 check(ami_v6.used==100&&!ami_v6.charging&&!unsafe_reads,"raw elapsed debit protected");
 ami_v6.used=0;clock_now=0xfffffff0U;ami_v6_charge_start();clock_now=0x10;ami_v6_charge_stop();
 check(ami_v6.used==32,"low word rollover is modulo-safe for bounded intervals");
 reset();bind();clock_now=1;ami_v6_charge_start();clock_now=10000;ami_v6_charge_stop();
 check(ami_v6.used==ami_v6_clock_stats[1],"debit saturates at quota without wrapping");
 unsigned spent_reads=reads;
 ami_v6_settle(FALSE);
 for(unsigned i=0;i<20;i++){ami_v6_charge_start();clock_now++;ami_v6_charge_stop();}
 check(reads==spent_reads&&ami_v6.used==ami_v6_clock_stats[1],"spent credit survives handoffs without more device reads");
 ami_v6_charge_start();clock_now=13479;h_tick();
 check(ami_v6.running&&ami_v6.charging&&!ami_v6.used&&ami_v6.t0==13479,"refill restarts busy spent interval");
 clock_now=13486;ami_v6_charge_stop();check(ami_v6.used==7,"new period after saturation is charged");
 reset();bind();ami_v6.used=ami_v6_clock_stats[1];ami_v6_charge_start();ami_v6_charge_stop();spent_reads=reads;clock_now=13479;h_tick();
 check(!ami_v6.running&&!ami_v6.charging&&!ami_v6.used&&reads==spent_reads+1,"parked spent reader does not charge idle time after refill");
 reset();bind();ami_v6.used=ami_v6_clock_stats[1];Forbid();ami_v6_charge_start();clock_now=13478;pending=1;Permit();
 check(ami_v6.charging&&!ami_v6.used&&ami_v6.t0==13479&&!forbid_depth&&!pending,"deferred refill at spent-start return arms the new period");
 reset();bind();ami_v6.used=ami_v6_clock_stats[1];ami_v6.level=0;task.tc_Node.ln_Pri=0;_tx_amiga_tick_run.tr_live=0;spent_reads=reads;ami_v6_charge_start();
 check(reads==spent_reads&&ami_v6_clock_stats[3]==1&&ami_v6_clock_stats[4]==1&&task.tc_Node.ln_Pri==1,"spent credit detects retirement and restores progress without clock read");
 reset();bind();clock_now=1000;ami_v6_charge_start();clock_now=13478;inject=1;ami_v6_charge_stop();
 check(ami_v6_refilled==13479&&ami_v6.used==0&&!ami_v6.charging,"new period not overcharged across native refill");
 check(!forbid_depth&&!pending&&!unsafe_reads,"refill path exclusion balanced");
 reset();bind();clock_now=13478;inject=1;ami_v6_charge_start();
 check(ami_v6_refilled==13479&&ami_v6.t0==13479,"start/refill uses new period origin");
 clock_now=13480;ami_v6_charge_stop();check(ami_v6.used==1,"one new-period raw tick charged");
 reset();bind();ami_v6.level=0;task.tc_Node.ln_Pri=0;ami_v6.used=100;rate=0;ami_v6_charge_start();
 check(ami_v6_clock_stats[3]==1&&ami_v6_clock_stats[4]==1,"start failure counted and latched");
 check(ami_v6.level==1&&task.tc_Node.ln_Pri==1&&!ami_v6.used&&!ami_v6.charging,"start failure restores progress priority");
 unsigned old_reads=reads;task.tc_Node.ln_Pri=0;ami_v6_charge_start();ami_v6_charge_stop();h_tick();ami_v6_settle(FALSE);
 check(reads==old_reads&&ami_v6_clock_stats[3]==1&&task.tc_Node.ln_Pri==1,"latched failure avoids new reads and restores park priority");
 reset();bind();ami_v6_charge_start();ami_v6.level=0;task.tc_Node.ln_Pri=0;rate=0;ami_v6_charge_stop();
 check(ami_v6_clock_stats[4]&&task.tc_Node.ln_Pri==1&&!ami_v6.charging&&!ami_v6.used,"stop failure restores progress");
 reset();bind();ami_v6.level=0;task.tc_Node.ln_Pri=0;_tx_amiga_tick_run.tr_live=0;h_tick();
 check(ami_v6_clock_stats[4]&&task.tc_Node.ln_Pri==1&&!ami_v6.stats[0],"retired clock fails open before refill");
 reset();bind();ami_v6_charge_start();ami_v6.level=0;task.tc_Node.ln_Pri=0;clock_now=13479;fail_at=reads+2;h_tick();
 check(ami_v6_clock_stats[4]&&task.tc_Node.ln_Pri==1&&!ami_v6.charging,"second tick read failure also restores progress");
 reset();bind();task.tc_Node.ln_Pri=5;rate=0;ami_v6_charge_start();
 check(task.tc_Node.ln_Pri==5&&ami_v6_clock_stats[4],"failure preserves externally raised priority");
 ami_v6_unbind(&other);check(ports==2&&ami_v6.task==&task,"other reader cannot unbind owner");
 ami_v6_unbind(&task);check(!ports&&!ami_v6.task&&!_tx_amiga_tick_hook,"unbind removes ports and callback");
 rate=709379;bind();
 check(ports==2&&ami_v6_clock_stats[3]==1&&!ami_v6_clock_stats[4],"successful rebind preserves cumulative failure count");
 ami_v6_unbind(&task);rate=0;bind();
 check(ami_v6_clock_stats[3]==1&&ami_v6_clock_stats[4]==2&&ami_v6_clock_stats[5]==1&&task.tc_Node.ln_Pri==1,"not-ready rebind preserves past failure and records startup wait");
 rate=709379;ami_v6_charge_start();rate=0;ami_v6_charge_stop();
 check(ami_v6_clock_stats[3]==2&&ami_v6_clock_stats[4]==1,"another runtime fault increments cumulative failure count after startup retry");
 ami_v6_unbind(&task);check(!ports&&!_tx_amiga_tick_hook,"failed rebind is fully unpublished on stop");
 old_reads=reads;ami_v6_charge_start();ami_v6_charge_stop();check(reads==old_reads,"unbound owner never reads device");
 check(!forbid_depth&&!pending&&!unsafe_reads,"all native clock reads protected and balanced");
 reset();bind();clock_now=10;ami_v6_charge_start();clock_now=110;old_reads=reads;ami_v6_charge_checkpoint(0);
 check(reads==old_reads+1&&ami_v6.used==100&&ami_v6.t0==110&&ami_v6.running&&ami_v6.charging,"checkpoint charges once and reuses one sample");
 clock_now=150;ami_v6_charge_stop();check(ami_v6.used==140&&!ami_v6.running&&!ami_v6.charging,"stop charges only time since checkpoint");
 reset();bind();ami_v6_charge_start();task.tc_Node.ln_Pri=0;clock_now=10;ami_v6_charge_checkpoint(0);
 check(task.tc_Node.ln_Pri==1,"checkpoint restores the current high level after park priority");
 task.tc_Node.ln_Pri=5;clock_now=20;ami_v6_charge_checkpoint(0);check(task.tc_Node.ln_Pri==5,"checkpoint preserves externally raised priority");
 reset();bind();clock_now=1;ami_v6_charge_start();clock_now=10000;ami_v6_charge_checkpoint(0);old_reads=reads;
 check(ami_v6.used==ami_v6_clock_stats[1]&&ami_v6.running&&!ami_v6.charging,"checkpoint saturates while work remains active");
 for(unsigned i=0;i<20;i++){clock_now++;ami_v6_charge_checkpoint(0);}
 check(reads==old_reads&&ami_v6.used==ami_v6_clock_stats[1],"spent checkpoints retain quota without device reads");
 clock_now=13479;h_tick();clock_now=13488;ami_v6_charge_checkpoint(0);
 check(ami_v6.used==9&&ami_v6.t0==13488&&ami_v6.charging,"refill resumes busy checkpoint accounting");
 reset();bind();clock_now=1000;ami_v6_charge_start();clock_now=13478;inject=1;ami_v6_charge_checkpoint(0);
 check(ami_v6_refilled==13479&&!ami_v6.used&&ami_v6.charging&&ami_v6.t0==13479,"checkpoint publishes before deferred refill");
 clock_now=13480;ami_v6_charge_stop();check(ami_v6.used==1,"checkpoint refill charges only new period");
 reset();bind();ami_v6_charge_start();ami_v6.level=0;task.tc_Node.ln_Pri=0;rate=0;ami_v6_charge_checkpoint(0);
 check(ami_v6_clock_stats[4]==1&&ami_v6_clock_stats[3]==1&&task.tc_Node.ln_Pri==1&&!ami_v6.running,"checkpoint clock loss restores progress");
 reset();bind();ami_v6.used=ami_v6_clock_stats[1];ami_v6.level=0;task.tc_Node.ln_Pri=0;_tx_amiga_tick_run.tr_live=0;ami_v6_charge_checkpoint(0);
 check(ami_v6_clock_stats[4]==1&&task.tc_Node.ln_Pri==1,"spent checkpoint detects retired clock");
 reset();_tx_amiga_tick_run.tr_live=0;bind();ami_v6_charge_checkpoint(0);_tx_amiga_tick_run.tr_live=1;clock_now=10;ami_v6_charge_checkpoint(0);
 check(!ami_v6_clock_stats[4]&&ami_v6.charging&&ami_v6.running&&ami_v6.t0==10,"checkpoint can activate delayed startup without waiting");
 ami_v6_unbind(&task);old_reads=reads;ami_v6_charge_checkpoint(0);check(reads==old_reads&&!forbid_depth&&!unsafe_reads,"unbound checkpoint is inert and all exclusion balanced");
 printf("native-clock checks=%u failures=%u\n",checks,failures);return failures?1:0;
}
'''
if '--idle' in sys.argv:
    loop=source[source.index('    while (!rd->stop)',source.index('    ami_v6_bind(FindTask(NULL));')):]
    actual=function(loop,'        if (live == 0)')
    idle_fixture=r'''
static ULONG sleep_advance;
static unsigned sleep_tick,sleep_calls,active_in_sleep,active_at_refill;
static UINT tx_thread_sleep(ULONG ticks){
 if(ticks!=2)return 1;
 sleep_calls++;active_in_sleep+=(ami_v6.running||ami_v6.charging);
 clock_now+=sleep_advance;
 if(sleep_tick&&_tx_amiga_tick_hook){h_tick();active_at_refill+=(ami_v6.running||ami_v6.charging);}
 return 0;
}
static VOID run_pool_empty(VOID){do {ULONG live=0;
'''
    idle_main=main[:main.index('int main(void)')]+r'''
int main(void){
 reset();bind();clock_now=10;ami_v6_charge_start();clock_now=20;
 sleep_advance=1000;run_pool_empty();
 check(sleep_calls==1&&!active_in_sleep,"pool sleep parks with accounting stopped");
 check(ami_v6.used==10&&ami_v6.running&&ami_v6.charging&&ami_v6.t0==1020,"pool sleep excludes parked interval and restarts at wake");
 clock_now=1030;ami_v6_charge_stop();check(ami_v6.used==20,"pool sleep retains earlier credit across wake");
 reset();bind();clock_now=10;ami_v6_charge_start();clock_now=20;ami_v6_charge_checkpoint(1);
 check(!ami_v6.used&&ami_v6.charging,"pool fixture begins with a pending sparse interval");
 clock_now=30;sleep_advance=100;run_pool_empty();
 check(ami_v6.used==20&&ami_v6.t0==130&&ami_v6.charging,"pool sleep fully debits pending sparse work");
 reset();bind();clock_now=10;ami_v6_charge_start();clock_now=20;sleep_advance=13459;sleep_tick=1;run_pool_empty();
 check(!active_at_refill&&!ami_v6.used&&ami_v6.t0==13479&&ami_v6.charging,"pool refill cannot restart a parked interval");
 clock_now=13484;ami_v6_charge_stop();check(ami_v6.used==5,"pool refill charges only work after wake");
 ami_v6_unbind(&task);unsigned old_reads=reads;run_pool_empty();
 check(reads==old_reads&&!ports&&!_tx_amiga_tick_hook,"unbound pool accounting leaves clock untouched");
 check(!forbid_depth&&!pending&&!unsafe_reads,"pool accounting exclusion remains balanced");
 printf("idle-clock checks=%u failures=%u\n",checks,failures);return failures?1:0;
}
'''
    selected={s:b for s,b in functions.items() if s not in ['static BOOL ami_v6_has_victim(VOID)','static VOID ami_v6_settle(BOOL overrun)']}
    results={}
    for name in ['fixed','stop-missing-negative','start-missing-negative','early-start-negative']:
        block=actual
        if name=='stop-missing-negative':block=block.replace('            ami_v6_charge_stop();\n','')
        if name=='start-missing-negative':block=block.replace('            ami_v6_charge_start();\n','')
        if name=='early-start-negative':block=block.replace('            tx_thread_sleep(2);\n            ami_v6_charge_start();','            ami_v6_charge_start();\n            tx_thread_sleep(2);')
        text=prelude+config+provider+data+macros+''.join(selected.values())+idle_fixture+block+'\n} while(0);}\n'+idle_main
        c=build/(name+'.c');exe=build/name;c.write_text(text)
        cmd=[os.environ.get('CC','clang'),'-std=c11','-Wall','-Wextra','-Werror','-O1','-g','-DV6_ECLOCK_STRIDE=4']+shlex.split(os.environ.get('CFLAGS',''))
        if '--sanitize' in sys.argv:cmd+=['-fsanitize=address,undefined','-fno-sanitize-recover=all']
        elif '--ubsan' in sys.argv:cmd+=['-fsanitize=undefined','-fno-sanitize-recover=all']
        subprocess.run(cmd+[str(c),'-o',str(exe)],check=True)
        run=subprocess.run([str(exe)],capture_output=True,text=True);(build/(name+'.log')).write_text(run.stdout+run.stderr)
        summary=re.search(r'idle-clock checks=(\d+) failures=(\d+)',run.stdout)
        assert summary and int(summary[1])==9 and run.returncode==(0 if name=='fixed' else 1),(name,run.stdout,run.stderr)
        assert (int(summary[2])==0)==(name=='fixed'),(name,run.stdout)
        if name=='stop-missing-negative':assert 'FAIL pool sleep parks with accounting stopped' in run.stdout
        if name=='start-missing-negative':assert 'FAIL pool sleep excludes parked interval and restarts at wake' in run.stdout
        if name=='early-start-negative':assert 'FAIL pool refill cannot restart a parked interval' in run.stdout
        results[name]={'checks':int(summary[1]),'failures':int(summary[2]),'exit':run.returncode,'source_sha256':hashlib.sha256(text.encode()).hexdigest(),'output':run.stdout};print(name,run.stdout)
    (build/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    raise SystemExit(0)

if '--sparse' in sys.argv:
    native_start=source.index('#elif defined(V6_ECLOCK) && defined(V6_TIME_US)\n        {')
    native_end=source.index('\n#else',native_start)
    drain=source[source.index('        {',native_start):native_end]
    sparse_fixture='''
typedef uint16_t UWORD;
#define AMI_SANA2_RX_RUN_MAX 128
static UWORD drain_frames=128;
static UWORD ami_sana2_rx_drain(VOID *rd,UWORD max){(void)rd;(void)max;return drain_frames;}
static VOID run_drain(VOID){VOID *rd=0;
'''+drain+'\n}\n'
    sparse_main=main[:main.index('int main(void)')]+r'''
int main(void){
 reset();bind();check(ami_v6_clock_stats[6]==1&&ami_v6_clock_stats[7]==V6_ECLOCK_STRIDE&&ami_v6_clock_stats[8]==128,"sample configuration and initial attempt published");
 clock_now=10;ami_v6_charge_start();unsigned before=reads;clock_now=20;ami_v6_charge_checkpoint(1);
 check(reads==before&&!ami_v6.used&&ami_v6.t0==10&&ami_v6.charging&&ami_v6_clock_stats[9]==1,"skip retains full active interval");
 for(unsigned i=1;i<V6_ECLOCK_STRIDE;i++){clock_now=20+10*i;ami_v6_charge_checkpoint(1);}
 check(reads==before+1&&ami_v6.used==10*V6_ECLOCK_STRIDE&&ami_v6.t0==clock_now&&!ami_v6.pending_frames&&!ami_v6.checkpoints,"stride sample debits all skipped time once");
 clock_now=1000;ami_v6_charge_stop();check(ami_v6.used==990&&!ami_v6.running&&!ami_v6.charging,"actual stop charges entire pending interval");
 clock_now=2000;ami_v6_charge_start();clock_now=2005;ami_v6_charge_checkpoint(1);clock_now=2010;ami_v6_charge_stop();check(ami_v6.used==1000,"handoff does not replenish aggregate credit");
 reset();bind();ami_v6_charge_start();before=reads;clock_now=10;ami_v6_charge_checkpoint(128);
 check(reads==before+1&&ami_v6.used==10&&ami_v6_clock_stats[10]==1,"full frame threshold forces early sample");
 reset();bind();ami_v6_charge_start();before=reads;clock_now=10;ami_v6_charge_checkpoint(127);clock_now=20;ami_v6_charge_checkpoint(1);
 check(reads==before+1&&ami_v6.used==20&&!ami_v6.pending_frames,"accumulated partial drains force a sample");
 reset();bind();ami_v6_charge_start();before=reads;clock_now=10;ami_v6_charge_checkpoint(0xffffffffU);
 check(reads==before+1&&ami_v6.used==10&&!ami_v6.pending_frames,"large frame input cannot wrap the threshold");
 reset();bind();ami_v6_charge_start();before=reads;ami_v6.level=0;task.tc_Node.ln_Pri=0;_tx_amiga_tick_run.tr_live=0;ami_v6_charge_checkpoint(1);
 check(reads==before&&ami_v6_clock_stats[4]==1&&task.tc_Node.ln_Pri==1&&!ami_v6.running,"skipped checkpoint detects retirement without device read");
 reset();bind();ami_v6_charge_start();clock_now=13478;pending=1;ami_v6_charge_checkpoint(1);
 check(ami_v6_refilled==13479&&!ami_v6.used&&ami_v6.t0==13479&&ami_v6.charging&&!ami_v6.pending_frames&&!ami_v6.checkpoints,"deferred refill resets skipped interval atomically");
 clock_now=13480;ami_v6_charge_stop();check(ami_v6.used==1,"stop after refill charges only new period");
 reset();bind();ami_v6_charge_start();clock_now=10000;ami_v6_charge_checkpoint(128);before=reads;
 check(ami_v6.used==ami_v6_clock_stats[1]&&!ami_v6.charging&&ami_v6.running,"sparse sample saturates but keeps running");
 ami_v6_settle(FALSE);check(ami_v6.level==0&&task.tc_Node.ln_Pri==0,"spent sparse credit demotes at a released baton");
 for(unsigned i=0;i<10;i++)ami_v6_charge_checkpoint(128);
 check(reads==before&&ami_v6.used==ami_v6_clock_stats[1],"spent sparse budget survives checkpoints without reads");
 clock_now=13479;h_tick();clock_now=13484;ami_v6_charge_checkpoint(1);clock_now=13489;ami_v6_charge_stop();
 check(ami_v6.used==10,"refill restarts a saturated busy reader with deferred samples");
 reset();_tx_amiga_tick_run.tr_live=0;bind();ami_v6_charge_checkpoint(1);_tx_amiga_tick_run.tr_live=1;clock_now=10;ami_v6_charge_checkpoint(1);
 check(!ami_v6_clock_stats[4]&&ami_v6.running&&ami_v6.charging&&ami_v6.t0==10,"sparse startup retries without waiting");
 reset();bind();ami_v6_charge_start();ami_v6.level=0;task.tc_Node.ln_Pri=0;rate=0;ami_v6_charge_checkpoint(128);
 check(ami_v6_clock_stats[4]==1&&task.tc_Node.ln_Pri==1,"forced sample clock fault restores progress");
 reset();bind();ami_v6_charge_start();task.tc_Node.ln_Pri=0;ami_v6_charge_checkpoint(1);check(task.tc_Node.ln_Pri==1,"skipped sample restores current priority level");
 task.tc_Node.ln_Pri=5;ami_v6_charge_checkpoint(1);check(task.tc_Node.ln_Pri==5,"skipped sample keeps externally raised priority");
 reset();bind();ami_v6_charge_start();before=reads;clock_now=10;run_drain();
 check(reads==before+1&&ami_v6_clock_stats[10]==1&&ami_v6.used==10,"real drain call forwards its actual frame count");
 reset();bind();clock_now=10;ami_v6_charge_start();ami_v6_charge_checkpoint(127);
 before=reads;drain_frames=159;clock_now=20;run_drain();
 check(reads==before+1&&ami_v6.used==10&&!ami_v6.pending_frames,"overshooting batch drain forces a sample after a pending interval");
 check(ami_v6_clock_stats[10]==(V6_ECLOCK_STRIDE>2?1U:0U)&&ami_v6_clock_stats[9]==1,"batch overshoot distinguishes cadence from early forced sampling");
 unsigned attempts=ami_v6_clock_stats[6];ami_v6_unbind(&task);bind();check(ami_v6_clock_stats[6]==attempts+1,"attempt count survives interface rebind");
 ami_v6_unbind(&task);before=reads;ami_v6_charge_checkpoint(1);check(!ports&&reads==before&&!_tx_amiga_tick_hook,"unbound sparse path is inert");
 check(!forbid_depth&&!pending&&!unsafe_reads,"all sparse sampling exclusion is balanced");
 printf("sparse-clock checks=%u failures=%u\n",checks,failures);return failures?1:0;
}
'''
    sparse_results={}
    variants=['fixed','frame-cap-negative','stride-negative','stop-gap-negative','live-probe-negative','lock-negative','drain-count-negative']
    strides=[int(sys.argv[sys.argv.index('--stride')+1])] if '--stride' in sys.argv else [2,4]
    assert all(stride in [2,4] for stride in strides)
    for stride in strides:
        for name in variants:
            api=provider;body=functions.copy();drain_fixture=sparse_fixture
            if name=='frame-cap-negative':body[checkpoint_key]=body[checkpoint_key].replace('ami_v6.pending_frames < AMI_V6_SAMPLE_FRAMES)','1)')
            if name=='stride-negative':body[checkpoint_key]=body[checkpoint_key].replace('(ULONG)V6_ECLOCK_STRIDE','1UL')
            if name=='stop-gap-negative':body['static VOID ami_v6_charge_stop(VOID)']=body['static VOID ami_v6_charge_stop(VOID)'].replace('ami_v6.used += (elapsed >= left) ? left : elapsed;','ami_v6.used += (elapsed >= left) ? 0 : 0;')
            if name=='live-probe-negative':api=api.replace(live_provider,'UINT tx_amiga_eclock_live_locked(VOID){return TX_TRUE;}\n')
            if name=='lock-negative':body[checkpoint_key]=re.sub(r'(?m)^[ \t]*(?:Forbid|Permit)\(\);\n','',body[checkpoint_key])
            if name=='drain-count-negative':drain_fixture=drain_fixture.replace('ami_v6_charge_checkpoint((ULONG)taken);','ami_v6_charge_checkpoint((ULONG)taken * 0UL);')
            text=prelude+config+api+data+macros+fixture+''.join(body.values())+drain_fixture+sparse_main
            stem=f's{stride}-{name}';c=build/(stem+'.c');exe=build/stem;c.write_text(text)
            cmd=[os.environ.get('CC','clang'),'-std=c11','-Wall','-Wextra','-Werror','-O1','-g',f'-DV6_ECLOCK_STRIDE={stride}']+shlex.split(os.environ.get('CFLAGS',''))
            if '--sanitize' in sys.argv:cmd+=['-fsanitize=address,undefined','-fno-sanitize-recover=all']
            elif '--ubsan' in sys.argv:cmd+=['-fsanitize=undefined','-fno-sanitize-recover=all']
            subprocess.run(cmd+[str(c),'-o',str(exe)],check=True)
            run=subprocess.run([str(exe)],capture_output=True,text=True);(build/(stem+'.log')).write_text(run.stdout+run.stderr)
            summary=re.search(r'sparse-clock checks=(\d+) failures=(\d+)',run.stdout)
            assert summary and run.returncode==(0 if name=='fixed' else 1),(stem,run.stdout,run.stderr)
            assert (int(summary[2])==0)==(name=='fixed'),(stem,run.stdout)
            targets={'frame-cap-negative':'full frame threshold forces early sample','stride-negative':'skip retains full active interval','stop-gap-negative':'actual stop charges entire pending interval','live-probe-negative':'skipped checkpoint detects retirement','lock-negative':'deferred refill resets skipped interval atomically','drain-count-negative':'real drain call forwards its actual frame count'}
            if name in targets:assert 'FAIL '+targets[name] in run.stdout,(stem,run.stdout)
            sparse_results[stem]={'checks':int(summary[1]),'failures':int(summary[2]),'exit':run.returncode,'source_sha256':hashlib.sha256(text.encode()).hexdigest(),'output':run.stdout};print(stem,run.stdout)
    (build/'results.json').write_text(json.dumps(sparse_results,indent=2)+'\n')
    raise SystemExit(0)

results={}
for name in ['fixed','live-guard-negative','charge-lock-negative','startup-limit-negative',
             'spent-refill-negative','spent-live-negative','unbounded-debit-negative',
             'checkpoint-debit-negative','checkpoint-reread-negative','checkpoint-lock-negative',
             'checkpoint-saturate-negative','checkpoint-priority-negative']:
    api=provider;body=functions.copy()
    if name=='live-guard-negative':
        api=api.replace('        _tx_amiga_tick_run.tr_live == (UINT) TX_FALSE ||\n','')
    if name=='charge-lock-negative':
        for key in ['static VOID ami_v6_charge_start(VOID)','static VOID ami_v6_charge_stop(VOID)']:
            body[key]=re.sub(r'(?m)^[ \t]*(?:Forbid|Permit)\(\);\n','',body[key])
    if name=='startup-limit-negative':
        key='static BOOL ami_v6_clock_try_init(VOID)'
        old='        if (ami_v6_startup_left == 0UL)\n            ami_v6_clock_fail_open();\n'
        assert old in body[key]
        body[key]=body[key].replace(old,'')
    if name=='spent-refill-negative':
        key='static VOID ami_v6_tick(VOID)'
        assert '    if (ami_v6.running)\n' in body[key]
        body[key]=body[key].replace('    if (ami_v6.running)\n','    if (ami_v6.charging)\n')
    if name=='spent-live-negative':
        api=api.replace(live_provider,'UINT tx_amiga_eclock_live_locked(VOID){return TX_TRUE;}\n')
    if name=='unbounded-debit-negative':
        key='static VOID ami_v6_charge_stop(VOID)'
        old='(elapsed >= left) ? left : elapsed'
        assert old in body[key]
        body[key]=body[key].replace(old,'(elapsed >= left) ? elapsed : elapsed')
    if name=='checkpoint-debit-negative':
        body[checkpoint_key]=body[checkpoint_key].replace('ami_v6.used += (elapsed >= left) ? left : elapsed;', 'ami_v6.used += (elapsed >= left) ? 0 : 0;')
    if name=='checkpoint-reread-negative':
        body[checkpoint_key]=body[checkpoint_key].replace('ami_v6.t0 = now;', 'ami_v6.t0 = ami_v6_clock();')
    if name=='checkpoint-lock-negative':
        body[checkpoint_key]=re.sub(r'(?m)^[ \t]*(?:Forbid|Permit)\(\);\n','',body[checkpoint_key])
    if name=='checkpoint-saturate-negative':
        body[checkpoint_key]=body[checkpoint_key].replace('ami_v6.charging = (ami_v6.used < (ULONG)V6_BUDGET);', 'ami_v6.charging = TRUE;')
    if name=='checkpoint-priority-negative':
        body[checkpoint_key]=body[checkpoint_key].replace('(VOID)SetTaskPri(me, (LONG)TX_AMIGA_HANDOFF_PRIORITY);', '(VOID)me;')
    text=prelude+config+api+data+macros+fixture+''.join(body.values())+main
    c=build/(name+'.c');exe=build/name;c.write_text(text)
    cmd=[os.environ.get('CC','clang'),'-std=c11','-Wall','-Wextra','-Werror','-O1','-g']+shlex.split(os.environ.get('CFLAGS',''))
    if '--sanitize' in sys.argv:cmd+=['-fsanitize=address,undefined','-fno-sanitize-recover=all']
    elif '--ubsan' in sys.argv:cmd+=['-fsanitize=undefined','-fno-sanitize-recover=all']
    subprocess.run(cmd+[str(c),'-o',str(exe)],check=True)
    run=subprocess.run([str(exe)],text=True,capture_output=True);(build/(name+'.log')).write_text(run.stdout+run.stderr)
    print(name,run.stdout,run.stderr);summary=re.search(r'native-clock checks=(\d+) failures=(\d+)',run.stdout)
    assert summary and int(summary[1])>=50
    assert run.returncode==(0 if name=='fixed' else 1)
    assert (int(summary[2])==0)==(name=='fixed')
    if name=='live-guard-negative':assert 'FAIL not-live base refused' in run.stdout
    if name=='charge-lock-negative':assert 'FAIL new period not overcharged' in run.stdout
    if name=='startup-limit-negative':assert 'FAIL startup retry exhaustion' in run.stdout
    if name=='spent-refill-negative':assert 'FAIL refill restarts busy spent interval' in run.stdout
    if name=='spent-live-negative':assert 'FAIL spent credit detects retirement' in run.stdout
    if name=='unbounded-debit-negative':assert 'FAIL debit saturates at quota' in run.stdout
    if name=='checkpoint-debit-negative':assert 'FAIL checkpoint charges once' in run.stdout
    if name=='checkpoint-reread-negative':assert 'FAIL checkpoint charges once' in run.stdout
    if name=='checkpoint-lock-negative':assert 'FAIL checkpoint publishes before deferred refill' in run.stdout
    if name=='checkpoint-saturate-negative':assert 'FAIL checkpoint saturates while work remains active' in run.stdout
    if name=='checkpoint-priority-negative':assert 'FAIL checkpoint restores the current high level' in run.stdout
    results[name]={'exit':run.returncode,'checks':int(summary[1]),'failures':int(summary[2]),'source_sha256':hashlib.sha256(text.encode()).hexdigest(),'output':run.stdout}
(build/'results.json').write_text(json.dumps(results,indent=2)+'\n')

# The optional frame arm still uses the native refill clock. In particular a
# latched failure must not recharge the budget and trigger repeated handoffs.
frame_start=source.index('            UWORD taken = ami_sana2_rx_drain')
frame_start=source.rfind('        {',0,frame_start)
frame_end=source.index('\n        }',frame_start)+len('\n        }')
frame=source[frame_start:frame_end]
frame_fixture=r'''
typedef uint16_t UWORD;
#define AMI_SANA2_RX_RUN_MAX 128
static UWORD ami_sana2_rx_drain(VOID *rd,UWORD max){(void)rd;(void)max;return 7;}
static VOID charge_frames(VOID){VOID *rd=0;
'''+frame+'\n}\n'
frame_main=main[:main.index('int main(void)')]+r'''
int main(void){
 reset();bind();ami_v6_charge_start();charge_frames();ami_v6_charge_stop();
 check(ami_v6_clock_stats[1]==128&&ami_v6.used==7,"native frame quota charges frames");
 clock_now=13479;ami_v6.level=0;task.tc_Node.ln_Pri=0;h_tick();
 check(!ami_v6.used&&task.tc_Node.ln_Pri==1,"native refill restores frame reader");
 rate=0;ami_v6.level=0;task.tc_Node.ln_Pri=0;h_tick();charge_frames();ami_v6_settle(FALSE);
 check(ami_v6_clock_stats[4]&&!ami_v6.used&&task.tc_Node.ln_Pri==1,"disabled frame quota cannot recharge after clock failure");
 ami_v6_unbind(&task);check(!ports&&!_tx_amiga_tick_hook,"frame arm unpublishes ports and hook");
 check(!unsafe_reads&&!forbid_depth,"frame clock accesses protected");
 printf("native-frame checks=%u failures=%u\n",checks,failures);return failures?1:0;
}
'''
text=prelude+'\n#define V6_FRAMES 128\n'+config+provider+data+macros+fixture+''.join(functions.values())+frame_fixture+frame_main
c=build/'frames.c';exe=build/'frames';c.write_text(text)
subprocess.run(cmd+[str(c),'-o',str(exe)],check=True)
run=subprocess.run([str(exe)],capture_output=True,text=True);(build/'frames.log').write_text(run.stdout+run.stderr)
print('frames',run.stdout,run.stderr);assert run.returncode==0 and 'native-frame checks=5 failures=0' in run.stdout
results['frames']={'exit':run.returncode,'checks':5,'failures':0,'source_sha256':hashlib.sha256(text.encode()).hexdigest(),'output':run.stdout}
(build/'results.json').write_text(json.dumps(results,indent=2)+'\n')
