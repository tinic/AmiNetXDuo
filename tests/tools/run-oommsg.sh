#!/usr/bin/env bash
#
# THE REGRESSION TEST FOR "the machine is out of memory and the message blames
# the cable".
#
#   tests/tools/run-oommsg.sh [-t SECONDS] [-b BUILDDIR]
#
# WHAT IT PROVES
#
#   A 512 KB machine cannot start the stack: the packet pool, the NX_IP
#   instance and the thread stacks do not fit, bsdsocket.library's first
#   OpenLibrary() fails, and AddNetInterface has to say why.  Its failure text
#   walks a decision tree that used to end at "The card is fine, so what failed
#   was getting an address ... Check the cable", which is what someone with
#   73 KB free was told to go and look at.  docs/RESEARCH.md 81.3 found it.
#
#   The out-of-memory branch that replaced it cannot be reached by a unit test:
#   it reads AvailMem() on a real machine after a real failed open.  So this
#   boots the real machine and reads the real words, and asserts all three
#   things that were wrong:
#
#     * the text says out of memory
#     * it quotes a free-byte figure, and the figure is under the threshold
#       that fired, a plausible number, not a constant someone typed
#     * it does NOT mention the cable
#
#   AddNetInterface must also still FAIL, exit code and all: a diagnosis that
#   arrives with a success status is worse than no diagnosis.
#
# WHY THIS NEEDS -a
#
#   AddNetInterface takes an INTERFACE argument, and until tools/amiberry-run.sh
#   grew -a there was no way to give an executable under test one.  That is why
#   this branch shipped in v0.14.3 having never run.  The whole class of
#   commands that take a parameter was untestable; this is the first test to
#   use the flag.
#
# THE MACHINE
#
#   A2000, 68000, Kickstart 2.04, 512 KB of Chip RAM and nothing else, no
#   Fast RAM and no Slow RAM, which the stock quickstart profile would
#   otherwise add half a megabyte of and take the machine back over the floor.
#   docs/RESEARCH.md 81 established this configuration refuses cleanly, so the
#   failure under test is known-reachable rather than hoped for.
#
#   THE BUILD MUST BE A 68000 ONE.  A 68020 binary stops this machine with an
#   illegal instruction before a line of ours runs, and the failure then looks
#   like the thing being tested:
#
#     cmake -S . -B build/m68000 -DAMINETXDUO_CPU=68000 -DCMAKE_BUILD_TYPE=Release
#     tests/tools/run-oommsg.sh -b build/m68000
#
#   Kickstart 2.04 comes from AMINETXDUO_KICKSTART_A2000; the 3.1 ROM that
#   ~/amiga-assets/env.sh exports by default is not it.  a2065.device is not
#   ours to ship: AMINETXDUO_A2065, or a copy in build/a2065.device.  The card
#   is staged even though nothing gets as far as using it, because the branch
#   under test has to be chosen IN PREFERENCE to the device probe, a run with
#   no driver would take a different path and prove nothing.
#
# Amiberry only.  FS-UAE cannot be driven headless on the Linux box that has
# the ROMs (`FATAL: [GLAD] Failed to initialize OpenGL context`).
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

TIMEOUT=240
BUILD="${AMINETXDUO_BUILD:-build/cm}"

while getopts "t:b:" opt; do
    case "$opt" in
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        *) echo "usage: $0 [-t seconds] [-b builddir]" >&2; exit 2 ;;
    esac
done

ADDIF="$ROOT/$BUILD/src/tools/AddNetInterface"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
for f in "$ADDIF" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
done

KICK="${AMINETXDUO_KICKSTART_A2000:-}"
[ -n "$KICK" ] && [ -f "$KICK" ] || {
    echo "Kickstart 2.04 needed.  Set AMINETXDUO_KICKSTART_A2000=<rom>." >&2
    exit 2
}

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for candidate in \
        "$ROOT/build/a2065.device" \
        "$HOME/amiga-os-src/os-source/other_networking/sana2/bin/devs/a2065.device"
    do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "No a2065.device found. Set AMINETXDUO_A2065=<path>." >&2
    exit 2
}

# ------------------------------------------------------------- staging ---

