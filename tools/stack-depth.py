#!/usr/bin/env python3
"""Worst-case stack depth over the call graph of an LTO'd m68k binary.

Reads the `*.ltrans*.s` assembly GCC keeps with `-save-temps=obj` and the
`*.ltrans*.su` frame sizes `-fstack-usage` writes beside it, joins the two, and
reports the deepest path from a named root.  tools/check-stack-frames.sh drives
it; run it by hand to see WHERE a budget went.

  jsr/bsr <sym>   a call:      own frame + the deepest callee subtree
  jra/jmp <sym>   a tail call:  max(own frame, callee subtree), frame is gone
  lea/pea <sym>   the address is taken; treated as a possible indirect call
                  unless --no-indirect

Symbols are canonicalised before the join.  m68k-amigaos prefixes every C
symbol with one underscore, so the assembly's `__nxe_dhcp_x` is the `.su`
file's `_nxe_dhcp_x`; and GCC's clone suffixes -- .part, .constprop, .isra,
.lto_priv, .cold -- appear in the assembly and not in the `.su`, so they are
stripped.  Getting either wrong silently reports a function as a leaf.

WHAT IT CANNOT SEE.  A call through a function pointer held in a struct is not
in the assembly as a symbol.  --edge A=B supplies one by hand; the resolver
ladder needs four.  An unknown symbol contributes zero rather than failing, so
a number from this tool is a floor, not a ceiling -- which is the right way
round for a budget that is checked against measurement in the emulator.

usage: stack-depth.py [--no-indirect] [--edge A=B]... [--cut SYM]...
                      [--quiet] <dir> <root>...

SPDX-License-Identifier: MIT
"""
import os
import re
import sys
from collections import defaultdict

CALL = re.compile(r'^\s*(?:jsr|bsr\.?[wsbl]?)\s+\(?([A-Za-z_.$][\w.$]*)\)?\s*$')
TAIL = re.compile(r'^\s*(?:jra|jmp|bra\.?[wsbl]?)\s+\(?([A-Za-z_.$][\w.$]*)\)?\s*$')
TAKEN = re.compile(r'^\s*(?:lea|pea|move\.l)\s+#?(_[A-Za-z_][\w.$]*)[,\s]')
LABEL = re.compile(r'^(_?[A-Za-z_.$][\w.$]*):')
CLONE = re.compile(r'\.(part|constprop|isra|cold|localalias|lto_priv)(\.\d+)*$')
LOCAL = re.compile(r'^\.?L[\dA-Z]')


def declone(name):
    prev = None
    while prev != name:
        prev = name
        name = CLONE.sub('', name)
    return name


def canon(label):
    return declone(label[1:] if label.startswith('_') else label)


def load(where, indirect):
    frames = defaultdict(int)
    calls = defaultdict(set)
    tails = defaultdict(set)
    seen = 0

    for root, _dirs, files in os.walk(where):
        for name in files:
            path = os.path.join(root, name)
            if name.endswith('.su'):
                seen += 1
                for line in open(path, errors='replace'):
                    parts = line.rstrip('\n').split('\t')
                    if len(parts) < 2:
                        continue
                    where_bits = parts[0].split(':')
                    if len(where_bits) < 4:
                        continue
                    try:
                        size = int(parts[1])
                    except ValueError:
                        continue
                    fn = declone(':'.join(where_bits[3:]))
                    frames[fn] = max(frames[fn], size)
            elif name.endswith('.s'):
                seen += 1
                cur = None
                for line in open(path, errors='replace'):
                    hit = LABEL.match(line)
                    if hit:
                        if not LOCAL.match(hit.group(1)):
                            cur = canon(hit.group(1))
                        continue
                    if cur is None:
                        continue
                    hit = CALL.match(line)
                    if hit:
                        calls[cur].add(canon(hit.group(1)))
                        continue
                    hit = TAIL.match(line)
                    if hit:
                        if not LOCAL.match(hit.group(1)):
                            tails[cur].add(canon(hit.group(1)))
                        continue
                    if indirect:
                        hit = TAKEN.match(line)
                        if hit:
                            calls[cur].add(canon(hit.group(1)))
    return frames, calls, tails, seen


def main():
    argv = sys.argv[1:]
    indirect, quiet = True, False
    edges, cuts = [], set()

    while argv and argv[0].startswith('--'):
        if argv[0] == '--no-indirect':
            indirect, argv = False, argv[1:]
        elif argv[0] == '--quiet':
            quiet, argv = True, argv[1:]
        elif argv[0] == '--edge':
            edges.append(argv[1]); argv = argv[2:]
        elif argv[0] == '--cut':
            cuts.add(argv[1]); argv = argv[2:]
        else:
            sys.stderr.write('unknown option %s\n' % argv[0])
            return 2

    if len(argv) < 2:
        sys.stderr.write(__doc__)
        return 2

    where, roots = argv[0], argv[1:]
    frames, calls, tails, seen = load(where, indirect)
    if seen == 0:
        sys.stderr.write('no .su or .s files under %s\n' % where)
        return 2

    for edge in edges:
        caller, callee = edge.split('=', 1)
        calls[caller].add(callee)
    for sym in cuts:
        calls.pop(sym, None)
        tails.pop(sym, None)
        frames[sym] = 0

    memo, onstack = {}, set()

    def depth(sym):
        if sym in memo:
            return memo[sym]
        if sym in onstack:
            return (0, ['*** RECURSION at %s' % sym])
        if sym not in frames:
            return (0, [])
        own = frames[sym]
        onstack.add(sym)
        best = (0, [])
        for callee in calls.get(sym, ()):
            sub = depth(callee)
            if sub[0] > best[0]:
                best = sub
        answer = (own + best[0], ['%-56s %5d' % (sym, own)] + best[1])
        for callee in tails.get(sym, ()):
            sub = depth(callee)
            if sub[0] > answer[0]:
                answer = sub
        onstack.discard(sym)
        memo[sym] = answer
        return answer

    for root in roots:
        memo.clear()
        onstack.clear()
        total, path = depth(root)
        print('root=%s worst_case_bytes=%d' % (root, total))
        if not quiet:
            for step in path:
                print('    %s' % step)
            print()
    return 0


if __name__ == '__main__':
    sys.exit(main())
