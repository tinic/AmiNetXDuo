#!/usr/bin/env bash
#
# THE AMITCP ARexx HOST RUN.
#
#   tests/tools/run-arexx.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
MODEL=A1200
TIMEOUT=120
BUILD=build/cm

while getopts "m:t:b:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-b builddir]" >&2; exit 2 ;;
    esac
done

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" \
         "$TOOLS/RemoveNetInterface" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
done

# ---- Commodore's ARexx, located and never committed ------------------------

REXXDIR="${AMINETXDUO_AMIGA_REXX:-}"
if [ -z "$REXXDIR" ]; then
    for candidate in "$ROOT/build/rexxbin" "$HOME/amiga-rexx"; do
        [ -f "$candidate/RX" ] && { REXXDIR="$candidate"; break; }
    done
fi
for f in RexxMast RX WaitForPort rexxsyslib.library \
         mathieeedoubbas.library mathieeedoubtrans.library; do
    [ -f "$REXXDIR/$f" ] || {
        cat >&2 <<EOF
No AmigaOS ARexx found. This run's whole point is that the REAL interpreter is
satisfied by our host, so it will not substitute anything of ours for it.

  export AMINETXDUO_AMIGA_REXX=<a directory containing RexxMast, RX,
                                WaitForPort, rexxsyslib.library and the
                                mathieeedoub* libraries>

Workbench 3.1 keeps them in System/, Rexxc/ and Libs/ on the Workbench disk.
(looked in "${REXXDIR:-<nothing>}", missing $f)
EOF
        exit 2
    }
done

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

# --------------------------------------------------------------- staging ---

STAGE="$ROOT/build/arexx-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs" "$STAGE/rexxc"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"

for cfg in "$STAGE"/devs/NetInterfaces/*; do
    [ -f "$cfg" ] || continue
    printf 'MDNS=YES\n' >> "$cfg"
done
cp "$BSD"   "$STAGE/libs/bsdsocket.library"
cp "$REXXDIR/rexxsyslib.library" "$STAGE/libs/rexxsyslib.library"
for opt in rexxsupport.library mathieeedoubbas.library \
           mathieeedoubtrans.library mathieeesingtrans.library; do
    [ -f "$REXXDIR/$opt" ] && cp "$REXXDIR/$opt" "$STAGE/libs/$opt"
done
cp "$REXXDIR/RexxMast"    "$STAGE/RexxMast"
cp "$REXXDIR/RX"          "$STAGE/RX"
cp "$REXXDIR/WaitForPort" "$STAGE/WaitForPort"
cp "$TOOLS/AddNetInterface" "$STAGE/AddNetInterface"
cp "$TOOLS/RemoveNetInterface" "$STAGE/RemoveNetInterface"
cp "$TOOLS/nc"            "$STAGE/nc"
cp "$TOOLS/ShowNetStatus" "$STAGE/ShowNetStatus"

cat > "$STAGE/amitcptest.rexx" <<'REXX'
/* AMITCP ARexx host: does a real interpreter get an answer? */
OPTIONS RESULTS
OPTIONS FAILAT 9999

ADDRESS AMITCP

RESULT = 'NONE'
'QUERY HOSTNAME'
SAY 'case known:   QUERY HOSTNAME     rc=' RC ' result=' RESULT

'FROBNICATE'
SAY 'case unknown: FROBNICATE         rc=' RC

'QUERY NOSUCHVARIABLE'
SAY 'case badvar:  QUERY NOSUCHVAR    rc=' RC

'READ something'
SAY 'case unimpl:  READ something     rc=' RC

''
SAY 'case empty:                      rc=' RC

'Q HOSTNAME'
SAY 'case abbrev:  Q HOSTNAME         rc=' RC ' result=' RESULT

RESULT = 'NONE'
'QUERY CONNECTIONS'
SAY 'case conns:   QUERY CONNECTIONS  rc=' RC ' bytes=' LENGTH(RESULT)

RESULT = 'NONE'
'QUERY ROUTES ALL'
SAY 'case routes:  QUERY ROUTES ALL   rc=' RC ' bytes=' LENGTH(RESULT)

