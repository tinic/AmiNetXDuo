#!/usr/bin/env bash
# The whole-vector-surface leak drill.
# The a2065.device driver is not ours to ship: point AMINETXDUO_A2065 at one,
# or drop a copy in build/a2065.device.  Every other board's driver comes out
# of AMINETXDUO_SANA2_STORE or ~/amiga-assets/devs.
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

. "$ROOT/tools/test-verdict.sh"

MODEL=A1200
TIMEOUT=0
BUILD="${AMINETXDUO_BUILD:-build/cm}"
BOARD="${AMINETXDUO_AMIBERRY_BOARD:-a2065}"
IFACE="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"
NEGATIVE=0

while getopts "m:t:b:N:B:n" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        n) NEGATIVE=1 ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-b builddir] [-N board] [-B backend] [-n]" >&2; exit 2 ;;
    esac
done

case "$IFACE" in
    slirp|slirp_inbound|none)
        echo "apidrill_backend=refused:$IFACE" >&2
        echo "This harness is bridged only.  -B names a host interface." >&2
        exit 2
        ;;
esac

if [ "$TIMEOUT" = 0 ]; then
    if [ "$NEGATIVE" = 1 ]; then
        TIMEOUT=120
    else
        TIMEOUT=180
    fi
fi

MINCOVER="${AMINETXDUO_APIDRILL_MINCOVER:-145}"

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
DRILL="$ROOT/$BUILD/tests/tools/ApiDrill"

for f in "$TOOLS/ToolsSmoke" "$TOOLS/NetShutdown" "$DRILL" "$BSD"; do
    [ -f "$f" ] || {
        echo "INFRA: missing $f, build the tree first" >&2
        exit 2
    }
done

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for candidate in \
        "$ROOT/build/a2065.device" \
        "$HOME/amiga-assets/devs/a2065.device"
    do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "INFRA: no a2065.device found. Set AMINETXDUO_A2065=<path>." >&2
    exit 2
}

STAGE="$ROOT/build/apidrill-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"

cat > "$STAGE/devs/NetInterfaces/eth0" <<'IFEOF'
DEVICE=a2065.device
UNIT=0
CONFIGURE=STATIC
ADDRESS=10.0.2.15
NETMASK=255.255.255.0
GATEWAY=10.0.2.2
IFEOF

. "$ROOT/tools/sana2-stage.sh"
if [ -z "${AMINETXDUO_SANA2_DRIVER:-}" ] && [ "$BOARD" != a2065 ]; then
    _want=$(sana2_driver_for "$BOARD")
    _have=$(sana2_local_driver "$_want")
    [ -n "$_have" ] && [ -f "$_have" ] &&
        export AMINETXDUO_SANA2_DRIVER="$_have"
fi
sana2_stage "$BOARD" "$STAGE/devs"

cat > "$STAGE/devs/Internet/name_resolution" <<'NREOF'
# ApiDrill stages this file with no nameserver line on purpose: every resolver
# failure path in the drill then answers out of DEVS:Internet/hosts and returns
# at once, instead of timing a DNS round trip 64 times over.
NREOF

cp "$BSD"                   "$STAGE/libs/bsdsocket.library"
cp "$TOOLS/NetShutdown"     "$STAGE/NetShutdown"
cp "$DRILL"                 "$STAGE/ApiDrill"

DRILLARGS=""
[ -n "${AMINETXDUO_APIDRILL_ITERS:-}" ] && \
    DRILLARGS="ITERS $AMINETXDUO_APIDRILL_ITERS"
[ -n "${AMINETXDUO_APIDRILL_ONLY:-}" ] && \
    DRILLARGS="$DRILLARGS ONLY $AMINETXDUO_APIDRILL_ONLY"
[ "$NEGATIVE" = 1 ] && DRILLARGS="$DRILLARGS BROKEN"

cat > "$STAGE/commands.txt" <<EOF
SYS:ApiDrill $DRILLARGS
SYS:NetShutdown
EOF

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-apidrill}"

if [ "$NEGATIVE" = 1 ]; then
    echo "==> NEGATIVE CONTROL: this run must be REJECTED below"
fi

set +e
echo "==> booting $MODEL under Amiberry, $BOARD on $IFACE, ceiling ${TIMEOUT}s"
"$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
    -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/ApiDrill" "$STAGE/NetShutdown"
RUN_RC=$?
set -e

HD="$(verdict_hd_amiberry)"
REPORT="$HD/tools.txt"

if [ ! -s "$REPORT" ]; then
    echo "INFRA: the guest wrote no $REPORT (emulator rc=$RUN_RC)." >&2
    if [ "$RUN_RC" = "124" ]; then
        echo "       The run hit its ceiling.  That is a defect in what was" >&2
        echo "       run, not a number to raise: the last '> N name vK' line" >&2
        echo "       in the transcript names the vector that hung." >&2
    fi
    exit 2
fi

FAILED=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }
pass() { echo "  ok: $*"; }

STARTS=$(grep -c "^===== SYS:ApiDrill" "$REPORT" || true)
if [ "$STARTS" -eq 1 ]; then
    pass "the machine booted once (no reset)"
