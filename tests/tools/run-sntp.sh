#!/usr/bin/env bash
#
# sntp, and what setting the clock does to certificate checking.
#
#   tests/tools/run-sntp.sh [-m MODEL] [-t SECONDS] [-c CPU] [-b BUILDDIR]
#                           [-s TIMESERVER] [-P BASEPORT]
#
# WHAT THIS PROVES, AND WHY IT IS NOT A CLOCK TEST
#
#   tls.library skips a certificate's notBefore/notAfter when the machine's
#   clock is outside a plausible window (src/tlslib/tls_time.c).  That is the
#   right call on an Amiga with a dead battery, the alternative is a machine
#   that cannot reach any HTTPS site, but it means an EXPIRED certificate is
#   accepted.  `sntp` puts the clock right, and the check comes back.
#
#   So the run is a before and an after, against the same expired certificate:
#
#     ClockSet 0        the machine everybody has: no clock, 1978
#     fetch expired     ACCEPTED, and fetch says "validity dates NOT checked"
#     sntp <server>     the clock is set from a real time server
#     fetch expired     REFUSED: "the certificate is expired or not yet valid"
#
#   with a certificate that is perfectly good (rsa2.test) fetched on both sides
#   of it, so that the refusal is demonstrably about the expiry date and not
#   about the clock having broken TLS in general.
#
# WHAT IS ON THE OTHER END
#
#   tests/peer/httppeer.py with tests/peer/mkpki.sh's local PKI, which already
#   contains an expired leaf (notAfter 2021-01-01) chained to a root the guest
#   trusts.  The guest reaches it at 10.0.2.2, which is what SLIRP calls the
#   host, and resolves the names out of DEVS:Internet/hosts.
#
#   THE TIME SERVER IS A REAL ONE ON THE INTERNET, and that is not laziness.
#   SLIRP is a NAT and forwards the guest's outbound UDP perfectly well, so a
#   real server is reachable and was measured to be so.  A local one is not
#   possible: SNTP is UDP port 123, ports below 1024 need root on both macOS
#   and Linux, and this suite does not ask for root.  A `PORT` argument on
#   `sntp` would have made it hermetic and would have been a knob that exists
#   only for the test, which is the wrong trade.
#
#   Everything else in the run IS hermetic; if the time server is unreachable
#   the script says so and stops before starting the emulator, rather than
#   producing a failure that looks like a bug in sntp.
#
# The a2065.device driver is not ours to ship: point AMINETXDUO_A2065 at one,
# or drop a copy in build/a2065.device.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
MODEL=A1200
TIMEOUT=420
CPU=""
BUILD="${AMINETXDUO_BUILD:-build/cm}"
TIMESERVER="${AMINETXDUO_TIMESERVER:-time.apple.com}"
BASE_PORT=7200

while getopts "m:t:c:b:s:P:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        c) CPU="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        s) TIMESERVER="$OPTARG" ;;
        P) BASE_PORT="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t secs] [-c cpu] [-b builddir]" \
                "[-s timeserver] [-P baseport]" >&2
           exit 2 ;;
    esac
done

SMOKE="$ROOT/$BUILD/src/tools/ToolsSmoke"
ADDIF="$ROOT/$BUILD/src/tools/AddNetInterface"
SNTP="$ROOT/$BUILD/src/tools/sntp"
FETCH="$ROOT/$BUILD/src/tools/fetch"
NETSTAT="$ROOT/$BUILD/src/tools/netstat"
CLOCKSET="$ROOT/$BUILD/tests/tools/ClockSet"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
TLSLIB="$ROOT/$BUILD/src/tlslib/tls.library"

for f in "$SMOKE" "$ADDIF" "$SNTP" "$FETCH" "$NETSTAT" "$CLOCKSET" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
done
[ -f "$TLSLIB" ] || {
    echo "missing $TLSLIB, this test needs an AMINETXDUO_TLS=ON build" >&2
    exit 2
}

# ------------------------------------------------------- the time server ---
#
# Asked here, from the host, before anything else is set up.  If the answer is
# "no route to the internet" then the run would fail in a way that looks like a
# bug in sntp, and finding that out after four minutes of emulator is worse
# than finding it out now.

echo "==> checking that $TIMESERVER answers on this host"

cat > "$ROOT/build/sntp-probe.py" <<'EOF'
import socket, struct, sys
host = sys.argv[1]
ip = socket.gethostbyname(host)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(5)
s.sendto(b'\x1b' + 47 * b'\0', (ip, 123))
d, _ = s.recvfrom(96)
if len(d) < 48 or (d[0] & 7) != 4:
    sys.exit(1)
secs = struct.unpack('!I', d[40:44])[0]
print("    %s (%s) stratum %d, NTP seconds %d" % (host, ip, d[1], secs))
EOF

if ! python3 "$ROOT/build/sntp-probe.py" "$TIMESERVER"; then
    echo "!! no SNTP answer from that server.  This test needs one, see the" >&2
    echo "   comment at the top of the script for why it cannot be local." >&2
    exit 2
fi

# ------------------------------------------------------------- a2065 -----

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

# ----------------------------------------------------------------- PKI ---

PKI="$ROOT/build/peer-pki"
"$ROOT/tests/peer/mkpki.sh" "$PKI" >/dev/null
[ -f "$PKI/teststore" ] || { echo "no trust store in $PKI" >&2; exit 2; }

# ------------------------------------------------------------- staging ---

