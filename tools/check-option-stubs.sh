#!/usr/bin/env bash
#
# An option's OFF side must define every function its ON side does.
#
#   tools/check-option-stubs.sh
#
# A file split `#ifdef AMINETXDUO_X ... #else ... #endif` has two branches that
# have to present the SAME set of externally visible functions, because the
# vector table takes their addresses and other translation units call them.
# Leave one out and the build links fine until the arm that turns the option
# off is built, which is the arm nobody runs locally.
#
# THIS EXISTS BECAUSE IT HAPPENED TWICE IN ONE DAY.  raw.c's off branch was
# written from a grep over the file rather than from the header, and
# bsd_raw_receive() -- the one entry point returning NX_PACKET * rather than a
# plain type -- was missed; three arms configured, built and failed at the link.
# netstack_dns_off.c lost ami_ns6_rdnss, ami_ns6_dnssl and two
# netstack_dns_server6_* the same way.  Both were minutes of build time to find
# and one second to check.
#
# The parse is deliberately dumb: a top-level definition is a line starting in
# column one with a type and a name and an open paren, not ending in a
# semicolon.  That is the whole style of this tree, and a false positive here
# is a mismatch that gets looked at, which is the right way for it to fail.
#
# SPDX-License-Identifier: MIT

set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

python3 - <<'PY'
import re,sys,os,subprocess

# -F: the pattern contains /* and an unescaped * is a quantifier, which is how
# the first version of this gate matched nothing and still reported PASS.
files=subprocess.run(['grep','-rlF','#else /* !AMINETXDUO_','src'],
                     capture_output=True,text=True).stdout.split()
if not files:
    print('option_stubs=FAIL reason=no_split_files_found'); sys.exit(1)
DEF=re.compile(r'^(?:[A-Za-z_][A-Za-z_0-9]*\s+)+\*?\s*([A-Za-z_][A-Za-z_0-9]*)\s*\(')
bad=0; checked=0
for f in sorted(files):
    src=open(f).read().split('\n')
    # find the split: #ifdef AMINETXDUO_X / #else /* !AMINETXDUO_X / #endif
    opt=None; a=b=c=None
    for i,l in enumerate(src):
        m=re.match(r'\s*#else\s*/\*\s*!(AMINETXDUO_[A-Z_0-9]+)',l)
        if m: opt=m.group(1); b=i; break
    if opt is None: continue
    for i in range(b,-1,-1):
        if re.match(r'\s*#ifdef\s+'+opt+r'\s*$',src[i]): a=i; break
    for i in range(b,len(src)):
        if re.match(r'\s*#endif\s*/\*\s*'+opt,src[i]): c=i; break
    if a is None or c is None:
        print(f"option_stubs=MALFORMED file={f} opt={opt}"); bad+=1; continue

    def defs(lo,hi):
        out=set(); depth=0
        for l in src[lo+1:hi]:
            if l.startswith('#if'): depth+=1
            elif l.startswith('#endif') and depth: depth-=1
            # typedef LONG (*Fn)(...) backtracks into capturing the return
            # type, so it is excluded with static rather than parsed.
            if (l.rstrip().endswith(';') or l.startswith('static')
                    or l.startswith('typedef')): continue
            m=DEF.match(l)
            if m and m.group(1) not in ('if','for','while','switch','return','sizeof'):
                out.add(m.group(1))
        return out

    on,off=defs(a,b),defs(b,c)
    checked+=1
    missing=sorted(on-off); extra=sorted(off-on)
    if missing:
        print(f"option_stubs=MISSING file={f} opt={opt} not_stubbed={','.join(missing)}"); bad+=1
    if extra:
        print(f"option_stubs=EXTRA file={f} opt={opt} only_in_off={','.join(extra)}"); bad+=1

print(f"option_stubs_files={checked}")
print(f"option_stubs_errors={bad}")
print("option_stubs=" + ("PASS" if bad==0 else "FAIL"))
sys.exit(1 if bad else 0)
PY