STAGE="$ROOT/build/oommsg-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs" "$STAGE/devs/NetInterfaces"
cp "$BSD"    "$STAGE/libs/bsdsocket.library"
cp "$A2065"  "$STAGE/devs/a2065.device"
cat > "$STAGE/devs/NetInterfaces/eth0" <<'EOF'
DEVICE=a2065.device
UNIT=0
CONFIGURE=DHCP
EOF

# ------------------------------------------------------------------ run ---

# 512 KB and nothing else.  fastmem_size and chipmem_size are already in the
# config the harness writes, so these lines override them; bogomem_size is not,
# and quickstart=A2000 supplies 512 KB of it unasked.  Appended rather than
# assigned, so a caller sweeping other settings keeps them.
MEM="fastmem_size=0;chipmem_size=1;bogomem_size=0"
export AMINETXDUO_AMIBERRY_EXTRA="${AMINETXDUO_AMIBERRY_EXTRA:+$AMINETXDUO_AMIBERRY_EXTRA;}$MEM"
export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-oommsg}"
export AMINETXDUO_A2065="$A2065"
HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"

echo "==> booting a 512 KB A2000 on Kickstart 2.04"
set +e
"$ROOT/tools/amiberry-run.sh" -m A2000 -c 68000 -N a2065 -t "$TIMEOUT" \
    -a eth0 "$ADDIF" "$STAGE/devs" "$STAGE/libs"
RUN_RC=$?
set -e

REPORT="$HD/stdout.txt"
[ -f "$REPORT" ] || { echo "FAIL: the guest wrote no $REPORT (run rc=$RUN_RC)" >&2; exit 1; }

echo
echo "================= what AddNetInterface printed ====================="
cat "$REPORT"
echo "===================================================================="
echo

# ------------------------------------------------------------ assertions ---

FAILED=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }
pass() { echo "  ok: $*"; }

expect() {
    local what="$1"; shift
    if grep -qiF -- "$*" "$REPORT"; then
        pass "$what"
    else
        fail "$what, nothing printed \"$*\""
    fi
}

reject() {
    local what="$1"; shift
    if grep -qiF -- "$*" "$REPORT"; then
        fail "$what, \"$*\" was printed and should not have been"
        grep -niF -- "$*" "$REPORT" | sed 's/^/       /' >&2
    else
        pass "$what"
    fi
}

# The argument arrived.  Without it AddNetInterface prints its template and
# stops, and every assertion below would fail for the wrong reason.
expect "the INTERFACE argument reached the guest" "eth0: a2065.device unit 0"

expect "it reports the start as failed"    "the network would not start"
expect "and names memory as the reason"    "not enough free memory"

# The figure has to come from AvailMem() on this machine, not from a constant.
# Under ADDNETIF_MIN_FREE, which is what the branch tested, and not zero.
FREE=$(sed -n 's/^ *\([0-9][0-9]*\) bytes are free.*/\1/p' "$REPORT" | head -1)
if [ -z "$FREE" ]; then
    fail "no free-byte figure was printed"
elif [ "$FREE" -gt 0 ] && [ "$FREE" -lt 204800 ]; then
    pass "it quotes $FREE bytes free, under the 200K the branch tested"
else
    fail "the free figure '$FREE' is not a plausible reading on this machine"
fi

# The thing this test exists for.
reject "it does not send anyone to look at the cable" "cable"
reject "and does not claim the card is fine"          "The card is fine"

# A diagnosis with a success status is worse than none.
RC=$(cat "$HD/.done" 2>/dev/null || echo "")
if [ "$RC" = "20" ]; then
    pass "AddNetInterface exited 20"
else
    fail "AddNetInterface exited '$RC', not 20, a failure reported as success"
fi

# The stack's own refusal, on the serial port, is what the text is explaining.
SERIAL="$ROOT/build/amiberry-serial-$AMINETXDUO_RUN_TAG.log"
if grep -qi "netstack_startup failed" "$SERIAL" 2>/dev/null; then
    pass "the stack refused for the reason the text describes"
else
    fail "the serial log has no netstack_startup failure, the run failed elsewhere"
    [ -f "$SERIAL" ] && tail -20 "$SERIAL" | sed 's/^/       /' >&2
fi

echo
if [ "$FAILED" -ne 0 ]; then
    echo "oommsg: FAILED" >&2
    exit 1
fi

echo "oommsg: PASSED"
exit 0
