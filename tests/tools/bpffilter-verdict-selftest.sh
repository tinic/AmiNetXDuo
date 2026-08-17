#!/usr/bin/env bash
#
# Prove src/tools/test/test_bpffilter.c can fail.
#
#   tests/tools/bpffilter-verdict-selftest.sh
#
# A filter test has a failure mode that looks exactly like success: if the
# programs it compiles accept everything, every "this frame must be kept" case
# passes and the capture NetCapture takes is the whole segment.  Half the
# assertions in that test exist for the other direction, and this is what
# proves they fire.
#
# Each case below breaks src/tools/bpffilter.c in one named way in a copy, and
# requires the test to fail.  Every one of them is a real mistake in this kind
# of code: an offset off by four, a compare that stops after the first word, a
# port read at a fixed offset instead of through the index register.  None of
# them stops a capture happening, and none of them is visible in the file.
#
# Compiles the test itself, against the library's own interpreter and
# validator: this runs before the host cmake configure, so there is no build
# tree.  Needs cc and python3; about five seconds.
#
# Output is key=value plus an exit code.
#
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

# case_red <old> <new> <what it is>
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

# --- offsets ---------------------------------------------------------------

# Source and destination four bytes apart in an IPv4 header.  A filter reading
# the destination twice matches every frame TO the host and none FROM it, which
# in a capture of a request and its reply is half the conversation.
case_red \
    '#define IP4_SRC_OFF         (ETH_HDR_LEN + 12)' \
    '#define IP4_SRC_OFF         (ETH_HDR_LEN + 16)' \
    'the IPv4 source address read from the destination offset'

# The transport starts after a variable-length header.  Reading the ports at a
# fixed 34 is right for every frame without IP options and wrong for the rest,
# and options are what a capture is opened to look at.
case_red \
    'LDH_IND(ETH_HDR_LEN + 0);' \
    'LDH_ABS(ETH_HDR_LEN + 20);' \
    'the IPv4 source port read at a fixed offset'

# --- comparisons -----------------------------------------------------------

# Sixteen bytes compared as four words.  A compare that only ever looks at the
# first matches every address in the same /32 -- which, on a link where every
# global address shares a prefix, is every machine on it.
case_red \
    'JEQ(tool_bpf_v6_word(host, i), (i == 3) ? ok : LBL_NEXT, dst);' \
    'JEQ(tool_bpf_v6_word(host, 0), (i == 3) ? ok : LBL_NEXT, dst);' \
    'the IPv6 source compared on its first word four times'

# A fragment after the first has no transport header, so the bytes at the port
# offsets are payload and a filter that reads them matches on data.
case_red \
    'JSET(IP4_FRAG_MASK, reject, LBL_NEXT);' \
    'JSET(0, reject, LBL_NEXT);' \
    'the fragment-offset check made unconditional'

# ICMPv6 is protocol 58 and ICMP is 1.  Asking for ICMP and capturing no IPv6
# at all reads as a link with no ICMPv6 on it.
case_red \
    'tool_bpf_gate_proto(b, reject, IP6_NEXT_OFF, IPPROTO_ICMPV6_,' \
    'tool_bpf_gate_proto(b, reject, IP6_NEXT_OFF, IPPROTO_ICMP_,' \
    'ICMPv6 matched on the IPv4 protocol number'

# ARP for something that is not IPv4 has no address at the offsets a HOST
# filter reads, so it matches on whatever is there.
case_red \
    'JEQ(ETHERTYPE_IP, LBL_NEXT, reject);' \
    'JEQ(ETHERTYPE_IP, LBL_NEXT, LBL_NEXT);' \
    'the ARP protocol-type gate never rejecting'

# --- the verdict itself ----------------------------------------------------

case_red \
    'RET(f->invert ? 0UL : f->snaplen);' \
    'RET(f->snaplen);' \
    'NOT no longer inverting'

# A combination that can never match compiles into a filter that captures
# nothing, and an empty pcap looks like a quiet network.
case_red \
    '(f->proto == TOOL_BPF_PROTO_ICMP || f->proto == TOOL_BPF_PROTO_ARP))' \
    '(f->proto == TOOL_BPF_PROTO_ICMP && f->proto == TOOL_BPF_PROTO_ARP))' \
    'a port on a protocol that has none no longer refused'

# --- and the copy is undone -------------------------------------------------

if ! build_and_run; then
    echo "  DIRTY   the restored copy does not pass"
    sed -n '1,10p' "$T/run.log"
    wrong=$((wrong + 1))
fi

echo "bpffilter selftest: $cases cases, $wrong wrong"
[ "$wrong" -eq 0 ]
