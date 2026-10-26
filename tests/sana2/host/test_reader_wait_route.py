#!/usr/bin/env python3
"""Preserve shipped defaults and route explicit reader experiments correctly."""
from pathlib import Path
import hashlib
import json
import os
import re
import shlex
import subprocess
import sys

root = Path(__file__).resolve().parents[3]
build = Path(sys.argv[1]).resolve()
build.mkdir(parents=True, exist_ok=True)
source = (root / 'src/sana2/sana2_rx.c').read_text()
device = (root / 'src/sana2/sana2_device.c').read_text()
netstack = (root / 'src/netstack/netstack.c').read_text()
port = (root / 'port/threadx-amiga/inc/tx_port.h').read_text()

def function(text, signature):
    start = text.index(signature)
    end = text.index('\n}', start) + 2
    return text[start:end] + '\n'

body = function(source, 'static VOID ami_sana2_rx_wait_begin(VOID)')
start = port.index('#ifndef TX_AMIGA_TASK_PRIORITY\n')
priorities = port[start:port.index('/* Exec priority of the periodic tick Task', start)]
start = netstack.index('    ami_sana2_set_block_hooks(')
registration = netstack[start:netstack.index('    ami_netstack_mark("start");', start)]
hooks = ''.join(function(device, signature) for signature in [
    'VOID ami_sana2_set_block_hooks(', 'VOID ami_sana2_block_enter(VOID)',
    'VOID ami_sana2_set_reader_wait_hook(', 'VOID ami_sana2_reader_wait_enter(VOID)'])
prefix = r'''
#include <stdio.h>
typedef void VOID;
typedef void (*AmiSana2BlockHook)(VOID);
#define FALSE 0
static AmiSana2BlockHook ami_block_enter, ami_block_leave, ami_reader_wait;
static int priority, released, charged, settled, bad_order, boosted;
VOID ami_netstack_baton_release(VOID) { released++; }
VOID ami_netstack_baton_acquire(VOID) { }
VOID ami_netstack_baton_release_reader(VOID) { released++; boosted++; priority=TX_AMIGA_HANDOFF_PRIORITY; }
VOID ami_v6_charge_stop(VOID) { charged++; }
VOID ami_v6_settle(int overrun) {
    if (!released || !charged || overrun) bad_order++;
    settled++; priority=0;
}
'''
main = r'''
int main(void) {
    priority=TX_AMIGA_TASK_PRIORITY;
    initialize_hooks();
    ami_sana2_rx_wait_begin();
#if defined(V6_TIME_US) || defined(V6_FRAMES)
    int fails=(TX_AMIGA_TASK_PRIORITY!=EXPECT_BASE)+(priority!=0)+(released!=1)+(charged!=1)+(settled!=1)+(bad_order!=0)+(boosted!=0);
#else
    int want_boost=(EXPECT_BASE==0);
    int fails=(TX_AMIGA_TASK_PRIORITY!=EXPECT_BASE)+(priority!=(want_boost ? 1 : EXPECT_BASE))+(released!=1)+(charged!=0)+(settled!=0)+(bad_order!=0)+(boosted!=want_boost);
#endif
    printf("wait-route checks=7 failures=%d\n",fails);
    return fails ? 1 : 0;
}
'''
results = {}
for name, flags, actual, priority_source, hook_source, failures in [
    ('ordinary', ['-DEXPECT_BASE=1'], body, priorities, registration, 0),
    ('explicit-band-zero', ['-DEXPECT_BASE=0', '-DTX_AMIGA_TASK_PRIORITY=0'], body, priorities, registration, 0),
    ('time-budget', ['-DEXPECT_BASE=0', '-DV6_TIME_US=6000'], body, priorities, registration, 0),
    ('frame-budget', ['-DEXPECT_BASE=0', '-DV6_FRAMES=128'], body, priorities, registration, 0),
    ('native-time-budget', ['-DEXPECT_BASE=0', '-DV6_TIME_US=6000', '-DV6_ECLOCK=1'], body, priorities, registration, 0),
    ('lost-wake-boost-negative', ['-DEXPECT_BASE=0', '-DTX_AMIGA_TASK_PRIORITY=0'],
     body.replace('ami_sana2_reader_wait_enter();', 'ami_sana2_block_enter();'), priorities, registration, 2),
    ('ordinary-band-leak-negative', ['-DEXPECT_BASE=1'], body,
     priorities.replace('#define TX_AMIGA_TASK_PRIORITY                  1', '#define TX_AMIGA_TASK_PRIORITY                  0'), registration, 2),
    ('ordinary-boost-leak-negative', ['-DEXPECT_BASE=1'], body, priorities,
     registration.replace(' && TX_AMIGA_TASK_PRIORITY == 0', ''), 2),
]:
    text = priority_source + prefix + hooks + 'static VOID initialize_hooks(VOID) {\n' + hook_source + '}\n' + actual + main
    c = build / (name + '.c')
    exe = build / name
    c.write_text(text)
    cmd = [os.environ.get('CC', 'clang'), '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g']
    cmd += shlex.split(os.environ.get('CFLAGS', '')) + flags
    if '--sanitize' in sys.argv:
        cmd += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    elif '--ubsan' in sys.argv:
        cmd += ['-fsanitize=undefined', '-fno-sanitize-recover=all']
    subprocess.run(cmd + [str(c), '-o', str(exe)], check=True)
    run = subprocess.run([str(exe)], text=True, capture_output=True)
    assert run.returncode == int(failures != 0)
    match = re.search(r'wait-route checks=7 failures=(\d+)', run.stdout)
    assert match and int(match[1]) == failures
    (build / (name + '.log')).write_text(run.stdout + run.stderr)
    print(name, run.stdout, run.stderr)
    results[name] = {'exit': run.returncode, 'checks': 7, 'failures': failures,
                     'source_sha256': hashlib.sha256(text.encode()).hexdigest(), 'output': run.stdout}
(build / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
