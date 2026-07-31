#!/usr/bin/env bash
#
# THE AMITCP ARexx HOST RUN.
#
#   tests/tools/run-arexx.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#
# WHAT IT IS PROVING, and why a unit test cannot
#
#   docs/RESEARCH.md 75.7: AMITCP is not a flag, it is AmiTCP's ARexx host port,
#   and 31 of the 2,149 comm/ archives surveyed send commands to it. We
#   published the port with PA_IGNORE and no signal task, so RexxSysLib found it,
#   PutMsg()ed a RexxMsg and waited for a reply nothing would ever send. With no
#   port at all the same script fails at once with "host environment not found",
#   so the port made a clean failure into a hang.
#
#   The only way to show that is fixed is to run the real ARexx interpreter and
#   watch a real script come back. A unit test of the message handler proves the
#   handler works and says nothing about whether RexxSysLib is satisfied by it --
#   which is the entire question, since the hang was in RexxSysLib's wait, not in
#   our code.
#
#   So: RexxMast and RX out of Workbench 3.1, a script that addresses AMITCP,
#   and assertions on what it wrote down.
#
# THE CASES, and why each one is here
#
#   WaitForPort AMITCP     the reason the port exists at all. Commodore's own
#                          command, so this is not our idea of what waiting for
#                          the port means.
#   QUERY HOSTNAME         a command we implement: RC 0 and a result string.
#   FROBNICATE             a command nobody implements. RC must be non-zero and
#                          the script must continue -- that is the whole fix.
#   QUERY NOSUCHVARIABLE   a keyword we know with a variable we do not.
#   READ something         a keyword AmiTCP has and we decline. Distinct from
#                          FROBNICATE on purpose: "not implemented" and "unknown
#                          command" are different answers.
#   ''  (empty)            AmiTCP returns RETURN_OK for an empty line, so we do.
#   KILL                   last, because it takes the interfaces down. The most
#                          commonly sent command in the corpus -- every stopnet.
#
#   The script writes a sentinel line last. A hang is then not "some case
#   failed", it is "the sentinel is missing", which is the assertion that would
#   have caught the original bug.
#
# WHAT IS NOT COMMITTED
#
#   RexxMast, RX, WaitForPort and rexxsyslib.library are Commodore's. They are
#   located at run time, exactly as the Kickstart ROM, a2065.device and the C:
#   commands in run-tcphandler.sh already are:
#
#     AMINETXDUO_AMIGA_REXX=<dir with RexxMast, RX, WaitForPort,
#                            rexxsyslib.library>
#
#   Workbench 3.1's copies live on the Workbench disk in System/, Rexxc/ and
#   Libs/.
#
#   Note that Workbench 3.1 ships rexxsyslib 37. SetRexxVarFromMsg(), which is
#   how AMITCP.LASTERROR would be set, is V45 (OS 3.9), so on this run the
#   variable is deliberately not set and only RC is asserted. Nothing in the
#   corpus reads LASTERROR (75.7), and src/netstack/netstack_rexx.c gates the
#   call on lib_Version for exactly this reason.
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

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f -- build the tree first" >&2; exit 2; }
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
cp "$BSD"   "$STAGE/libs/bsdsocket.library"
cp "$REXXDIR/rexxsyslib.library" "$STAGE/libs/rexxsyslib.library"
# ARexx is a floating-point language: RexxMast opens mathieeedoubbas and
# mathieeedoubtrans at startup and exits with returncode 20 without them. On a
# bare directory boot there is no LIBS: full of Workbench, so they are staged
# with the rest.
for opt in rexxsupport.library mathieeedoubbas.library \
           mathieeedoubtrans.library mathieeesingtrans.library; do
    [ -f "$REXXDIR/$opt" ] && cp "$REXXDIR/$opt" "$STAGE/libs/$opt"
done
cp "$REXXDIR/RexxMast"    "$STAGE/RexxMast"
cp "$REXXDIR/RX"          "$STAGE/RX"
cp "$REXXDIR/WaitForPort" "$STAGE/WaitForPort"
cp "$TOOLS/AddNetInterface" "$STAGE/AddNetInterface"

# The script. `OPTIONS RESULTS` is what makes rm_Result2 be asked for, so the
# RXFF_RESULT branch in ami_rx_service() is exercised rather than skipped.
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

'KILL'
SAY 'case kill:    KILL               rc=' RC

SAY 'AREXX-SENTINEL-REACHED'
REXX

{
    echo "SYS:AddNetInterface eth0"
    echo "wait 2"

    # The barrier, with Commodore's own command. If the port were gone this
    # would never return and the run would time out.
    echo "SYS:WaitForPort AMITCP"

    # RexxMast detaches itself; RX needs it resident.
    echo "&SYS:RexxMast"
    echo "wait 3"

    echo "SYS:RX >DH0:arexx.txt DH0:amitcptest.rexx"
    echo "wait 2"
    echo "SYS:c/envsetup"
} > "$STAGE/commands.txt"

# ------------------------------------------------------------------- run ---

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-arexx}"
HD="$ROOT/build/testhd-$AMINETXDUO_RUN_TAG"

echo "==> booting $MODEL with the A2065 on SLIRP"
set +e
"$ROOT/tools/fsuae-run.sh" -n -m "$MODEL" -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/AddNetInterface" "$STAGE/RexxMast" "$STAGE/RX" \
    "$STAGE/WaitForPort" "$STAGE/amitcptest.rexx"
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
    echo "FAIL: the script produced no output at all -- RexxMast or RX did not run" >&2
    exit 1
}

# The one that matters: the script ran to the end. Every hang stops before this.
if grep -q "AREXX-SENTINEL-REACHED" "$SCRIPTOUT"; then
    note "PASS: the script reached its last line -- nothing blocked"
else
    note "FAIL: no sentinel: a command did not come back"
    fails=$((fails + 1))
fi

# A known command: rc 0, and a result string, which only arrives if
# rm_Result2/RXFF_RESULT is handled.
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

if grep -qE "case kill:.*rc= *0" "$SCRIPTOUT"; then
    note "PASS: KILL was accepted"
else
    note "FAIL: KILL was refused"
    fails=$((fails + 1))
fi

# WaitForPort ran before any of this and the run did not time out, so the
# barrier still works. Assert that ToolsSmoke got past it.
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
