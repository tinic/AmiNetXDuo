#!/usr/bin/env python3
"""Replay actual reader gate/busy boundary, including late-arriving victims.
Usage: python3 test_reader_budget_gate.py BUILD_DIR [--sanitize]
"""
from pathlib import Path
import ast, hashlib, json, os, subprocess, sys
root = Path(__file__).resolve().parents[3]
build = Path(sys.argv[1]).resolve()
build.mkdir(parents=True, exist_ok=True)
source = (root / 'src/sana2/sana2_rx.c').read_text()
model = ast.parse((Path(__file__).with_name('test_reader_budget_refill.py')).read_text())
prefix = next(ast.literal_eval(n.value) for n in model.body
              if isinstance(n, ast.Assign) and any(isinstance(t,ast.Name) and t.id=='prefix' for t in n.targets))
def block(signature):
    start=source.index(signature); end=source.index('{',start)+1; depth=1
    while depth:
        depth += (source[end]=='{')-(source[end]=='}'); end+=1
    return source[start:end]+'\n'
fixture = '#define V6_GATED 1\n#define AMI_SANA2_RX_RUN_MAX 128\nstatic unsigned eligible,releases,gate_calls;\nstatic BOOL victim_hook(VOID){gate_calls++;return eligible;}\nstatic BOOL (*ami_sana2_budget_victim_hook)(VOID)=victim_hook;\nstatic BOOL ami_sana2_rx_should_block(VOID*r,unsigned n){(void)r;(void)n;return FALSE;}\nstatic VOID ami_sana2_block_enter(VOID){releases++;}\nstatic VOID ami_sana2_block_leave(VOID){}\n'
main = 'static unsigned checks,failures;\nstatic void check(int ok,const char*s){checks++;if(!ok){failures++;printf("FAIL %s\\n",s);}}\nstatic void reset(void){memset(&ami_v6,0,sizeof(ami_v6));ami_v6.task=&task;ami_v6.level=1;task.tc_Node.ln_Pri=1;ami_v6.used=6000;ami_v6_refilled=0;clock_now=1000;forbid_depth=inject=pending=eligible=releases=gate_calls=0;ami_sana2_budget_victim_hook=victim_hook;}\nint main(void){\n reset();ami_v6_settle(FALSE);check(ami_v6.level==1&&task.tc_Node.ln_Pri==1,"no victim leaves high level");check(ami_v6.stats[1]==0,"no victim records no exhaustion");\n busy_boundary();check(releases==0,"no victim avoids overrun handoff");\n eligible=1;busy_boundary();check(releases==1&&ami_v6.level==0,"late victim yields and demotes at next busy release");check(ami_v6.stats[1]==1&&ami_v6.stats[2]==1,"one demotion and one overrun");\n unsigned previous=gate_calls;busy_boundary();ami_v6_settle(FALSE);check(releases==1&&gate_calls==previous,"low level neither rescans nor yields repeatedly");\n clock_now=20000;h_tick();check(ami_v6.level==1&&task.tc_Node.ln_Pri==1,"refill restores high level");\n reset();ami_sana2_budget_victim_hook=0;busy_boundary();ami_v6_settle(FALSE);check(releases==0&&ami_v6.level==1,"absent classifier is unknown");\n check(forbid_depth==0,"all scheduling brackets balanced");\n printf("gate-integration checks=%u failures=%u\\n",checks,failures);return failures?1:0;\n}\n'

functions=''.join(block(s) for s in ['static BOOL ami_v6_has_victim(VOID)',
    'static VOID ami_v6_charge_start(VOID)', 'static VOID ami_v6_charge_stop(VOID)',
    'static VOID ami_v6_tick(VOID)', 'static VOID ami_v6_settle(BOOL overrun)'])
busy=block('if (FindTask(NULL) == ami_v6.task &&')
assert busy.count('            ami_v6_has_victim() &&\n')==1
results={}
for name,body in [('fixed',busy),('missing-busy-gate-negative',busy.replace('            ami_v6_has_victim() &&\n',''))]:
    text=prefix+fixture+functions+'static VOID busy_boundary(VOID){VOID*rd=0;\n'+body+'}\n'+main
    c=build/(name+'.c'); exe=build/name; c.write_text(text)
    cmd=[os.environ.get('CC','clang'),'-std=c11','-Wall','-Wextra','-Werror','-O1','-g']
    if '--sanitize' in sys.argv: cmd+=['-fsanitize=address,undefined','-fno-sanitize-recover=all']
    subprocess.run(cmd+[str(c),'-o',str(exe)],check=True)
    run=subprocess.run([str(exe)],text=True,capture_output=True)
    (build/(name+'.log')).write_text(run.stdout+run.stderr)
    print(name,run.returncode,run.stdout,run.stderr)
    assert run.returncode==(0 if name=='fixed' else 1)
    results[name]={'exit':run.returncode,'source_sha256':hashlib.sha256(text.encode()).hexdigest(),'output':run.stdout}
(build/'results.json').write_text(json.dumps(results,indent=2)+'\n')