elif [ "$STARTS" -gt 1 ]; then
    fail "THE MACHINE REBOOTED: the command list restarted"
else
    fail "the run never reached ApiDrill"
fi

NOISE_BAD=0
for row in '!noise64' '!noise16'; do
    LINE=$(grep -E "^V -[0-9]+ $row " "$REPORT" | tail -1 || true)
    if [ -z "$LINE" ]; then
        fail "the drill printed no $row control row, so nothing established
       the noise floor and no other row's byte count can be read"
        NOISE_BAD=1
        continue
    fi
    echo "   , $LINE"
    B=$(printf '%s' "$LINE" | sed -E 's/.* bytes=(-?[0-9]+) .*/\1/')
    if [ "$B" = 0 ]; then
        pass "$row reads 0 bytes per call"
    else
        fail "$row reads $B bytes per call: this machine cannot be measured on"
        NOISE_BAD=1
    fi
done

set +e
if [ "$NEGATIVE" = 1 ]; then MINCHECKS=1; else MINCHECKS=2000; fi
verdict_guest apidrill "$MINCHECKS" "$RUN_RC" "$REPORT"
VRC=$?
set -e
[ "$VRC" -eq 0 ] || FAILED=1

COVER=$(grep -E "^covered: [0-9]+ of [0-9]+ vectors" "$REPORT" | tail -1 || true)
if [ -z "$COVER" ]; then
    fail "the drill did not report its coverage"
else
    echo "   , $COVER"
    C_DONE=$(printf '%s' "$COVER" | sed -E 's/^covered: ([0-9]+) .*/\1/')
    C_ALL=$(printf  '%s' "$COVER" | sed -E 's/^covered: [0-9]+ of ([0-9]+) .*/\1/')
    C_UNC=$(printf  '%s' "$COVER" | sed -E 's/.*, ([0-9]+) uncovered.*/\1/')
    C_VAR=$(printf  '%s' "$COVER" | sed -E 's/.*, ([0-9]+) variants run/\1/')

    [ "$C_ALL" -eq 150 ] \
        && pass "the table has all 150 LVOs in it" \
        || fail "the table has $C_ALL rows, the library has 150"

    if [ "$NEGATIVE" != 1 ]; then
        [ "$C_DONE" -ge "$MINCOVER" ] \
            && pass "$C_DONE of $C_ALL vectors called ($C_UNC uncovered)" \
            || fail "only $C_DONE of $C_ALL vectors were called, wanted $MINCOVER"

        [ "$C_VAR" -ge "$((MINCOVER + 60))" ] \
            && pass "$C_VAR variants run" \
            || fail "only $C_VAR variants run: rows are being skipped at run time"
    fi

    echo
    echo "   vectors NOT called:"
    grep -E "^V [0-9]+ .*(UNCOVERED|NOTAPPLICABLE)" "$REPORT" \
        | sed 's/^/     /' || echo "     (none)"
    echo
fi

echo "   rows with a non-zero reading:"
awk '/^V [0-9-]+ / {
        z = 1
        for (i = 1; i <= NF; i++) {
            split($i, kv, "=")
            if (kv[1] == "bytes" || kv[1] == "alloc" || kv[1] == "sock" ||
                kv[1] == "sig" || kv[1] == "port" || kv[1] == "sem" ||
                kv[1] == "task") {
                if (kv[2] + 0 != 0) z = 0
            }
        }
        if (!z) print "     " $0
     }' "$REPORT" || true
echo

if [ "$NEGATIVE" = 1 ]; then
    BROKEN_ROWS=$(grep -cE "^V -1[0-9][0-9] !broken-" "$REPORT" || true)
    BROKEN_RED=$(grep -cE "^= -1[0-9][0-9] !broken-[a-z]+ LEAK" "$REPORT" || true)

    echo "   broken rows run: $BROKEN_ROWS, red: $BROKEN_RED"
    if [ "$BROKEN_ROWS" -lt 6 ]; then
        echo "apidrill: NEGATIVE CONTROL FAILED, only $BROKEN_ROWS broken rows ran" >&2
        exit 1
    fi
    if [ "$BROKEN_RED" -lt 6 ]; then
        echo "apidrill: NEGATIVE CONTROL FAILED, only $BROKEN_RED of $BROKEN_ROWS" >&2
        echo "          deliberately broken rows were caught; the gates are not" >&2
        echo "          gating and a green normal run proves nothing" >&2
        exit 1
    fi
    if [ "$FAILED" -ne 0 ]; then
        echo "apidrill: negative control PASSED, all $BROKEN_RED broken rows were caught and the run was rejected"
        exit 0
    fi
    echo "apidrill: NEGATIVE CONTROL FAILED, a run of deliberately broken rows was accepted" >&2
    exit 1
fi

if [ "$NOISE_BAD" -ne 0 ]; then
    echo "apidrill: INFRASTRUCTURE FAILURE, the noise floor is not zero" >&2
    exit 2
fi

if [ "$FAILED" -ne 0 ]; then
    echo "apidrill: FAILED" >&2
    exit 1
fi

echo "apidrill: PASSED"
exit 0
