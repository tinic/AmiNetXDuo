#!/usr/bin/env bash
#
# What each build profile answers for every bsdsocket.library vector.
#
#   tools/check-lvo-matrix.sh            compare against the checked-in table
#   tools/check-lvo-matrix.sh --write    regenerate it
#
# tests/profiles/lvo-matrix.tsv is the answer to "does this program still work
# on the minimal drawer, or on the floor build".  An LVO an option replaces
# with an ENOSYS stub is a call that used to do something and now does not, and
# there is no other place in the tree that says which those are.
#
# NOTHING HERE IS HARDCODED, which is the point.  The vector list comes from
# bsdsocket_vectors.c, the option that gates a vector is read from the
# `#else /* !AMINETXDUO_X */` in the file that defines it, and which options a
# profile turns off is read from the arm lines in tools/ci.sh.  Add an option
# and the table moves on its own; a stale table is then a diff, not a surprise.
#
# POSITION, NOT FILE.  roadshow.c is split, but In_LocalAddr() and
# In_CanForward() are below its #endif and are built whatever AMINETXDUO_DNS
# says.  A file-level mapping would call them stubbed and be wrong about two
# vectors out of eight.
#
# SPDX-License-Identifier: MIT

set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
OUT=tests/profiles/lvo-matrix.tsv
MODE="${1:-check}"

gen() {
python3 - <<'PY'
import re,os,subprocess

vec=open('src/bsdsocket/bsdsocket_vectors.c').read()
rows=re.findall(r'\(APTR\)(bsd_[A-Za-z_0-9]+),\s*/\*\s*(-0x[0-9a-fA-F]+)\s*\[\s*(\d+)\]\s*([A-Za-z_0-9]+)', vec)

# option a profile turns off, straight from the arm lines in tools/ci.sh
ci=open('tools/ci.sh').read()
def arm(name):
    m=re.search(r'"'+name+r':([^"]*)"',ci)
    if not m: raise SystemExit(f"lvo_matrix=FAIL reason=no_{name}_arm_in_ci.sh")
    return set(re.findall(r'-DAMINETXDUO_([A-Z_0-9]+)=OFF',m.group(1)))
PROF=[('default',set()),('minimal',arm('minimal')),('micro',arm('micro'))]

# where each symbol is defined, and whether that line sits inside a guard
srcs={}
for d,_,fs in os.walk('src'):
    for fn in fs:
        if fn.endswith('.c'): srcs[os.path.join(d,fn)]=open(os.path.join(d,fn)).read().split('\n')

def locate(sym):
    pat=re.compile(r'^[A-Za-z_].*\b'+re.escape(sym)+r'\s*\(')
    for path,lines in srcs.items():
        for i,l in enumerate(lines):
            if pat.match(l): return path,lines,i
    return None,None,None

# A vector whose file resolves through netstack_resolve*() is not stubbed when
# AMINETXDUO_DNS is off -- netstack_dns_off.c still answers it, out of
# DEVS:Internet/hosts and nothing else.  That is a different thing from ENOSYS
# and an application survey has to be able to tell them apart.  Derived from
# the call, not from a list of names.
# Scoped to THIS function's body -- from its definition line to the closing
# brace in column one -- because a file-wide search flags every vector in
# interfaces.c and roadshow.c on the strength of one unrelated call elsewhere
# in the same file.
def resolves(sym):
    path,lines,i=locate(sym)
    if path is None: return False
    def body_of(start):
        out=[]
        for l in lines[start:]:
            out.append(l)
            if l.startswith('}'): break
        return '\n'.join(out)

    b=body_of(i)
    if 'netstack_resolve' in b: return True
    # One level of delegation: bsd_gethostbyname() calls bsd_resolve_name(),
    # which is the function that resolves.  Follow same-file callees once --
    # not further, because two levels starts matching the whole file again.
    for callee in set(re.findall(r'\b(bsd_[A-Za-z_0-9]+)\s*\(', b)):
        if callee==sym: continue
        for j,l in enumerate(lines):
            if re.match(r'^[A-Za-z_].*\b'+re.escape(callee)+r'\s*\(',l) \
               or re.match(r'^static\s.*\b'+re.escape(callee)+r'\s*\(',l):
                if 'netstack_resolve' in body_of(j): return True
                break
    return False

def gate_of(sym):
    path,lines,i=locate(sym)
    if path is None: return '?','?'
    opt=None;a=b=c=None
    for j,l in enumerate(lines):
        m=re.match(r'\s*#else\s*/\*\s*!(AMINETXDUO_([A-Z_0-9]+))',l)
        if m: opt=m.group(2); b=j; break
    if opt is None: return os.path.basename(path),'-'
    for j in range(b,-1,-1):
        if re.match(r'\s*#ifdef\s+AMINETXDUO_'+opt+r'\s*$',lines[j]): a=j; break
    for j in range(b,len(lines)):
        if re.match(r'\s*#endif\s*/\*\s*AMINETXDUO_'+opt,lines[j]): c=j; break
    if a is None or c is None: return os.path.basename(path),'-'
    # inside the ON branch -> gated; anywhere else in the file -> always built
    return os.path.basename(path), (opt if a<i<b else '-')

print("offset\tidx\tapi\tsymbol\tfile\tgate\t"+"\t".join(n for n,_ in PROF))
for sym,off,idx,api in rows:
    f,g=gate_of(sym)
    res=resolves(sym)
    def status(off):
        if g!='-' and g in off: return 'STUB'
        if res and 'DNS' in off:  return 'hosts-only'
        return 'ok'
    st=[status(off) for _,off in PROF]
    print(f"{off}\t{idx}\t{api}\t{sym}\t{f}\t{g}\t"+"\t".join(st))
PY
}

if [ "$MODE" = "--write" ]; then
    gen > "$OUT"
    echo "lvo_matrix=WROTE rows=$(( $(wc -l < "$OUT") - 1 )) file=$OUT"
    exit 0
fi

[ -f "$OUT" ] || { echo "lvo_matrix=FAIL reason=missing file=$OUT"; exit 1; }
tmp=$(mktemp); trap 'rm -f "$tmp"' EXIT
gen > "$tmp"
if ! diff -q "$OUT" "$tmp" > /dev/null; then
    echo "lvo_matrix=FAIL reason=stale file=$OUT"
    diff "$OUT" "$tmp" | head -20
    echo "regenerate with: tools/check-lvo-matrix.sh --write"
    exit 1
fi
awk -F'\t' 'NR>1{for(i=7;i<=NF;i++) c[i"\t"$i]++} END{
  printf "lvo_matrix_rows=%d\n", NR-1
  printf "lvo_matrix=PASS\n"}' "$OUT"
