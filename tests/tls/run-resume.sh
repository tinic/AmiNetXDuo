#!/usr/bin/env bash
#
# TLS session resumption, measured on the A1200 profile.
#
#   tests/tls/run-resume.sh [-m MODEL] [-t SECONDS] [-c CPU] [-k MHZ]
#                           [-b BUILDDIR] [-s STAGE] [-p PHASE]
#
# THREE PHASES, and each answers a different question.
#
#   -p api    tests/tls/tls_resume, in ONE process: cold handshake and resumed
#             handshake against the same host, back to back, with the same
#             instrumentation.  That delta is the deliverable.  Also proves a
#             resumed session carries data, that a ticket the server refuses
#             falls back to a full handshake rather than failing, and that
#             TLSA_NoResume means what it says.
#
#   -p cross  the shipped `fetch` command, run three times in one boot.  Three
#             PROCESSES: if the second and third handshakes are fast, the cache
#             outlived the program that filled it, which is the case a user
#             actually meets when they run curl twice.
#
#   -p boot   two FS-UAE runs with the machine rebooted in between, carrying
#             DEVS:Internet/tlssessions across.  Proves the disk mirror, and is
#             the only way to seed a host whose COLD handshake a 14 MHz 68020
#             cannot finish, the seeding run uses -k to give the arithmetic
#             enough clock, and the resuming run is the A1200's own 14 MHz.
#             That is the www.iana.org headline.
#
#   -p all    (default) api, then cross, then boot.
#
# NOT A BASELINE.  It depends on the internet, on FS-UAE's SLIRP NAT, on third
# parties' servers, and on those servers' willingness to issue and honour a
# session ticket.  See tests/tls/run-api.sh for the same disclaimer at length.
#
# The a2065.device driver is not ours to ship: point AMINETXDUO_A2065 at one,
# or drop a copy in build/a2065.device.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
MODEL=A1200
TIMEOUT=600
CPU=""
CLOCK=""
BUILD="${AMINETXDUO_BUILD:-build/tls}"
PHASE=all

# The clock the SEEDING run of the boot phase uses.  www.iana.org is three
# certificates behind Cloudflare, and a cold verification at 14 MHz takes
# longer than that front end will wait, so the cache has to be filled at a
# clock where it completes.  The number that matters is the RESUMED one, and
# that is measured at the A1200's own 14 MHz.
SEED_CLOCK="${AMINETXDUO_RESUME_SEED_CLOCK:-28}"

while getopts "m:t:c:k:b:p:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        c) CPU="$OPTARG" ;;
        k) CLOCK="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        p) PHASE="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-c cpu] [-k MHz] [-b builddir] [-p api|cross|boot|all]" >&2; exit 2 ;;
    esac
done

RESUME="$ROOT/$BUILD/tests/tls/tls_resume"
SMOKE="$ROOT/$BUILD/src/tools/ToolsSmoke"
FETCH="$ROOT/$BUILD/src/tools/fetch"
ADDIF="$ROOT/$BUILD/src/tools/AddNetInterface"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
TLS="$ROOT/$BUILD/src/tlslib/tls.library"
STORE="$ROOT/$BUILD/certificates"

for f in "$RESUME" "$SMOKE" "$FETCH" "$ADDIF" "$BSD" "$TLS" "$STORE"; do
    [ -f "$f" ] || {
        echo "missing $f, build tls_resume ToolsSmoke fetch AddNetInterface bsdsocket_library tls_library first" >&2
        exit 2
    }
done

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

# ------------------------------------------------------------- staging ---

STAGE="$ROOT/build/resume-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065"  "$STAGE/devs/a2065.device"
cp "$BSD"    "$STAGE/libs/bsdsocket.library"
cp "$TLS"    "$STAGE/libs/tls.library"
cp "$STORE"  "$STAGE/devs/Internet/certificates"
cp "$FETCH"  "$STAGE/fetch"
cp "$ADDIF"  "$STAGE/AddNetInterface"

# A SECOND trust store: valid, well-formed, and holding one root that signed
# nothing on the public internet.  It is the acceptance test for the defect
# where a session verified against one store was resumed by a caller
# presenting another, the correct answer is the same refusal a cold
# handshake gives, and the only way to test that is to have a store that is
# real enough to be opened and wrong enough to be useless.
OTHER="$ROOT/build/resume-otherstore.pem"
if [ ! -f "$OTHER" ]; then
    openssl req -x509 -newkey rsa:2048 -keyout /dev/null -out "$OTHER" \
        -days 3650 -nodes -subj "/CN=AmiNetXDuo Resumption Test Root" \
        >/dev/null 2>&1 || {
        echo "openssl could not generate the decoy root" >&2; exit 2; }
