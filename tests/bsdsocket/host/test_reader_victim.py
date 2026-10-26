#!/usr/bin/env python3
"""Compile the actual bounded victim walk and in-Task marker helper.

Usage: python3 test_reader_victim.py BUILD_DIR [--sanitize]
Negative controls independently break caller continuation, root lifetime, and
the shared scan limit. These are source regressions, not timing measurements.
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
source = (root / 'src/bsdsocket/reader_victim.c').read_text()
port = (root / 'port/threadx-amiga/src/tx_thread_interrupt_control.c').read_text()
start = port.index('UINT tx_amiga_task_may_be_internal(VOID *task)')
end = port.index('{', start) + 1
depth = 1
while depth:
    depth += (port[end] == '{') - (port[end] == '}')
    end += 1
helper = '#include "reader_victim_host.h"\n' + port[start:end] + '\n'
(build / 'marker.c').write_text(helper)
clock_port = (root / 'port/threadx-amiga/src/tx_timer_interrupt.c').read_text()
clock_start = clock_port.index('ULONG tx_amiga_time_get_locked(VOID)')
clock_end = clock_port.index('\n}', clock_start) + 2
clock = '#include "reader_victim_host.h"\nextern ULONG _tx_timer_system_clock;\n' + clock_port[clock_start:clock_end] + '\n'
(build / 'clock.c').write_text(clock)


continuation = 'node = node->ln_Succ; /* a caller must not hide a later decoder */'
assert source.count(continuation) == 1
early_root = source.replace('    master = *master_slot;\n', '')
early_root = early_root.replace('    Disable();', '    master = *master_slot;\n    Disable();')
cap = 'if (left == 0)'
assert source.count(cap) == 2
variants = {
    'fixed': source,
    'caller-hides-decoder-negative': source.replace(continuation, 'goto done;'),
    'early-root-negative': early_root,
    'missing-shared-limit-negative': source.replace(cap, 'if (FALSE)', 2),
}
results = {}
for name, text in variants.items():
    c = build / (name + '.c')
    exe = build / name
    c.write_text(text)
    cmd = [os.environ.get('CC', 'clang'), '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
           '-DV6_GATED=1', '-DAMINETXDUO_READER_VICTIM_HOST',
           '-I' + str(root / 'tests/bsdsocket/host')]
    cmd += shlex.split(os.environ.get('CFLAGS', ''))
    if '--ubsan' in sys.argv:
        cmd += ['-fsanitize=undefined', '-fno-sanitize-recover=all']
    if '--sanitize' in sys.argv:
        cmd += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    subprocess.run(cmd + [str(root / 'tests/bsdsocket/host/test_reader_victim.c'),
                         str(c), str(build / 'marker.c'), str(build / 'clock.c'), '-o', str(exe)], check=True)
    run = subprocess.run([str(exe)], text=True, capture_output=True)
    (build / (name + '.log')).write_text(run.stdout + run.stderr)
    print(name, run.returncode, run.stdout, run.stderr)
    assert run.returncode == (0 if name == 'fixed' else 1)
    summary = re.search(r'reader-victim checks=(\d+) failures=(\d+)', run.stdout)
    expected_failures = {'fixed': 0, 'caller-hides-decoder-negative': 2,
                         'early-root-negative': 1, 'missing-shared-limit-negative': 2}
    assert summary and int(summary[1]) >= 60 and int(summary[2]) == expected_failures[name]
    results[name] = {'exit': run.returncode, 'source_sha256': hashlib.sha256(text.encode()).hexdigest(),
                     'marker_sha256': hashlib.sha256(helper.encode()).hexdigest(),
                     'clock_sha256': hashlib.sha256(clock.encode()).hexdigest(), 'output': run.stdout}
(build / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