RESULT = 'NONE'
'QUERY ICMP CHKSUM IP TOTAL TCP CONNECT UDP ITOTAL'
SAY 'case stats:   QUERY live stats   rc=' RC ' result=' RESULT

/* SERVICES blocks for its collection window, which is the one command here
   that can wedge the host rather than answer it. One second, because what is
   being asserted is that it comes back and the script continues, nothing on
   SLIRP answers an mDNS query, so an empty result is the expected answer and
   a full one would mean the emulator had grown a network. */
RESULT = 'NONE'
'QUERY SERVICES ALL 1'
SAY 'case browse:  QUERY SERVICES ALL rc=' RC ' bytes=' LENGTH(RESULT)

'QUERY SERVICES _http._tcp 1'
SAY 'case browse1: QUERY SERVICES typ rc=' RC

'QUERY SERVICES'
SAY 'case browsex: QUERY SERVICES     rc=' RC

/* Runtime removal does not renumber the interfaces above its slot, so it
   deliberately leaves a NULL below ns_IfaceCount. KILL used to call Down on
   that hole and return WARN even though no live interface failed. */
ADDRESS COMMAND
'SYS:RemoveNetInterface eth0 FORCE QUIET'
SAY 'case hole:    RemoveNetInterface rc=' RC
ADDRESS AMITCP

'KILL'
SAY 'case kill:    KILL               rc=' RC

SAY 'AREXX-SENTINEL-REACHED'
REXX

{
    echo "SYS:AddNetInterface eth0"
    echo "wait 2"

    echo "&SYS:nc -l 7099 -v -w 90 >DH0:nc.txt"
    echo "wait 3"
    echo "SYS:ShowNetStatus USERS"

    echo "SYS:WaitForPort AMITCP"

    # RexxMast detaches itself; RX needs it resident.
    echo "&SYS:RexxMast"
    echo "wait 3"

    echo "SYS:RX >DH0:arexx.txt DH0:amitcptest.rexx"
    echo "wait 3"
    echo "SYS:ShowNetStatus USERS"
    echo "SYS:c/envsetup"
} > "$STAGE/commands.txt"

# ------------------------------------------------------------------- run ---

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-arexx}"
HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"

echo "==> booting $MODEL with the A2065 on SLIRP"
set +e
"$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/AddNetInterface" "$STAGE/RexxMast" "$STAGE/RX" \
    "$STAGE/WaitForPort" "$STAGE/amitcptest.rexx" \
    "$STAGE/RemoveNetInterface" "$STAGE/nc" "$STAGE/ShowNetStatus"
RUN_RC=$?
set -e

REPORT="$HD/tools.txt"
SCRIPTOUT="$HD/arexx.txt"

[ -f "$REPORT" ] || {
    echo "FAIL: the guest wrote no $REPORT (run rc=$RUN_RC)" >&2
    exit 1
}

echo
echo "==================== what the commands printed ====================="
cat "$REPORT"
echo
echo "==================== what the ARexx script said ===================="
if [ -f "$SCRIPTOUT" ]; then cat "$SCRIPTOUT"; else echo "(no $SCRIPTOUT)"; fi
echo "==================================================================="
echo

# ---------------------------------------------------------- assertions ---

fails=0
note() { echo "  $*"; }

[ -f "$SCRIPTOUT" ] || {
    echo "FAIL: the script produced no output at all, RexxMast or RX did not run" >&2
    exit 1
}

# The one that matters: the script ran to the end. Every hang stops before this.
if grep -q "AREXX-SENTINEL-REACHED" "$SCRIPTOUT"; then
    note "PASS: the script reached its last line, nothing blocked"
else
    note "FAIL: no sentinel: a command did not come back"
    fails=$((fails + 1))
fi

if grep -qE "case known:.*rc= *0 " "$SCRIPTOUT"; then
    note "PASS: QUERY HOSTNAME returned rc=0"
else
    note "FAIL: QUERY HOSTNAME did not return rc=0"
    fails=$((fails + 1))
fi
if grep -E "case known:" "$SCRIPTOUT" | grep -qvE "result= *(NONE)? *$"; then
    note "PASS: QUERY HOSTNAME handed back a result string"
