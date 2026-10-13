#!/usr/bin/env python3
"""Release-inventory CLI cases; no shell executes generated arguments."""
import argparse
import itertools
import os
import json
import re
from pathlib import Path

ROOT = Path(os.environ.get('AMINETXDUO_CLI_ROOT', Path(__file__).resolve().parents[3])).resolve()
POLICY = Path(__file__).with_name('policy.json')


def strip_comments(text):
    return re.sub(r'"(?:\\.|[^"\\])*"|/\*.*?\*/|//[^\n]*',
                  lambda m: m.group(0) if m.group(0).startswith('"') else ' ', text, flags=re.S)


def inventory(root=ROOT):
    text = (root / 'dist/make-dist.sh').read_text()
    text = '\n'.join(x for x in text.splitlines() if not x.lstrip().startswith('#'))
    match = re.search(r'^CMDS=\((.*?)\)', text, re.M | re.S)
    if not match:
        raise ValueError('release CMDS inventory missing')
    tools = set(match.group(1).split())
    # Both C/ clients and executable installer companions, including Installer.
    tools.update(re.findall(r'cp\s+"[^"\n]+"\s+"\$TREE/(?:[^\"\n]*/)?([\w-]+)"', text))
    tools.update(re.findall(r'cp\s+"[^"\n]+/([\w-]+)"\s+"\$TREE/[^"\n]+/"', text))
    tools.discard('certificates')
    tools.discard('WirelessManager-LICENSE')
    tools.discard('Install-AmiNetXDuo')  # Installer script, not an executable.
    tools.discard('Uninstall-AmiNetXDuo')
    tools.discard('LICENSE')
    tools.discard('ReadMe')  # Packaged documentation.
    return tools


def readargs_template(root, spec):
    if spec['parser'] != 'readargs':
        return spec.get('template', '')
    text = strip_comments((root / spec['source']).read_text())
    defines = re.findall(r'#define\s+TEMPLATE\s+((?:[^\n]*\\\n)*[^\n]*)', text)
    if defines:
        index = spec.get('template_index', 0)
        if index >= len(defines):
            raise ValueError('missing template variant: ' + spec['source'])
        return ''.join(re.findall(r'"([^"\n]*)"', defines[index]))
    match = re.search(r'ReadArgs\(\s*\(CONST_STRPTR\)\s*"([^"]+)"', text)
    if not match:
        raise ValueError('ReadArgs template not found: ' + spec['source'])
    return match.group(1)


def fields(template):
    answer = []
    for item in template.split(','):
        if not item:
            continue
        bits = item.split('/')
        answer.append((bits[0].split('='), ''.join(bits[1:]).upper()))
    return answer


def validate(root=ROOT, policy=None):
    policy = policy or json.loads(POLICY.read_text())
    shipped = inventory(root)
    if shipped != set(policy):
        raise ValueError('inventory mismatch: missing=%s stale=%s' %
                         (sorted(shipped - set(policy)), sorted(set(policy) - shipped)))
    for tool, spec in policy.items():
        if not 1 <= spec.get('seconds', 3) <= 120:
            raise ValueError(tool + ' deadline must be 1..120 guest seconds')
        if spec.get('mode', 'exit') not in ('exit', 'interrupt'):
            raise ValueError(tool + ' invalid lifecycle policy')
        if 'binary_sha256' in spec:
            source = root / (spec['source'] if 'source' in spec else 'install/Installer')
            import hashlib
            if hashlib.sha256(source.read_bytes()).hexdigest() != spec['binary_sha256']:
                raise ValueError(tool + ' vendored executable changed; review its option contract')
        if spec['parser'] in ('readargs', 'installer', 'vendored-readargs'):
            template = readargs_template(root, spec)
            names = {aliases[0] for aliases, _ in fields(template)}
            if names != set(spec['values']):
                raise ValueError('%s options changed: missing=%s stale=%s' %
                                 (tool, sorted(names - set(spec['values'])),
                                  sorted(set(spec['values']) - names)))
            if set(spec.get('interrupt_options', [])) - names:
                raise ValueError(tool + ' unknown interrupt option')
        elif spec['parser'] == 'ssh':
            src = (root / spec['source']).read_text()
            body = src.split('void cli_getopts(', 1)[1].split('/* Handle the hostname', 1)[0]
            keys = set(re.findall(r"case '([^'])':", body))
            if keys != set(spec['values']):
                raise ValueError('SSH short options changed: ' + str(keys ^ set(spec['values'])))
            ext = set(re.findall(r'match_extendedopt\(&optstr, "([^"]+)"', src))
            if ext != set(spec['extended']):
                raise ValueError('SSH extended options changed: ' + str(ext ^ set(spec['extended'])))
        elif spec['parser'] == 'scp':
            src = (root / spec['source']).read_text()
            opts = re.search(r'getopt\(argc, argv, "([^"]+)"', src).group(1)
            if set(opts.replace(':', '')) != set(spec['values']):
                raise ValueError('SCP options changed')
        elif spec['parser'] == 'none':
            src = (root / spec['source']).read_text()
            if re.search(r'ReadArgs\(|getopt\(', strip_comments(src)):
                raise ValueError(tool + ' gained a parser')
        else:
            raise ValueError('unknown parser for ' + tool)
    return policy


def quote(value):
    # AmigaDOS escaping; NP_Arguments receives this directly, never via a Shell.
    if '\n' in value or '\r' in value or '\t' in value or '\x00' in value:
        raise ValueError('unrepresentable case argument')
    return '"' + value.replace('*', '**').replace('"', '*"') + '"'


