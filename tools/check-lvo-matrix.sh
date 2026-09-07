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
# The comment usually FOLLOWS the entry on the same line, but for
# NetStackQuery/NetStackControl it PRECEDES it on its own line.  A
# follows-only regex silently pairs each of those symbols with the NEXT
# comment, which put api=bsd_NetStackControl against symbol=bsd_NetStackQuery
# and was caught by the evidence cross-check rather than by reading.
rows=[]
_vl=vec.split('\n')
_pend=None
for _l in _vl:
    m=re.search(r'\(APTR\)(bsd_[A-Za-z_0-9]+),\s*/\*\s*(-0x[0-9a-fA-F]+)\s*'
                r'\[\s*(\d+)\]\s*([A-Za-z_0-9]+)', _l)
    if m:
        rows.append((m.group(1),m.group(2),m.group(3),m.group(4))); _pend=None; continue
    m=re.match(r'\s*/\*\s*(-0x[0-9a-fA-F]+)\s*\[\s*(\d+)\]\s*(bsd_[A-Za-z_0-9]+)',_l)
    if m:
        _pend=(m.group(1),m.group(2),m.group(3)); continue
    m=re.match(r'\s*\(APTR\)(bsd_[A-Za-z_0-9]+),\s*$',_l)
    if m and _pend:
        off,idx,api=_pend
        rows.append((m.group(1),off,idx,api)); _pend=None

# option a profile turns off, straight from the arm lines in tools/ci.sh
ci=open('tools/ci.sh').read()
def arm(name):
    m=re.search(r'"'+name+r':([^"]*)"',ci)
    if not m: raise SystemExit(f"lvo_matrix=FAIL reason=no_{name}_arm_in_ci.sh")
    return set(re.findall(r'-DAMINETXDUO_([A-Z_0-9]+)=OFF',m.group(1)))
PROF=[('default',set()),('minimal',arm('minimal')),
      ('micro',arm('micro')),('microcompat',arm('microcompat'))]

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
        # A vector wired to bsd_enosys is not "ok" in any profile -- it has no
        # implementation at all.  The survey found telnetd calling vsyslog 4x,
        # lpd 8x and AmiFTPd 28+ times against a column that read `ok`, which
        # is the matrix inviting exactly the wrong conclusion.
        if sym=='bsd_enosys': return 'enosys'
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
EV=tests/profiles/aminet-evidence.tsv
python3 - "$OUT" "$EV" <<'PY'
import sys
mat,ev=sys.argv[1],sys.argv[2]
hdr=None; rows=[]
for l in open(mat):
    p=l.rstrip('\n').split('\t')
    if hdr is None: hdr=p; continue
    rows.append(p)
prof={n:i for i,n in enumerate(hdr)}
status={}
for r in rows: status[r[2]]=r          # api -> row

bad=0; breaks=[]; cov=None
if not __import__('os').path.exists(ev):
    print("lvo_matrix=FAIL reason=no_evidence_file"); sys.exit(1)
# Coverage is COUNTED from the ledger, not read from a hand-typed header: a
# typed percentage is stale the moment a batch reports.
led='tests/profiles/aminet-surveyed.tsv'
if not __import__('os').path.exists(led):
    print("lvo_matrix=FAIL reason=no_surveyed_ledger"); sys.exit(1)
corpus=None; seen=[]
for l in open(led):
    if l.startswith('# CORPUS'):
        try: corpus=int(l.split('\t')[1])
        except Exception: pass
        continue
    if l.startswith('#') or not l.strip(): continue
    seen.append(l.split('\t')[0])
if corpus is None:
    print("lvo_matrix=FAIL reason=ledger_has_no_corpus_line"); bad+=1; corpus=0
n=len(set(seen))
cov=f"surveyed={n} corpus={corpus} pct={100.0*n/corpus:.1f}" if corpus else "surveyed=%d"%n
for l in open(ev):
    if l.startswith('#') or not l.strip(): continue
    p=l.rstrip('\n').split('\t')
    if len(p)<6: continue
    kind,key,pr,st=p[0],p[1],p[2],p[3]
    # A METHOD row records how the SURVEY can go wrong -- an AS225 dual stack,
    # an ixemul-hosted daemon -- and makes no claim about any profile, so it
    # carries `-` where a profile would go.
    if st=='METHOD' and pr=='-':
        continue
    if pr not in prof:
        print(f"lvo_matrix=FAIL reason=unknown_profile row={key} profile={pr}"); bad+=1; continue
    # "no proven caller" over an unstated sample is not a finding.  Four rows
    # said CLEAN with an empty archives field, which claimed the corpus does
    # not use those vectors on the strength of twelve archives out of 5,372.
    if st=='CLEAN' and p[4].strip() in ('','-'):
        print(f"lvo_matrix=FAIL reason=clean_without_sample key={key}"); bad+=1
    if kind=='lvo':
        r=status.get(key)
        if r is None:
            print(f"lvo_matrix=FAIL reason=evidence_names_unknown_vector api={key}"); bad+=1; continue
        got=r[prof[pr]]
        # evidence claiming a break must agree that the profile stubs it
        if st=='DECODED':
            pass                      # a claim about the method, not about use
        elif st in ('BREAKS','CLEAN') and got!='STUB':
            print(f"lvo_matrix=FAIL reason=evidence_disagrees api={key} "
                  f"profile={pr} evidence={st} matrix={got}"); bad+=1
        if st=='OK' and got=='STUB':
            print(f"lvo_matrix=FAIL reason=evidence_disagrees api={key} "
                  f"profile={pr} evidence=OK matrix=STUB"); bad+=1
        if st=='DEGRADED' and got!='hosts-only':
            print(f"lvo_matrix=FAIL reason=evidence_disagrees api={key} "
                  f"profile={pr} evidence=DEGRADED matrix={got}"); bad+=1
    if st in ('BREAKS','DEGRADED'):
        breaks.append(f"{pr}:{key}={st}")

print(f"lvo_matrix_rows={len(rows)}")
print(f"lvo_matrix_coverage={cov}")
print(f"lvo_matrix_known_impact={len(breaks)} {' '.join(sorted(breaks))}")
print("lvo_matrix=" + ("PASS" if bad==0 else "FAIL"))
sys.exit(1 if bad else 0)
PY
