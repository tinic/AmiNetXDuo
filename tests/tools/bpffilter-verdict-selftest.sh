#!/usr/bin/env bash
# Prove src/tools/test/test_bpffilter.c can fail.
# Compiles the test itself, against the library's own interpreter and
# validator: this runs before the host cmake configure, so there is no build
# tree.  Needs cc and python3; about five seconds.
# SPDX-License-Identifier: MIT

set -uo pipefail
ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

SRC="$T/bpffilter.c"
BIN="$T/test_bpffilter"

cp "$ROOT/src/tools/bpffilter.c" "$SRC"

build_and_run() {
    cc -std=c11 -Wall -Wextra -DAMI_BPF_REPLICA \
       -I"$ROOT/include" -I"$ROOT/src/bpf" -I"$ROOT/src/config/test/shim" \
       -I"$ROOT/src/tools" \
       "$ROOT/src/tools/test/test_bpffilter.c" "$SRC" \
       "$ROOT/src/bpf/bpf_filter.c" "$ROOT/src/bpf/bpf_validate.c" \
       -o "$BIN" > "$T/build.log" 2>&1 || return 2
    "$BIN" > "$T/run.log" 2>&1
}

cases=0
wrong=0

replace() {
    python3 - "$SRC" "$1" "$2" <<'PY'
import io, sys
path, old, new = sys.argv[1], sys.argv[2], sys.argv[3]
s = io.open(path, encoding='utf-8', errors='surrogateescape').read()
if old not in s:
    sys.exit(1)
io.open(path, 'w', encoding='utf-8', errors='surrogateescape').write(
    s.replace(old, new, 1))
PY
}

case_red() {
    local old=$1 new=$2 what=$3 rc

    cases=$((cases + 1))

    if ! replace "$old" "$new"; then
        echo "  BROKEN  $what: bpffilter.c no longer contains the text this"\
             "case edits"
        wrong=$((wrong + 1))
        return
    fi

    build_and_run
    rc=$?

    if [ "$rc" = 0 ]; then
        echo "  ESCAPED $what: the test stayed green"
        wrong=$((wrong + 1))
    elif [ "$rc" = 2 ]; then
        echo "  NOBUILD $what: the broken copy does not compile"
        sed -n '1,5p' "$T/build.log"
        wrong=$((wrong + 1))
    fi

    cp "$ROOT/src/tools/bpffilter.c" "$SRC"
}

if ! build_and_run; then
    echo "bpffilter selftest: the unbroken file does not pass"
    sed -n '1,10p' "$T/run.log" "$T/build.log" 2>/dev/null
    exit 1
fi

case_red \
    '#define IP4_SRC_OFF         (ETH_HDR_LEN + 12)' \
    '#define IP4_SRC_OFF         (ETH_HDR_LEN + 16)' \
    'the IPv4 source address read from the destination offset'

case_red \
    'LDH_IND(ETH_HDR_LEN + 0);' \
    'LDH_ABS(ETH_HDR_LEN + 20);' \
    'the IPv4 source port read at a fixed offset'

case_red \
    'JEQ(tool_bpf_v6_word(host, i), (i == 3) ? ok : LBL_NEXT, dst);' \
    'JEQ(tool_bpf_v6_word(host, 0), (i == 3) ? ok : LBL_NEXT, dst);' \
    'the IPv6 source compared on its first word four times'

case_red \
    'JSET(IP4_FRAG_MASK, reject, LBL_NEXT);' \
    'JSET(0, reject, LBL_NEXT);' \
    'the fragment-offset check made unconditional'

case_red \
    'tool_bpf_gate_proto(b, reject, IP6_NEXT_OFF, IPPROTO_ICMPV6_,' \
    'tool_bpf_gate_proto(b, reject, IP6_NEXT_OFF, IPPROTO_ICMP_,' \
    'ICMPv6 matched on the IPv4 protocol number'

case_red \
    'JEQ(ETHERTYPE_IP, LBL_NEXT, reject);' \
    'JEQ(ETHERTYPE_IP, LBL_NEXT, LBL_NEXT);' \
    'the ARP protocol-type gate never rejecting'

case_red \
    'RET(f->invert ? 0UL : f->snaplen);' \
    'RET(f->snaplen);' \
    'NOT no longer inverting'

case_red \
    '(f->proto == TOOL_BPF_PROTO_ICMP || f->proto == TOOL_BPF_PROTO_ARP))' \
    '(f->proto == TOOL_BPF_PROTO_ICMP && f->proto == TOOL_BPF_PROTO_ARP))' \
    'a port on a protocol that has none no longer refused'

if ! build_and_run; then
    echo "  DIRTY   the restored copy does not pass"
    sed -n '1,10p' "$T/run.log"
    wrong=$((wrong + 1))
fi

echo "bpffilter selftest: $cases cases, $wrong wrong"
[ "$wrong" -eq 0 ]