def tool_cases(tool, spec, root=ROOT):
    parser = spec['parser']
    # Installer reports argument/script errors through modal requesters.
    syntax_mode = spec.get('mode', 'exit') if parser in ('none', 'installer') else 'exit'
    rows = []
    def add(kind, args, mode=None, allowed=None):
        if mode is None and set(kind.split(':')[1:]) & set(spec.get('interrupt_options', [])):
            mode = 'interrupt'
        rows.append(dict(tool=tool, kind=kind, arguments=args,
                         mode=mode or spec.get('mode', 'exit'),
                         allowed=allowed or spec.get('allowed', [0, 5, 10, 20]),
                         seconds=spec.get('seconds', 1 if (mode or spec.get('mode', 'exit')) == 'interrupt' else 3)))
    add('empty', '')
    # ReadArgs '?' prompts for a replacement line. With NIL input it can
    # continue using defaults, so resident tools retain their normal lifecycle.
    add('help', spec.get('help', '?'), spec.get('mode', 'exit'))
    add('unknown', 'ANXD_UNKNOWN_OPTION=bad' if parser in ('readargs', 'installer', 'vendored-readargs') else '-@', syntax_mode)
    add('unterminated-quote', '"unterminated', syntax_mode)
    if parser == 'none':
        add('extra-long', quote('x' * 2048))
        return rows
    if parser in ('readargs', 'installer', 'vendored-readargs'):
        fs = fields(readargs_template(root, spec))
        base = {aliases[0]: spec['values'][aliases[0]] for aliases, mods in fs if 'A' in mods}
        base.update(spec.get('base', {}))
        def line(change=None, alias=None, omit=None, raw=None):
            values = dict(base)
            values.update(change or {})
            if omit:
                values.pop(omit, None)
            args = []
            for aliases, mods in fs:
                name = aliases[0]
                if name not in values:
                    continue
                val = values[name]
                key = alias if alias in aliases else name
                if 'S' in mods or 'T' in mods:
                    if val:
                        args.append(key)
                else:
                    # Always spell the keyword, even for positional fields, to
                    # prevent an omitted field shifting a later value into it.
                    args.extend([key, str(val) if 'F' in mods else quote(str(val))])
            if raw:
                args.append(raw)
            return ' '.join(args)
        add('baseline', line())
        for aliases, mods in fs:
            name = aliases[0]
            val = spec['values'][name]
            for alias in aliases:
                add('option:' + alias, line({name: val}, alias))
            add('duplicate:' + name, line({name: val}) + ' ' +
                (name if 'S' in mods else name + ' ' + quote(str(val))))
            if 'S' in mods or 'T' in mods:
                add('invalid-switch:' + name, line({name: val}) + ' "invalid-value"', syntax_mode)
            else:
                for tag, bad in [('empty', ''), ('invalid', 'not-a-value'), ('long', 'x' * 2048),
                                 ('quoted', 'x * " / : =')]:
                    add(tag + ':' + name, line({name: bad}))
                add('missing-value:' + name, line(omit=name, raw=name), syntax_mode)
                if 'N' in mods:
                    for num in ('-1', '0', '1', '2147483647', '-2147483648',
                                '2147483648', '-2147483649', '99999999999999999999'):
                        add('number:' + name + ':' + num, line({name: num}))
                if 'M' in mods:
                    add('multiple:' + name, line({name: val}) + ' ' + quote(str(val)))
                if 'A' in mods:
                    add('missing-required:' + name, line(omit=name), syntax_mode)
        # Every pair with valid representations, including conflicting modes.
        for a, b in itertools.combinations([a[0] for a, _ in fs], 2):
            add('pair:' + a + ':' + b, line({a: spec['values'][a], b: spec['values'][b]}))
        return rows
    # libc-style clients: cover even compiled-out flags (their refusal is part
    # of the CLI contract), and every supported -o configuration spelling.
    base = spec.get('base_args', '127.0.0.1')
    for opt, val in spec['values'].items():
        arg = '-' + opt + ((' ' + quote(str(val))) if val is not None else '')
        add('option:-' + opt, arg + ' ' + base)
        add('duplicate:-' + opt, arg + ' ' + arg + ' ' + base)
        if val is not None:
            add('missing-value:-' + opt, '-' + opt, 'exit')
            for bad in ('', 'not-a-value', '-1', '0', '2147483648', 'x' * 2048):
                add('invalid:-' + opt + ':' + str(len(bad)), '-' + opt + ' ' + quote(bad) + ' ' + base)
    for opt, val in spec.get('extended', {}).items():
        for bad in (val, '', 'not-a-value', 'x' * 2048):
            add('extended:' + opt + ':' + str(len(bad)), '-o ' + quote(opt + '=' + bad) + ' ' + base)
    for a, b in itertools.combinations(spec['values'], 2):
        def flag(k):
            v = spec['values'][k]
            return '-' + k + ((' ' + quote(str(v))) if v is not None else '')
        add('pair:-' + a + ':-' + b, flag(a) + ' ' + flag(b) + ' ' + base)
    return rows


def matrix(root=ROOT, policy=None, stacks=(4096, 8192)):
    policy = validate(root, policy)
    rows = []
    for tool, spec in sorted(policy.items()):
        for case in tool_cases(tool, spec, root):
            for stack in stacks:
                row = dict(case, stack=stack, binary=spec['binary'])
                row['id'] = 'c%06d' % len(rows)
                rows.append(row)
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--output', type=Path)
    ns = ap.parse_args()
    rows = matrix()
    counts = {t: sum(r['tool'] == t for r in rows) for t in sorted({r['tool'] for r in rows})}
    print('cli_inventory=PASS tools=%d cases=%d stacks=4096,8192' % (len(counts), len(rows)))
    if ns.output:
        ns.output.write_text(json.dumps({'cases': rows, 'counts': counts}, indent=2) + '\n')

if __name__ == '__main__':
    main()
