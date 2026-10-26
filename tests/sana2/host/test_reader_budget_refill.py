#!/usr/bin/env python3
"""Compile the exact reader-budget functions with a preempting tick model.

The production clock is stubbed; the source functions are extracted unchanged.
The negative control removes only their serialization brackets. This is a
scheduler/accounting regression, not a physical timing or throughput result.
Usage: python3 test_reader_budget_refill.py BUILD_DIR [--sanitize]
"""
from pathlib import Path
import hashlib
import shlex
import json
import os
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[3]
build = Path(sys.argv[1]).resolve()
build.mkdir(parents=True, exist_ok=True)
source = (root / "src/sana2/sana2_rx.c").read_text()

def function(signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"

prefix = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef void VOID;typedef uint32_t ULONG;typedef int8_t BYTE;typedef int32_t LONG;typedef int BOOL;
#define TRUE 1
#define FALSE 0
#define TX_AMIGA_HANDOFF_PRIORITY 1
#define V6_BUDGET 6000U
#define V6_REFILL 19000U
struct Task {struct {BYTE ln_Pri;}tc_Node;};
static struct Task task;
static struct {struct Task*task;ULONG used,t0;BYTE level;BOOL charging;ULONG stats[5];}ami_v6;
static ULONG ami_v6_refilled,clock_now;
static unsigned forbid_depth,inject,pending;
static VOID ami_v6_tick(VOID);
static VOID Forbid(VOID){forbid_depth++;}
static VOID h_tick(VOID){forbid_depth++;ami_v6_tick();forbid_depth--;}
static VOID Permit(VOID){forbid_depth--;if(!forbid_depth&&pending){pending=0;clock_now++;h_tick();}}
static struct Task*FindTask(VOID*x){(void)x;return &task;}
static BYTE SetTaskPri(struct Task*t,LONG p){BYTE old=t->tc_Node.ln_Pri;t->tc_Node.ln_Pri=(BYTE)p;return old;}
/* Simulate a higher-priority tick after the clock read, before its caller continues.
   Forbid defers task preemption; the real tick calls its hook under Forbid. */
static ULONG ami_v6_clock(VOID){ULONG captured=clock_now;if(inject){inject=0;if(forbid_depth)pending=1;else{clock_now++;h_tick();}}return captured;}
'''

main = r'''
static unsigned checks,failures;
static void check(int ok,const char*s){checks++;if(!ok){failures++;printf("FAIL %s\n",s);}}
static void reset(ULONG t){memset(&ami_v6,0,sizeof(ami_v6));ami_v6.task=&task;ami_v6.level=1;task.tc_Node.ln_Pri=1;ami_v6_refilled=0;clock_now=t;forbid_depth=inject=pending=0;}
int main(void){
 reset(100);ami_v6_charge_start();clock_now=200;ami_v6_charge_stop();check(ami_v6.used==100,"normal 100us debit");check(!ami_v6.charging,"stop ends charging");
 reset(0xfffffff0U);ami_v6_charge_start();clock_now=0x10;ami_v6_charge_stop();check(ami_v6.used==32,"clock wrap subtracts32us");
 reset(1000);ami_v6_charge_start();clock_now=19999;inject=1;ami_v6_charge_stop();
 printf("cross-refill: used=%u period_start=%u t0=%u charging=%d\n",ami_v6.used,ami_v6_refilled,ami_v6.t0,ami_v6.charging);
 check(ami_v6_refilled==20000,"fixture crossed a real refill");check(ami_v6.used==0,"new period starts with zero charge after stop/refill");check(!ami_v6.charging,"preempted stop still ends charging");
 ami_v6_settle(FALSE);printf("settle: level=%d priority=%d exhaustions=%u\n",ami_v6.level,task.tc_Node.ln_Pri,ami_v6.stats[1]);
 check(ami_v6.level==1&&task.tc_Node.ln_Pri==1,"fresh period is not spuriously demoted");check(ami_v6.stats[1]==0,"no false budget exhaustion");check(forbid_depth==0&&!pending,"all scheduling brackets balanced");
 reset(19999);inject=1;ami_v6_charge_start();
 check(ami_v6_refilled==20000&&ami_v6.t0==20000,"start/refill establishes new interval origin");
 clock_now=20001;ami_v6_charge_stop();check(ami_v6.used==1,"start/refill charges only new-period elapsed time");
 reset(1000);ami_v6_charge_start();Forbid();clock_now=19999;inject=1;ami_v6_charge_stop();
 check(forbid_depth==1&&pending==1,"nested stop preserves caller Forbid and pending tick");
 check(ami_v6.used==18999,"nested stop charges old period before pending refill");
 Permit();check(ami_v6_refilled==20000&&ami_v6.used==0,"outer Permit delivers pending refill");
 check(forbid_depth==0&&!pending,"nested scheduling brackets balanced");
 reset(1234);inject=1;ami_v6_charge_stop();
 check(inject==1&&ami_v6.used==0&&!ami_v6.charging,"inactive stop does not read the clock");
 check(forbid_depth==0,"inactive stop balances scheduling bracket");
 printf("%u checks, %u failures\n",checks,failures);return failures?1:0;
}
'''

start = function("static VOID ami_v6_charge_start(VOID)")
stop = function("static VOID ami_v6_charge_stop(VOID)")
tick = function("static VOID ami_v6_tick(VOID)")
settle = function("static VOID ami_v6_settle(BOOL overrun)")

def unserialized(body):
    assert body.count("    Forbid();\n") == 1
    assert body.count("    Permit();\n") >= 1
    return body.replace("    Forbid();\n", "").replace("    Permit();\n", "")

results = {}
for name, functions in [("serialized", start + stop),
                        ("unserialized-negative", unserialized(start) + unserialized(stop))]:
    text = prefix + functions + tick + settle + main
    c = build / (name + ".c")
    exe = build / name
    c.write_text(text)
    command = [os.environ.get("CC", "clang"), "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g"]
    command += shlex.split(os.environ.get("CFLAGS", ""))
    if "--ubsan" in sys.argv:
        command += ["-fsanitize=undefined", "-fno-sanitize-recover=all"]
    if "--sanitize" in sys.argv:
        command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    subprocess.run(command + [str(c), "-o", str(exe)], check=True)
    run = subprocess.run([str(exe)], text=True, capture_output=True)
    (build / (name + ".log")).write_text(run.stdout + run.stderr)
    print(name, run.stdout, run.stderr)
    assert run.returncode == (0 if name == "serialized" else 1)
    summary = re.search(r"(\d+) checks, (\d+) failures", run.stdout)
    assert summary and int(summary[1]) >= 17 and int(summary[2]) == (0 if name == "serialized" else 5)
    results[name] = {"exit": run.returncode, "source_sha256": hashlib.sha256(text.encode()).hexdigest(),
                     "output": run.stdout}
(build / "results.json").write_text(json.dumps(results, indent=2) + "\n")

# Frame-count builds share the same refill writer. Check the actual drain
# boundary's accesses under scheduling exclusion as well as its charge value.
frame_start = source.index('            UWORD taken = ami_sana2_rx_drain')
frame_start = source.rfind('        {', 0, frame_start)
frame_end = source.index('\n        }', frame_start) + len('\n        }')
frame = source[frame_start:frame_end]
frame_prefix = prefix + r'''
typedef uint16_t UWORD;
#define AMI_SANA2_RX_RUN_MAX 128
static unsigned unprotected;
static UWORD ami_sana2_rx_drain(VOID *rd, UWORD max) {(void)rd;(void)max;return 7;}
static __typeof__(ami_v6) *frame_state(VOID) {
    if (!forbid_depth) unprotected++;
    return &ami_v6;
}
#define ami_v6 (*frame_state())
static VOID charge_frames(VOID) {VOID *rd=0;
'''
frame_main = r'''
}
#undef ami_v6
static unsigned checks,failures;
static void check(int ok,const char*s){checks++;if(!ok){failures++;printf("FAIL %s\n",s);}}
int main(void) {
 ami_v6.task=&task;ami_v6.used=10;charge_frames();
 check(ami_v6.used==17,"seven drained frames charged");
 check(forbid_depth==0,"frame charge balances exclusion");
 ami_v6.task=0;charge_frames();
 check(ami_v6.used==17,"other reader does not charge bound reader");
 check(forbid_depth==0,"other reader balances exclusion");
 check(unprotected==0,"frame task and debit accesses protected from refill");
 printf("frame checks=%u failures=%u\n",checks,failures);return failures?1:0;
}
'''
for name, body in [('frames', frame),
                   ('frames-unprotected-negative', frame.replace('            Forbid();\n','').replace('            Permit();\n',''))]:
    # Reuse the original model clock/tick definitions so compiler warnings
    # still cover every extracted helper; only frame boundary is exercised.
    text = prefix + start + stop + tick + settle
    text += frame_prefix[len(prefix):] + body + frame_main
    # The time helpers are not exercised by this separate frame executable.
    text = text.replace('static VOID ami_v6_charge_start(VOID)', 'VOID ami_v6_charge_start(VOID)')
    text = text.replace('static VOID ami_v6_charge_stop(VOID)', 'VOID ami_v6_charge_stop(VOID)')
    text = text.replace('static VOID ami_v6_settle(BOOL overrun)', 'VOID ami_v6_settle(BOOL overrun)')
    c=build/(name+'.c');exe=build/name;c.write_text(text)
    subprocess.run(command+[str(c),'-o',str(exe)],check=True)
    run=subprocess.run([str(exe)],text=True,capture_output=True)
    (build/(name+'.log')).write_text(run.stdout+run.stderr)
    negative=name.endswith('-negative')
    assert run.returncode==int(negative)
    assert f'frame checks=5 failures={int(negative)}' in run.stdout
    results[name]={'exit':run.returncode,'source_sha256':hashlib.sha256(text.encode()).hexdigest(),'output':run.stdout}
    print(name,run.stdout,run.stderr)
(build/'results.json').write_text(json.dumps(results,indent=2)+'\n')