else
    note "FAIL: QUERY HOSTNAME set no result (rm_Result2 not honoured)"
    fails=$((fails + 1))
fi

# An unknown command: answered, and answered with an error.
if grep -qE "case unknown:.*rc= *[1-9]" "$SCRIPTOUT"; then
    note "PASS: an unknown command got a non-zero rc instead of silence"
else
    note "FAIL: an unknown command did not report an error"
    fails=$((fails + 1))
fi

for case_name in badvar unimpl; do
    if grep -qE "case $case_name:.*rc= *[1-9]" "$SCRIPTOUT"; then
        note "PASS: $case_name reported an error"
    else
        note "FAIL: $case_name did not report an error"
        fails=$((fails + 1))
    fi
done

# AmiTCP returns RETURN_OK for an empty command line.
if grep -qE "case empty:.*rc= *0" "$SCRIPTOUT"; then
    note "PASS: an empty command line is not an error, as in AmiTCP"
else
    note "FAIL: an empty command line was rejected"
    fails=$((fails + 1))
fi

# FindArg() abbreviation, inherited from DOS rather than reimplemented.
if grep -qE "case abbrev:.*rc= *0 " "$SCRIPTOUT"; then
    note "PASS: 'Q' is accepted as QUERY"
else
    note "FAIL: the AmiTCP abbreviation 'Q' was not accepted"
    fails=$((fails + 1))
fi

for case_name in conns routes stats; do
    if grep -qE "case $case_name:.*rc= *0" "$SCRIPTOUT"; then
        note "PASS: $case_name returned from a live NetX snapshot"
    else
        note "FAIL: $case_name did not return rc=0"
        fails=$((fails + 1))
    fi
done

for case_name in browse browse1; do
    if grep -qE "case $case_name:.*rc= *0" "$SCRIPTOUT"; then
        note "PASS: $case_name returned from its collection window"
    else
        note "FAIL: $case_name did not return rc=0"
        fails=$((fails + 1))
    fi
done

if grep -qE "case browsex:.*rc= *[1-9]" "$SCRIPTOUT"; then
    note "PASS: SERVICES with no type is a syntax error"
else
    note "FAIL: SERVICES with no type was accepted"
    fails=$((fails + 1))
fi

if grep -qE "case hole:.*rc= *0" "$SCRIPTOUT"; then
    note "PASS: runtime removal created the interface-table hole"
else
    note "FAIL: RemoveNetInterface did not create the hole before KILL"
    fails=$((fails + 1))
fi

if grep -qE "case kill:.*rc= *0" "$SCRIPTOUT"; then
    note "PASS: KILL was accepted"
else
    note "FAIL: KILL was refused"
    fails=$((fails + 1))
fi

users_block() {
    awk -v want="$1" '
        index($0, "===== SYS:ShowNetStatus USERS =====") == 1 { n++; if (n == want) { on = 1; next } }
        on && /^----- rc / { exit }
        on { print }
    ' "$REPORT"
}

if users_block 1 | grep -qE "^nc +[0-9]"; then
    note "PASS: nc is listed as using the network before KILL"
else
    note "FAIL: nc was not listed before KILL, so this proves nothing"
    users_block 1 | sed 's/^/       /' >&2
    fails=$((fails + 1))
fi

if users_block 2 | grep -qE "^nc +[0-9]"; then
    note "FAIL: nc is STILL using the network after KILL: it was never told"
    users_block 2 | sed 's/^/       /' >&2
    fails=$((fails + 1))
else
    note "PASS: KILL told nc to stop and it let go of bsdsocket.library"
fi

if grep -q "WaitForPort" "$REPORT"; then
    note "PASS: WaitForPort AMITCP returned"
else
    note "note: WaitForPort left no line in the report"
fi

echo
if [ "$fails" -eq 0 ]; then
    echo "PASS: the AMITCP port is a real ARexx host; known and unknown commands"
    echo "      both answer, and nothing hangs."
    exit 0
fi

echo "FAIL: $fails assertion(s) failed" >&2
exit 1
