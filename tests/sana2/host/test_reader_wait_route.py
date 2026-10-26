#!/usr/bin/env python3
"""Preserve ordinary wake boosting while the diagnostic owns its priorities."""
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
start = source.index('static VOID ami_sana2_rx_wait_begin(VOID)')
end = source.index('\n}', start) + 2
body = source[start:end]
prefix = r'''
#include <stdio.h>
typedef void VOID;
#define FALSE 0
static int priority, released, charged, settled, bad_order;
VOID ami_sana2_block_enter(VOID) { released++; }
VOID ami_sana2_reader_wait_enter(VOID) { released++; priority=1; }
VOID ami_v6_charge_stop(VOID) { charged++; }
VOID ami_v6_settle(int overrun) {
    if (!released || !charged || overrun) bad_order++;
    settled++; priority=0;
}
'''
main = r'''
int main(void) {
    ami_sana2_rx_wait_begin();
#if defined(V6_TIME_US) || defined(V6_FRAMES)
    int fails=(priority!=0)+(released!=1)+(charged!=1)+(settled!=1)+(bad_order!=0);
#else
    int fails=(priority!=1)+(released!=1)+(charged!=0)+(settled!=0)+(bad_order!=0);
#endif
    printf("wait-route checks=5 failures=%d\n",fails);
    return fails ? 1 : 0;
}
'''
results = {}
for name, flags, actual in [
    ('ordinary', [], body),
    ('time-budget', ['-DV6_TIME_US=6000'], body),
    ('frame-budget', ['-DV6_FRAMES=128'], body),
    ('lost-wake-boost-negative', [], body.replace('ami_sana2_reader_wait_enter();',
                                                 'ami_sana2_block_enter();')),
]:
    text = prefix + actual + main
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
    failed = (name == 'lost-wake-boost-negative')
    assert run.returncode == int(failed)
    match = re.search(r'wait-route checks=5 failures=(\d+)', run.stdout)
    assert match and int(match[1]) == int(failed)
    (build / (name + '.log')).write_text(run.stdout + run.stderr)
    print(name, run.stdout, run.stderr)
    results[name] = {'exit': run.returncode, 'source_sha256': hashlib.sha256(text.encode()).hexdigest(),
                     'output': run.stdout}
(build / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