fi
python3 "$ROOT/tools/mkcertstore.py" --output "$STAGE/devs/Internet/otherstore" \
    --min-roots 1 "$OTHER" >/dev/null
echo "==> decoy trust store: $(wc -c < "$STAGE/devs/Internet/otherstore" | tr -d ' ') bytes, 1 root"

CPUARG=()
[ -z "$CPU" ]   || CPUARG+=(-c "$CPU")
[ -z "$CLOCK" ] || CPUARG+=(-k "$CLOCK")

# fsuae-run.sh wipes build/testhd<tag> at the start of every run, so anything
# the Amiga wrote to DEVS: is gone by the next one.  Carrying the session cache
# forward is therefore an explicit copy, and it is also exactly what makes the
# boot phase a proof rather than an assertion: the only thing that crosses is
# that one file.
carry_sessions() {
    local tag="$1"
    local src="$ROOT/build/testhd-$tag/devs/Internet/tlssessions"

    if [ -f "$src" ]; then
        cp "$src" "$STAGE/devs/Internet/tlssessions"
        echo "==> carried $(wc -c < "$src" | tr -d ' ') bytes of session cache forward"
        return 0
    fi

    echo "==> no session cache was written by the $tag run"
    return 1
}

run_api() {
    echo
    echo "======================================================== phase: api"
    AMINETXDUO_RUN_TAG=resume-api \
    "$ROOT/tools/fsuae-run.sh" -n -m "$MODEL" -t "$TIMEOUT" "${CPUARG[@]}" \
        "$RESUME" "$STAGE/devs" "$STAGE/libs"
}

run_cross() {
    echo
    echo "====================================================== phase: cross"
    echo "Three fetch invocations, three processes, one boot."

    cat > "$STAGE/commands.txt" <<'EOF'
SYS:AddNetInterface eth0
SYS:fetch https://tls-v1-2.badssl.com/ TO DH0:cross1.txt
SYS:fetch https://tls-v1-2.badssl.com/ TO DH0:cross2.txt
SYS:fetch https://ecc256.badssl.com/ TO DH0:cross3.txt
SYS:fetch https://ecc256.badssl.com/ TO DH0:cross4.txt
EOF

    AMINETXDUO_RUN_TAG=resume-cross \
    "$ROOT/tools/fsuae-run.sh" -n -m "$MODEL" -t "$TIMEOUT" "${CPUARG[@]}" \
        "$SMOKE" "$STAGE/devs" "$STAGE/libs" "$STAGE/fetch" \
        "$STAGE/AddNetInterface" "$STAGE/commands.txt"
}

run_boot() {
    echo
    echo "======================================================= phase: boot"
    echo "Boot 1 seeds the cache at ${SEED_CLOCK} MHz; boot 2 resumes at the A1200's 14 MHz."

    rm -f "$STAGE/devs/Internet/tlssessions"

    cat > "$STAGE/commands.txt" <<'EOF'
SYS:AddNetInterface eth0
SYS:fetch https://www.iana.org/ TO DH0:seed.txt
EOF

    AMINETXDUO_RUN_TAG=resume-seed \
    "$ROOT/tools/fsuae-run.sh" -n -m "$MODEL" -t "$TIMEOUT" -k "$SEED_CLOCK" \
        "$SMOKE" "$STAGE/devs" "$STAGE/libs" "$STAGE/fetch" \
        "$STAGE/AddNetInterface" "$STAGE/commands.txt" || true

    carry_sessions resume-seed || return 1

    cat > "$STAGE/commands.txt" <<'EOF'
SYS:AddNetInterface eth0
SYS:fetch https://www.iana.org/ TO DH0:warm.txt
EOF

    AMINETXDUO_RUN_TAG=resume-warm \
    "$ROOT/tools/fsuae-run.sh" -n -m "$MODEL" -t "$TIMEOUT" "${CPUARG[@]}" \
        "$SMOKE" "$STAGE/devs" "$STAGE/libs" "$STAGE/fetch" \
        "$STAGE/AddNetInterface" "$STAGE/commands.txt"
}

case "$PHASE" in
    api)   run_api ;;
    cross) run_cross ;;
    boot)  run_boot ;;
    all)   run_api; run_cross; run_boot ;;
    *)     echo "unknown phase: $PHASE" >&2; exit 2 ;;
esac
