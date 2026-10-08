#!/usr/bin/env python3
"""Inventory real, preprocessed production consumers; not a semantic proof.

Use a configured parent build's compile_commands.json. Headers and macros are
expanded with its actual compiler/defines, but references inside header declarations
are excluded. Compiled source coverage over-approximates the linked dependencies.
SPDX-License-Identifier: MIT
"""
import argparse
import concurrent.futures
import hashlib
import json
import re
import shlex
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NX = ROOT / 'third_party/netxduo'
TX = ROOT / 'third_party/threadx'


def scan(entry):
    path = Path(entry['file']).resolve()
    args = list(entry.get('arguments') or shlex.split(entry['command']))
    command = []
    skip = False
    for arg in args:
        if skip:
            skip = False
        elif arg in ('-o', '-MF', '-MT', '-MQ'):
            skip = True
        elif arg not in ('-c', '-MD', '-MMD', '-MP'):
            command.append(arg)
    command.append('-E')
    run = subprocess.run(command, cwd=entry['directory'], text=True,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if run.returncode:
        raise RuntimeError(str(path) + ': ' + run.stderr[-2000:])
    body = []
    own = False
    for line in run.stdout.splitlines():
        marker = re.match(r'#\s+\d+\s+"([^"]+)"', line)
        if marker:
            named = marker.group(1)
            own = (Path(entry['directory']) / named).resolve() == path
        elif own:
            body.append(line)
    text = '\n'.join(body)
    # Strip comments and literals from the unexpanded source for macro inventory.
    raw = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                 '', path.read_text(), flags=re.S)
    calls = set(re.findall(r'\b(_tx_\w+|_txe_\w+|_txr_\w+|tx_\w+)\s*\(', text))
    identifiers = set(re.findall(r'\b_tx_\w+\b', text)) - calls
    fields = set(re.findall(r'(?:->|\.)\s*(tx_(?:thread|timer|mutex|event_flags|semaphore|queue|block_pool|byte_pool)_\w+)', text))
    macros = set(re.findall(r'\bTX_[A-Z0-9_]+\b', raw))
    return path.relative_to(ROOT).as_posix(), {
        'calls': sorted(calls), 'internal_identifiers': sorted(identifiers),
        'fields': sorted(fields), 'source_macros_and_types': sorted(macros)}


def inventory(database, profile, jobs):
    entries = json.loads(database.read_text())
    selected = {}
    for entry in entries:
        path = Path(entry['file']).resolve()
        if path.suffix != '.c' or not path.is_relative_to(ROOT):
            continue
        relative = path.relative_to(ROOT).as_posix()
        if relative.startswith(('src/', 'port/threadx-amiga/', 'third_party/netxduo/')):
            if '/test/' not in relative and '/tests/' not in relative:
                selected.setdefault(path, entry)
    if not selected:
        raise RuntimeError('No production consumers in ' + str(database))
    files = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for path, result in pool.map(scan, selected.values()):
            if any(result.values()):
                files[path] = result
    aggregate = {kind: sorted({name for f in files.values() for name in f[kind]})
                 for kind in ('calls', 'internal_identifiers', 'fields', 'source_macros_and_types')}
    return {'profile': profile, 'production_files_preprocessed': len(selected),
            'files_with_references': dict(sorted(files.items())), 'aggregate': aggregate}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--database', type=Path, required=True)
    ap.add_argument('--profile', choices=['full', 'micro', 'minimal'], required=True)
    ap.add_argument('--output', type=Path)
    ap.add_argument('--check', type=Path)
    ap.add_argument('--jobs', type=int, default=4)
    args = ap.parse_args()
    if bool(args.output) == bool(args.check):
        ap.error('choose exactly one of --output or --check')
    if args.jobs < 1:
        ap.error('--jobs must be positive')
    result = inventory(args.database.resolve(), args.profile, args.jobs)
    result['netxduo_pin'] = subprocess.check_output(['git', '-C', str(NX), 'rev-parse', 'HEAD'], text=True).strip()
    result['threadx_pin'] = subprocess.check_output(['git', '-C', str(TX), 'rev-parse', 'HEAD'], text=True).strip()
    result['method'] = 'configured compiler preprocessing; own source lines only; compiled, not linked coverage'
    result['threadx_header_sha256'] = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
        for p in sorted((TX / 'common/inc').glob('tx_*.h'))}
    if args.check:
        old = json.loads(args.check.read_text())
        if result != old:
            for kind in result['aggregate']:
                added = sorted(set(result['aggregate'][kind]) - set(old['aggregate'][kind]))
                removed = sorted(set(old['aggregate'][kind]) - set(result['aggregate'][kind]))
                if added or removed:
                    print(kind, 'added=', added, 'removed=', removed)
            raise SystemExit('research_contract=CHANGED review pins, headers and per-file contract before updating baseline')
        print('research_contract=UNCHANGED profile=' + args.profile)
    else:
        args.output.write_text(json.dumps(result, indent=2) + '\n')
        print('research_contract=CAPTURED profile=' + args.profile,
              'files=' + str(result['production_files_preprocessed']),
              'calls=' + str(len(result['aggregate']['calls'])),
              'fields=' + str(len(result['aggregate']['fields'])))


if __name__ == '__main__':
    main()