STAGE="$ROOT/build/sntp-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065"    "$STAGE/devs/a2065.device"
cp "$BSD"      "$STAGE/libs/bsdsocket.library"
cp "$TLSLIB"   "$STAGE/libs/tls.library"
cp "$PKI/teststore" "$STAGE/devs/Internet/certificates"
cp "$ADDIF"    "$STAGE/AddNetInterface"
cp "$SNTP"     "$STAGE/sntp"
cp "$FETCH"    "$STAGE/fetch"
cp "$NETSTAT"  "$STAGE/netstat"
cp "$CLOCKSET" "$STAGE/ClockSet"

# fetch has no --resolve, so the names the certificates carry are pointed at
# SLIRP's host address the way an Amiga has always done it.
cat >> "$STAGE/devs/Internet/hosts" <<EOF
10.0.2.2 expired.test
10.0.2.2 rsa2.test
EOF

GOOD=$((BASE_PORT + 1))
EXPIRED=$((BASE_PORT + 6))

cat > "$STAGE/commands.txt" <<EOF
# ---- the template, through ReadArgs' own "?" --------------------------
SYS:sntp ?
# ---- a machine whose battery died: AmigaOS starts at 1978 -------------
SYS:ClockSet 0
SYS:AddNetInterface eth0
# ---- what a shipped stack-reading command sees of a stack that lives
#      inside bsdsocket.library ---------------------------------------
SYS:netstat -r
# ---- BEFORE: the clock is unset, so tls.library skips the dates -------
SYS:fetch https://rsa2.test:$GOOD/bytes/16 TO DH0:ctl1.bin >DH0:a-ctl-before.txt
SYS:fetch https://expired.test:$EXPIRED/bytes/16 TO DH0:exp1.bin >DH0:b-exp-before.txt
# ---- a "time server" that is not one: this must fail legibly ----------
SYS:sntp 10.0.2.2 TIMEOUT 5 >DH0:g-sntp-nobody.txt
# ---- ask the time, and then set it ------------------------------------
SYS:sntp $TIMESERVER SHOW >DH0:c-sntp-show.txt
SYS:sntp $TIMESERVER >DH0:d-sntp-set.txt
# ---- AFTER: the same two certificates, with a clock -------------------
SYS:fetch https://rsa2.test:$GOOD/bytes/16 TO DH0:ctl2.bin >DH0:e-ctl-after.txt
SYS:fetch https://expired.test:$EXPIRED/bytes/16 TO DH0:exp2.bin >DH0:f-exp-after.txt
EOF

# --------------------------------------------------------------- peer ---

PEERLOG="$ROOT/build/sntp-peer.log"

# The peer's own lifetime has to cover the WAIT for the emulator as well as the
# run, and build/.fsuae.lock queues runs behind each other for as long as it
# takes.  Sizing it from TIMEOUT alone let the peer expire while this run was
# still in the queue, and the whole thing then failed as four refused
# connections.  The trap below is what actually ends it; the number is only a
# backstop for a script that dies without running it.
python3 "$ROOT/tests/peer/httppeer.py" --base-port "$BASE_PORT" --pki "$PKI" \
    --advertise 10.0.2.2 --log "$PEERLOG" --seconds 7200 \
    > "$ROOT/build/sntp-peer.out" 2>&1 &
PEER_PID=$!

cleanup_peer() { kill -TERM "$PEER_PID" 2>/dev/null || true; }
trap cleanup_peer EXIT INT TERM HUP

sleep 1
kill -0 "$PEER_PID" 2>/dev/null || {
    echo "httppeer.py did not start:" >&2
    cat "$ROOT/build/sntp-peer.out" >&2
    exit 2
}
echo "==> httppeer.py: good cert on $GOOD, expired cert on $EXPIRED"

# ---------------------------------------------------------------- run ---

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-sntp}"

CPUARG=()
[ -z "$CPU" ] || CPUARG=(-c "$CPU")

set +e
"$ROOT/tools/emu-net-run.sh" -n -m "$MODEL" -t "$TIMEOUT" "${CPUARG[@]}" \
    "$SMOKE" "$STAGE/devs" "$STAGE/libs" "$STAGE/AddNetInterface" \
    "$STAGE/sntp" "$STAGE/fetch" "$STAGE/netstat" "$STAGE/ClockSet" \
    "$STAGE/commands.txt"
RC=$?
set -e

HD="$ROOT/build/testhd-${AMINETXDUO_RUN_TAG}"

echo
echo "================ what the host server saw ================"
cat "$PEERLOG" 2>/dev/null || true

# ------------------------------------------------------------ verdict ---

echo
echo "================ the point of the run ===================="

fail=0
say() { printf '    %-5s %s\n' "$1" "$2"; }

check_has()
{
    # $1 file  $2 text  $3 what it means
    if grep -qF "$2" "$HD/$1" 2>/dev/null; then
        say "ok" "$3"
    else
        say "FAIL" "$3"
        fail=1
    fi
}

check_lacks()
{
    if grep -qF "$2" "$HD/$1" 2>/dev/null; then
        say "FAIL" "$3"
        fail=1
    else
        say "ok" "$3"
    fi
}

EXPIRY_MSG="the certificate is expired or not yet valid"

check_has  b-exp-before.txt "validity dates NOT checked" \
    "before: the expired certificate was accepted, dates NOT checked"
check_lacks b-exp-before.txt "$EXPIRY_MSG" \
    "before: nothing was refused for being expired"
check_has  d-sntp-set.txt   "Clock set to" \
    "sntp set the clock from $TIMESERVER"
check_has  f-exp-after.txt  "$EXPIRY_MSG" \
    "after:  the same expired certificate was REFUSED"
check_has  e-ctl-after.txt  "validity dates checked" \
    "after:  a valid certificate still verifies, dates now checked"

echo
if [ "$fail" -ne 0 ]; then
    echo "!! the before/after did not hold, read the files above"
    exit 1
fi

exit "$RC"
