#!/usr/bin/env bash
#
# Run the DHCP lifecycle / RFC 3927 test under FS-UAE on SLIRP.
#
#   tests/netstack/run-dhcp3927.sh [-m MODEL] [-t SECONDS] [-c CPU]
#                                  [-b BUILDDIR] [-C CONFIGURE] [-M MODE]
#
# -C sets CONFIGURE= in the staged DEVS:NetInterfaces/eth0.  DHCP (the
#    default) is the ordinary case; AUTO asks for a link-local address and
#    nothing else, which is the only way to reach RFC 3927 without first
#    making DHCP fail.
# -M sets ENV:DHCP3927MODE, which the test reads on the Amiga:
#      lease     (default) start normally and drive the lease lifecycle
#      killlink  hold the interface down across netstack_startup()'s DHCP
#                window so the RFC 3927 fallback has to fire, then put the
#                wire back and watch DHCP take the address off AutoIP
#
# WHY THE WIRE IS TAKEN AWAY RATHER THAN THE SERVER
#
#   FS-UAE's SLIRP intercepts UDP port 67 itself (its own bootp.c) and answers
#   from 10.0.2.2.  A DHCP server on the host can therefore never see a
#   DISCOVER, a renewal or anything else, see docs/RESEARCH.md.  SLIRP's
#   lease is 24 hours and it never withdraws one, so "the server stopped
#   answering" is produced by disabling the link instead.
#
# The a2065.device driver is not ours to ship: point AMINETXDUO_A2065 at one,
# or drop a copy in build/a2065.device.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
HERE="$ROOT/tests/netstack"
MODEL=A1200
TIMEOUT=420
CPU=""
BUILD="${AMINETXDUO_BUILD:-build/cm}"
CONFIGURE=DHCP
MODE=lease

while getopts "m:t:c:b:C:M:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        c) CPU="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        C) CONFIGURE="$OPTARG" ;;
        M) MODE="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t secs] [-c cpu] [-b dir] [-C configure] [-M mode]" >&2
           exit 2 ;;
    esac
done

EXE="$ROOT/$BUILD/tests/netstack/dhcp3927_test"
[ -f "$EXE" ] || { echo "build $BUILD/tests/netstack/dhcp3927_test first" >&2; exit 2; }

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

STAGE="$ROOT/build/dhcp3927-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/env"
cp -R "$HERE/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"

# The interface file the test boots on.
cat > "$STAGE/devs/NetInterfaces/eth0" <<EOF
DEVICE=a2065.device
UNIT=0
CONFIGURE=$CONFIGURE
EOF

# fsuae-run.sh backs ENV: with the staged env/ directory, so this reaches
# GetVar() on the Amiga.
printf '%s' "$MODE" > "$STAGE/env/DHCP3927MODE"

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-dhcp3927}"

# ---------------------------------------------------------- the verdict ---
#
# This used to end in `exec <runner>`, so the script's exit status was the
# guest's own return code: a guest that opened nothing, ran no checks and
# returned 0 was a pass, and so was one whose transcript never arrived.
# tools/test-verdict.sh reads the guest's own counters instead, puts a floor
# under the number of checks, and fails loudly and by name when there is no
# transcript at all.
. "$ROOT/tools/test-verdict.sh"

verdict() {
    # 0 pass, 1 fail, 77 the guest skipped: all three are carried out.
    verdict_guest "dhcp3927" 1 "$1" \
        "$(verdict_hd_amiberry)/stdout.txt" \
        "$(verdict_serial_amiberry)" && exit 0
    exit $?
}

CPUARG=()
[ -z "$CPU" ] || CPUARG=(-c "$CPU")

echo "==> CONFIGURE=$CONFIGURE, DHCP3927MODE=$MODE"

set +e
"$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" -t "$TIMEOUT" "${CPUARG[@]}" \
     "$EXE" "$STAGE/devs" "$STAGE/env"
RUN_RC=$?
set -e
verdict "$RUN_RC"
