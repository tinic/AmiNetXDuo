#!/bin/sh
#
# What the vectors with no observed caller cost, in bytes.
#
#   tools/aminet-survey/vector-cost.sh [build-dir]
#
# THIS EXISTS BECAUSE THE FIRST TWO MEASUREMENTS SILENTLY DID NOTHING.
#
#   `nm -S` emits no size column for this object format -- three fields,
#   address/type/name -- so the first pass computed 0 bytes for every symbol
#   and read as "these vectors are free".  Sizes here come from the gap to the
#   next text symbol instead.
#
#   `tools/ci.sh` does not honour AMINETXDUO_EXTRA_CMAKE.  Asking it for a
#   non-LTO arm returned a byte-identical LTO library, and `nolto` also strips
#   symbols, so the comparison table came back empty.  Hence the direct cmake
#   configure below rather than a ci.sh arm.
#
# AND WHY NON-LTO IS THE ONE TO QUOTE: under LTO a symbol absorbs its inlined
# neighbours and the gap after it is charged to it.  FreeRouteInfo measured
# 6,710 bytes with LTO and does not appear in the non-LTO top 14 at all.  The
# LTO total was 29,634 against a real 14,290 -- inflated 107%, and it reversed
# the conclusion: the unused half is TWO THIRDS of the used half, not larger.
#
# Quote the aggregate.  Per-vector figures move by hundreds either way.
#
# SPDX-License-Identifier: MIT

set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT" || exit 2
BUILD="${1:-build/vector-cost}"
DATA=docs/aminet-survey

for f in "$DATA/lvo-usage.tsv" "$DATA/lvomap.tsv"; do
    [ -r "$f" ] || { echo "vector_cost=FAIL missing $f"; exit 2; }
done

if [ ! -f "$BUILD/src/bsdsocket/bsdsocket.library" ]; then
    cmake -S . -B "$BUILD" \
        -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-m68k-amigaos.cmake \
        -DCMAKE_BUILD_TYPE=Release \
        -DAMINETXDUO_LTO=OFF -DAMINETXDUO_KEEP_SYMBOLS=ON \
        -DAMINETXDUO_TESTS=OFF > /dev/null 2>&1 || {
            echo "vector_cost=FAIL configure"; exit 2; }
    cmake --build "$BUILD" --parallel 8 --target bsdsocket_library \
        > /dev/null 2>&1 || { echo "vector_cost=FAIL build"; exit 2; }
fi

LIB="$BUILD/src/bsdsocket/bsdsocket.library"
NM="${AMIGA_NM:-/opt/amiga/bin/m68k-amigaos-nm}"
command -v "$NM" > /dev/null 2>&1 || { echo "vector_cost=SKIPPED no_nm"; exit 0; }

"$NM" "$LIB" 2>/dev/null > "$BUILD/syms.txt"
[ -s "$BUILD/syms.txt" ] || { echo "vector_cost=FAIL no symbols in $LIB"; exit 2; }

python3 - "$DATA" "$BUILD/syms.txt" <<'PY'
import sys
data, symfile = sys.argv[1], sys.argv[2]
zero, used = set(), set()
for l in open(f'{data}/lvo-usage.tsv'):
    p = l.rstrip('\n').split('\t')
    if p[0] == 'offset' or p[1] == 'reserved':
        continue
    (zero if p[2] == '0' else used).add(p[1])
impl = {}
for l in open(f'{data}/lvomap.tsv'):
    p = l.rstrip('\n').split('\t')
    if p[0] != 'offset':
        impl[p[3]] = p[4]

txt = []
for l in open(symfile):
    f = l.split()
    if len(f) == 3 and f[1] in 'Tt':
        txt.append((int(f[0], 16), f[2].lstrip('_').split('.lto_priv')[0]))
txt.sort()
size = {}
for i, (a, n) in enumerate(txt):
    nxt = txt[i + 1][0] if i + 1 < len(txt) else a
    size[n] = max(size.get(n, 0), max(0, nxt - a))

def total(names):
    seen, t = set(), 0
    for v in names:
        s = impl.get(v)
        if s and s not in seen:
            seen.add(s)
            t += size.get(s, 0)
    return t, len(seen)

zt, zc = total(zero)
ut, uc = total(used)
print(f"vector_cost zero_caller_impls={zc} zero_caller_bytes={zt}"
      f" used_impls={uc} used_bytes={ut}")
print("vector_cost=PASS")
PY
