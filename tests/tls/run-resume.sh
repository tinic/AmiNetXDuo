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
# ---------------------------------------------------------- the verdict ---
#
# Every phase used to end with the emulator's exit status and nothing else.
# The deliverable in the header -- that a resumed handshake happens and is
# cheaper than a cold one -- was never asserted anywhere, so a build in which
# resumption silently stopped working passed all three phases: `fetch` still
# fetches, tls_resume still returns 0 for the checks it does run, and a boot
# phase whose seeding run died was already carrying an empty cache forward.
. "$ROOT/tools/test-verdict.sh"

FAILED=0
fail() { echo "FAIL: $*" >&2; FAILED=$((FAILED + 1)); }
pass() { echo "  ok: $*"; }

hd_of() { echo "$ROOT/build/amiberry-testhd-$1"; }

# `fetch` prints " (resumed session)" on the handshake line when the session
# came out of the cache, and nothing there when it did not (src/tools/fetch.c).
# That string is the only direct evidence a resume happened.
resumes_in() { grep -c "(resumed session)" "$1" 2>/dev/null || true; }

carry_sessions() {
    local tag="$1"
    local src bytes
    src="$(hd_of "$tag")/devs/Internet/tlssessions"

    if [ ! -f "$src" ]; then
        fail "the $tag run wrote no session cache, so nothing can be resumed"
        return 1
    fi
    bytes=$(wc -c < "$src" | tr -d ' ')
    # A zero-byte file is not a cache. It used to be carried forward and the
    # warm run then did a second cold handshake, which nothing noticed.
    if [ "$bytes" -eq 0 ]; then
        fail "the $tag run wrote a 0-byte session cache, which holds no session"
        return 1
    fi
    cp "$src" "$STAGE/devs/Internet/tlssessions"
    pass "carried $bytes bytes of session cache forward"
    return 0
}

run_api() {
    echo
    echo "======================================================== phase: api"
    set +e
    AMINETXDUO_RUN_TAG=resume-api \
    "$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" -t "$TIMEOUT" "${CPUARG[@]}" \
        "$RESUME" "$STAGE/devs" "$STAGE/libs"
    local rc=$?
    set -e

    local out="$ROOT/build/amiberry-testhd-resume-api/stdout.txt"
    AMINETXDUO_RUN_TAG=resume-api verdict_guest "resume-api" 8 "$rc" \
        "$out" "$ROOT/build/amiberry-serial-resume-api.log" || FAILED=$((FAILED + 1))

    # THE DELIVERABLE (see the header): a resumed handshake, and one that costs
    # less than the cold one it followed.  Without this the phase passes on a
    # library that quietly does two full handshakes.
    if ! grep -q "DELTA " "$out" 2>/dev/null; then
        fail "no DELTA line: tls_resume never completed a cold/resumed pair"
        return
    fi
    local seen=0 slower=0
    while read -r host cold warm; do
        seen=$((seen + 1))
        if [ "$warm" -lt "$cold" ]; then
            pass "$host resumed in $warm ms against a cold $cold ms"
        else
            slower=$((slower + 1))
            fail "$host resumed in $warm ms, no cheaper than the cold $cold ms"
        fi
    done < <(sed -n 's/.*DELTA \([^:]*\): cold \([0-9]*\) ms -> resumed \([0-9]*\) ms.*/\1 \2 \3/p' "$out")
    [ "$seen" -gt 0 ] || fail "a DELTA line was printed but none could be read"
    [ "$slower" -eq 0 ] || true
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

    set +e
    AMINETXDUO_RUN_TAG=resume-cross \
    "$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" -t "$TIMEOUT" "${CPUARG[@]}" \
        "$SMOKE" "$STAGE/devs" "$STAGE/libs" "$STAGE/fetch" \
        "$STAGE/AddNetInterface" "$STAGE/commands.txt"
    local rc=$?
    set -e

    local rep; rep="$(hd_of resume-cross)/tools.txt"
    if [ ! -s "$rep" ]; then
        fail "the cross phase wrote no transcript at $rep (emulator rc=$rc)"
        return
    fi
    cat "$rep"

    # Four fetches, two hosts: the second call to each host is a different
    # PROCESS and must still find the session the first one left behind.  Two
    # resumes is the whole point of the phase.
    local n; n=$(resumes_in "$rep")
    if [ "$n" -ge 2 ]; then
        pass "$n resumed handshakes across processes"
    else
        fail "only $n resumed handshakes: the cache did not outlive the process"
    fi
    [ "$rc" = "0" ] || fail "the cross phase's emulator exited $rc"
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

    # The seeding run's status used to be thrown away with `|| true`, so a boot
    # that never reached the network still went on to "carry" a cache that was
    # not there and the phase reported whatever the second run felt like.
    set +e
    AMINETXDUO_RUN_TAG=resume-seed \
    "$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" -t "$TIMEOUT" -k "$SEED_CLOCK" \
        "$SMOKE" "$STAGE/devs" "$STAGE/libs" "$STAGE/fetch" \
        "$STAGE/AddNetInterface" "$STAGE/commands.txt"
    local seed_rc=$?
    set -e
    if [ "$seed_rc" != "0" ]; then
        fail "the seeding boot exited $seed_rc, so there is no cache to resume from"
        return
    fi
    pass "the seeding boot completed"

    carry_sessions resume-seed || return

    cat > "$STAGE/commands.txt" <<'EOF'
SYS:AddNetInterface eth0
SYS:fetch https://www.iana.org/ TO DH0:warm.txt
EOF

    set +e
    AMINETXDUO_RUN_TAG=resume-warm \
    "$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" -t "$TIMEOUT" "${CPUARG[@]}" \
        "$SMOKE" "$STAGE/devs" "$STAGE/libs" "$STAGE/fetch" \
        "$STAGE/AddNetInterface" "$STAGE/commands.txt"
    local warm_rc=$?
    set -e

    local rep; rep="$(hd_of resume-warm)/tools.txt"
    if [ ! -s "$rep" ]; then
        fail "the warm boot wrote no transcript at $rep (emulator rc=$warm_rc)"
        return
    fi
    cat "$rep"

    # The claim the phase exists for: the cache survived a power cycle, so the
    # handshake on the second boot is a resume and not a second cold one.
    local n; n=$(resumes_in "$rep")
    if [ "$n" -ge 1 ]; then
        pass "the handshake after the reboot was resumed from the carried cache"
    else
        fail "the warm boot did a COLD handshake: the disk mirror did not work"
    fi
    [ "$warm_rc" = "0" ] || fail "the warm boot's emulator exited $warm_rc"
}

case "$PHASE" in
    api)   run_api ;;
    cross) run_cross ;;
    boot)  run_boot ;;
    all)   run_api; run_cross; run_boot ;;
    *)     echo "unknown phase: $PHASE" >&2; exit 2 ;;
esac

echo
if [ "$FAILED" -ne 0 ]; then
    echo "resume: FAILED ($FAILED)" >&2
    exit 1
fi
echo "resume: PASSED"
exit 0
