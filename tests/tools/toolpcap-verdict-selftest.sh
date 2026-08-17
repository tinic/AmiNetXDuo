#!/usr/bin/env bash
#
# Prove src/tools/test/test_toolpcap.c can fail.
#
#   tests/tools/toolpcap-verdict-selftest.sh
#
# A file-format test is the easiest kind to write so that it cannot go red.
# It asserts on bytes it wrote itself, so an assertion that stopped reading the
# right offset still finds what it put there, and the ctest case stays green
# while the file NetCapture writes stops being a pcap.  Nothing else in the
# tree reads that file: the emulator arm does, on a peer, and it runs on one
# machine with an Amiga attached.
#
# Each case below breaks src/tools/toolpcap.c in one named way in a copy of the
# tree, and requires the test to fail.  The break is then undone, and the last
# thing this does is check the unbroken copy still passes -- so a case that
# left the tree damaged is a failure too.
#
# Compiles the test itself: this runs before the host cmake configure, so there
# is no build tree.  Needs cc and python3; about two seconds.
#
# Output is key=value plus an exit code.
#
# SPDX-License-Identifier: MIT

set -uo pipefail
ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

SRC="$T/toolpcap.c"
BIN="$T/test_toolpcap"

cp "$ROOT/src/tools/toolpcap.c" "$SRC"

build_and_run() {
    cc -std=c11 -Wall -Wextra -I"$ROOT/src/tools" -I"$T" \
       "$ROOT/src/tools/test/test_toolpcap.c" "$SRC" -o "$BIN" \
       > "$T/build.log" 2>&1 || return 2
    "$BIN" > "$T/run.log" 2>&1
}

cases=0
wrong=0

# Exact literal replacement, first occurrence, and a hard error when the text
# a case is built on is gone.  sed would need the pattern escaped and would
# silently do nothing once it stopped matching, which is the failure this file
# exists to prevent.
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
        echo "  BROKEN  $what: toolpcap.c no longer contains the text this"\
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
        # A sabotage that does not compile proves nothing about the test.
        echo "  NOBUILD $what: the broken copy does not compile"
        sed -n '1,5p' "$T/build.log"
        wrong=$((wrong + 1))
    fi

    cp "$ROOT/src/tools/toolpcap.c" "$SRC"
}

if ! build_and_run; then
    echo "toolpcap selftest: the unbroken file does not pass"
    sed -n '1,10p' "$T/run.log" "$T/build.log" 2>/dev/null
    exit 1
fi

# --- the file header, which is what a reader believes before anything else --

# The little-endian magic is a VALID pcap header saying every length in the
# file is byte-swapped, so this produces a file that opens and is nonsense.
case_red \
    'tool_pcap_u32(o, TOOL_PCAP_MAGIC);' \
    'tool_pcap_u32(o, 0xd4c3b2a1UL);' \
    'the magic written little-endian'

case_red \
    'tool_pcap_u32(o, TOOL_PCAP_DLT_EN10MB);' \
    'tool_pcap_u32(o, 0);' \
    'the link type written as DLT_NULL rather than Ethernet'

# The two-byte fields, which are only the version numbers -- and which nothing
# else in the file exercises.
case_red \
    'b[0] = (unsigned char)((v >> 8) & 0xFFUL);
    b[1] = (unsigned char)(v & 0xFFUL);

    tool_pcap_raw(o, b, 2);' \
    'b[1] = (unsigned char)((v >> 8) & 0xFFUL);
    b[0] = (unsigned char)(v & 0xFFUL);

    tool_pcap_raw(o, b, 2);' \
    'the 16-bit fields written little-endian'

# --- the record ------------------------------------------------------------

# orig_len is the wire length and incl_len is what was stored.  Writing caplen
# into both is invisible in any capture where nothing was truncated, which is
# every capture taken with a snap length nothing reached.
case_red \
    'tool_pcap_u32(o, datalen);' \
    'tool_pcap_u32(o, caplen);' \
    'the wire length written as the stored length'

# A record longer than the file header promises is where tcpdump stops reading
# and reports a bogus savefile header, losing every frame after it.
case_red \
    'if (caplen > o->snaplen)' \
    'if (caplen > o->snaplen + 100000UL)' \
    'the clamp to the snap length removed'

# --- the counters, which are what the SIZE limit and the summary read -------

case_red \
    'o->filelen += len;' \
    'o->filelen += 0;' \
    'the file length not counted'

# A writer that carries on after a refused write leaves a file with a hole in
# the middle, which reads as a valid pcap and is not -- and spends the rest of
# the capture calling Write() on a full disk.
case_red \
    'if (n == 0 || o->failed || o->sink == 0)' \
    'if (n == 0 || o->sink == 0)' \
    'a failed write no longer stopping the file'

# --- and the copy is undone -------------------------------------------------

if ! build_and_run; then
    echo "  DIRTY   the restored copy does not pass"
    sed -n '1,10p' "$T/run.log"
    wrong=$((wrong + 1))
fi

echo "toolpcap selftest: $cases cases, $wrong wrong"
[ "$wrong" -eq 0 ]
