#!/usr/bin/env bash
# Fitz bulk throughput, on a link that has latency.
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"
. "$ROOT/tests/perf/peercap.sh"

PEER="${AMINETXDUO_FITZ_PEER:-}"
PEER_ADDR="${AMINETXDUO_FITZ_PEER_ADDR:-}"
PEER_DIR="${AMINETXDUO_FITZ_PEER_DIR:-}"
PEER_BIN="${AMINETXDUO_FITZ_PEER_BIN:-\$HOME/fitzsrc/fitz-serve}"
MODEL=A3000
CPU=""
BUILD="${AMINETXDUO_BUILD:-build/cm}"
KB=512
CHUNK=32768
REPS=3
TAG="${AMINETXDUO_RUN_TAG:-fitzbench}"
TIMEOUT=400
PORT="${AMINETXDUO_FITZ_PORT:-17712}"
SLIRP=0
ACCURATE=0
ROADSHOW=""
NGDIR="${AMINETXDUO_CMP_AMITCPNG:-}"
EXTRALIBS="${AMINETXDUO_FITZ_EXTRALIBS:-}"
USE_AMIBERRY=0
IFACE="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"
BOARD=a2065
LOSSCAP=0
MAXLOSS=""
MAXEFF=""

while getopts "H:A:m:c:b:k:C:r:T:t:p:sxR:aB:N:wl:L:G:E:" opt; do
    case "$opt" in
        H) PEER="$OPTARG" ;;
        A) PEER_ADDR="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        c) CPU="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        k) KB="$OPTARG" ;;
        C) CHUNK="$OPTARG" ;;
        r) REPS="$OPTARG" ;;
        T) TAG="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        p) PORT="$OPTARG" ;;
        s) SLIRP=1 ;;
        x) ACCURATE=1 ;;
        R) ROADSHOW="${OPTARG:-/tmp/rsdemo/Roadshow-Demo-1.15/Workbench}" ;;
        G) NGDIR="$OPTARG" ;;
        E) EXTRALIBS="$OPTARG" ;;
        a) USE_AMIBERRY=1 ;;
        B) USE_AMIBERRY=1; IFACE="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        w) LOSSCAP=1 ;;
        l) LOSSCAP=1; MAXLOSS="$OPTARG" ;;
        L) LOSSCAP=1; MAXEFF="$OPTARG" ;;
        *) echo "usage: $0 [-H user@host] [-A addr] [-m model] [-c cpu]" \
                "[-b build] [-k KB] [-C chunk] [-r reps] [-T tag] [-t secs]" \
                "[-p port] [-s] [-x] [-R roadshowdir] [-a] [-B iface]" \
                "[-N board] [-w] [-l pct] [-L pct]" >&2
           exit 2 ;;
    esac
done

PEER_DIR="${PEER_DIR:-/tmp/fitzbench-share-$PORT}"

case "$PEER" in
    *playhouse2*)
        echo "playhouse2 cannot serve this: VMs on one Proxmox host never cross" >&2
        echo "a NIC, so its TX checksums are never computed and our stack rejects" >&2
        echo "them, it reads as 6 bad packets and no transfer.  Use another." >&2
        exit 2 ;;
esac
[ -n "$PEER" ] || [ -n "$PEER_ADDR" ] || {
    echo "set AMINETXDUO_FITZ_PEER=<user@host> or pass -H, a third machine on" >&2
    echo "real hardware, not this emulator's host and not an LXC container." >&2
    echo "-A <addr> alone is enough if a fitz-serve is already listening." >&2
    exit 2
}

[ "$USE_AMIBERRY" = "0" ] || [ "$SLIRP" = "0" ] || {
    echo "-a and -s are different emulators; pick one" >&2; exit 2; }

if [ "$USE_AMIBERRY" = "1" ]; then
    . "$ROOT/tools/amiberry-resolve.sh"
    amiberry_resolve || exit 2
fi

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
UG="$ROOT/$BUILD/src/usergroup/usergroup.library"
BENCH="$ROOT/$BUILD/tests/perf/FitzBench"
FITZ="$ROOT/build/fitz/Fitz/fitz"

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$BSD" "$BENCH"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
done
[ -f "$FITZ" ] || {
    echo "missing $FITZ, run tests/endurance/fetch-fitz.sh" >&2; exit 2; }

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for candidate in \
        "$ROOT/build/a2065.device" \
        "$HOME/amiga-assets/devs/a2065.device" \
        "$HOME/amiga-os-src/os-source/other_networking/sana2/bin/devs/a2065.device"
    do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "No a2065.device found. Set AMINETXDUO_A2065=<path>." >&2; exit 2; }

. "$ROOT/tools/sana2-stage.sh"

case "$BOARD" in
    a2065|ariadne|ariadne2|hydra|eb920|xsurf|xsurf100z2|xsurf100z3|ne2000_pcmcia) ;;
    *) echo "unknown network board '$BOARD'.  tools/amiberry-run.sh knows:" >&2
       echo "  a2065 ariadne ariadne2 hydra eb920 xsurf xsurf100z2" >&2
       echo "  xsurf100z3 ne2000_pcmcia" >&2
       exit 2 ;;
esac

[ "$BOARD" = a2065 ] || [ "$USE_AMIBERRY" = "1" ] || {
    echo "-N $BOARD needs the Amiberry path (-a, or -B <iface>): the WinUAE" >&2
    echo "and SLIRP branches here boot an a2065 and nothing else." >&2
    exit 2; }

BOARD_DRIVER=""
BOARD_DEVICE=""
if [ "$BOARD" != a2065 ] &&
   [ -z "${AMINETXDUO_EXTRA_DRIVER:-}${AMINETXDUO_IFCONFIG:-}" ]; then
    BOARD_DEVICE=$(sana2_driver_for "$BOARD")
    BOARD_DRIVER=$(sana2_local_driver "$BOARD_DEVICE")
    [ -n "$BOARD_DRIVER" ] || {
        echo "-N $BOARD wants $BOARD_DEVICE and this host has not got it." >&2
        echo "Looked in:" >&2
        for _d in ${AMINETXDUO_SANA2_STORE:-} "$HOME/amiga-assets/devs"; do
            echo "  $_d" >&2
        done
        echo "Put the driver there, or name one:" >&2
        echo "  AMINETXDUO_EXTRA_DRIVER=<path to the .device>" >&2
        echo "  AMINETXDUO_IFCONFIG=<an eth0 whose DEVICE= is that driver>" >&2
        exit 2; }
fi


if [ -z "$PEER" ]; then
    [ -n "$PEER_ADDR" ] || {
        echo "no peer: give -A <addr> for a server that is already listening," >&2
        echo "or -H <user@host> to have this script start one" >&2; exit 2; }
    SERVER_ADDR="$PEER_ADDR"
    cleanup() { :; }
elif [ "$SLIRP" = "1" ]; then
    SERVER_ADDR=10.0.2.2
    LOCAL_SERVE="$ROOT/build/endurance/fitz-serve"
    [ -x "$LOCAL_SERVE" ] || {
        echo "missing $LOCAL_SERVE, run tests/endurance/build.sh" >&2; exit 2; }
    SHARE="$ROOT/build/fitzbench-share-$TAG"
    rm -rf "$SHARE"; mkdir -p "$SHARE"
    PEERLOG="$ROOT/build/fitzbench-$TAG-peer.log"
    "$LOCAL_SERVE" "$SHARE" PORT "$PORT" > "$PEERLOG" 2>&1 &
    PEER_PID=$!
    cleanup() { kill -TERM "$PEER_PID" 2>/dev/null || true; }
else
    PEER_ADDRS=$(ssh "$PEER" \
        "ip -4 -o addr show scope global 2>/dev/null | awk '{print \$4}' | cut -d/ -f1" \
        2>/dev/null)
    [ -n "$PEER_ADDRS" ] || {
        echo "$PEER did not report an address of its own; pass -A <addr>" >&2
        exit 2; }
    if [ -z "$PEER_ADDR" ]; then
        PEER_ADDR=$(printf '%s\n' "$PEER_ADDRS" | head -1)
        [ "$(printf '%s\n' "$PEER_ADDRS" | wc -l)" -eq 1 ] || {
            echo "note: $PEER holds more than one address, using $PEER_ADDR" >&2
            echo "      pass -A to choose the one the guest's bridge reaches" >&2; }
    else
        printf '%s\n' "$PEER_ADDRS" | grep -qx "$PEER_ADDR" || {
            echo "$PEER does not hold $PEER_ADDR -- the server would be staged" >&2
            echo "on one machine and the guest pointed at another.  It holds:" >&2
            printf '%s\n' "$PEER_ADDRS" | sed 's/^/  /' >&2
            exit 2; }
    fi

    SERVER_ADDR="$PEER_ADDR"
    PEERLOG="$ROOT/build/fitzbench-$TAG-peer.log"
    PEER_KILL="pkill -f '[f]itz-serve .* PORT $PORT'; sleep 1;
               pkill -9 -f '[f]itz-serve .* PORT $PORT'; true"
    ssh "$PEER" "$PEER_KILL" >/dev/null 2>&1 || true
    ssh "$PEER" "rm -rf $PEER_DIR; mkdir -p $PEER_DIR;
                 nohup $PEER_BIN $PEER_DIR PORT $PORT > /tmp/fitzbench-peer.log 2>&1 &
                 sleep 1; ps -o args= -C fitz-serve" > "$PEERLOG" 2>&1 || {
        echo "no fitz-serve is running on $PEER after starting one on port" >&2
        echo "$PORT.  $PEER_BIN is what was run; its output there is in" >&2
        echo "/tmp/fitzbench-peer.log, and what came back is:" >&2
        sed 's/^/  /' "$PEERLOG" >&2
        exit 2; }
    cat "$PEERLOG"
    cleanup() { ssh "$PEER" "$PEER_KILL" >/dev/null 2>&1 || true; }
fi
trap cleanup EXIT INT TERM HUP

echo "==> fitz-serve on $SERVER_ADDR:$PORT"


STAGE="$ROOT/build/fitzbench-stage-$TAG"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"
[ -z "${AMINETXDUO_EXTRA_DRIVER:-}" ] || \
    cp "$AMINETXDUO_EXTRA_DRIVER" "$STAGE/devs/$(basename "$AMINETXDUO_EXTRA_DRIVER")"
[ -z "${AMINETXDUO_IFCONFIG:-}" ] || \
    cp "$AMINETXDUO_IFCONFIG" "$STAGE/devs/NetInterfaces/eth0"
if [ -n "$BOARD_DRIVER" ]; then
    cp "$BOARD_DRIVER" "$STAGE/devs/$BOARD_DEVICE"
    sed "s|^DEVICE=.*|DEVICE=$BOARD_DEVICE|" "$STAGE/devs/NetInterfaces/eth0" \
        > "$STAGE/devs/NetInterfaces/eth0.new"
    mv "$STAGE/devs/NetInterfaces/eth0.new" "$STAGE/devs/NetInterfaces/eth0"
    echo "==> $BOARD: staged $BOARD_DEVICE from $BOARD_DRIVER"
fi

case "$BOARD" in
    a2065) _fb_dev=a2065.device; _fb_path=${A2065:-none} ;;
    *)     _fb_dev=$BOARD_DEVICE; _fb_path=$BOARD_DRIVER ;;
esac
[ -z "${AMINETXDUO_EXTRA_DRIVER:-}" ] || {
    _fb_dev=$(basename "$AMINETXDUO_EXTRA_DRIVER")
    _fb_path=$AMINETXDUO_EXTRA_DRIVER; }
[ -z "${AMINETXDUO_IFCONFIG:-}" ] ||
    _fb_dev=$(sed -n 's/^DEVICE=//p' "$STAGE/devs/NetInterfaces/eth0" | head -1)
printf 'sana2_staged board=%s driver=%s source=%s device=%s card=none dir=DEVS: path=%s\n' \
       "$BOARD" "${_fb_dev:-none}" \
       "$(case "$_fb_dev" in anxnet*) echo anxnet ;; *) echo vendor ;; esac)" \
       "${_fb_dev:-none}" "${_fb_path:-none}"

WANT_DEVICE=$(sed -n 's/^DEVICE=//p' "$STAGE/devs/NetInterfaces/eth0" |
              head -1 | tr -d '\r')
WANT_DEVICE=${WANT_DEVICE##*[:/]}
[ -n "$WANT_DEVICE" ] || {
    echo "the staged DEVS:NetInterfaces/eth0 has no DEVICE= line" >&2; exit 2; }
[ -n "$(find "$STAGE/devs" -name "$WANT_DEVICE" -print -quit)" ] || {
    echo "eth0 asks for $WANT_DEVICE and nothing staged it into DEVS:." >&2
    echo "What is on the drive:" >&2
    (cd "$STAGE/devs" && find . -name '*.device' | sed 's|^\./|  |') >&2
    exit 2; }
if [ -n "$NGDIR" ]; then
    [ -f "$NGDIR/Libs/bsdsocket.library" ] || {
        echo "no AmiTCP_NG at $NGDIR" >&2; exit 2; }
    cp "$NGDIR/Libs/bsdsocket.library" "$STAGE/libs/bsdsocket.library"
    [ -f "$UG" ] && cp "$UG" "$STAGE/libs/usergroup.library"
    cp "$NGDIR/C/AddNetInterface" "$STAGE/AddNetInterface"
    cp "$NGDIR/C/GetNetStatus"    "$STAGE/NetStat"
elif [ -n "$ROADSHOW" ]; then
    [ -f "$ROADSHOW/Libs/bsdsocket.library" ] || {
        echo "no Roadshow at $ROADSHOW" >&2; exit 2; }
    cp "$ROADSHOW/Libs/bsdsocket.library" "$STAGE/libs/bsdsocket.library"
    cp "$ROADSHOW/Libs/usergroup.library" "$STAGE/libs/usergroup.library"
    cp "$ROADSHOW/C/AddNetInterface"      "$STAGE/AddNetInterface"
    cp "$ROADSHOW/C/GetNetStatus"         "$STAGE/NetStat"
else
    cp "$BSD"   "$STAGE/libs/bsdsocket.library"
    [ -f "$UG" ] && cp "$UG" "$STAGE/libs/usergroup.library"
    cp "$TOOLS/AddNetInterface" "$STAGE/AddNetInterface"
fi
[ -n "$ROADSHOW$NGDIR" ] || cp "$TOOLS/netstat" "$STAGE/NetStat"
if [ -n "$EXTRALIBS" ]; then
    _found=0
    for _l in "$EXTRALIBS"/*.library; do
        [ -f "$_l" ] || continue
        cp "$_l" "$STAGE/libs/"
        _found=1
    done
    [ "$_found" = 1 ] || {
        echo "-E $EXTRALIBS holds no *.library" >&2; exit 2; }
fi
cp "$FITZ"  "$STAGE/fitz"
cp "$BENCH" "$STAGE/FitzBench"

STATARGS="-s"
[ -z "$ROADSHOW$NGDIR" ] || STATARGS=""

cat > "$STAGE/commands.txt" <<EOF
SYS:AddNetInterface eth0
wait 6
&SYS:fitz mount $SERVER_ADDR:$PORT FITZ:
wait 10
SYS:NetStat $STATARGS
SYS:FitzBench FITZ: KB=$KB CHUNK=$CHUNK REPS=$REPS
SYS:NetStat $STATARGS
SYS:FitzBench RAM: KB=$KB CHUNK=$CHUNK REPS=$REPS
EOF


export AMINETXDUO_RUN_TAG="$TAG"

CPUARG=()
[ -z "$CPU" ] || CPUARG=(-c "$CPU")
[ "$ACCURATE" = "0" ] || CPUARG+=(-x)

CAPDIR="$ROOT/build/losscap-$TAG"
CAPTURING=0
if [ "$LOSSCAP" = "1" ]; then
    if [ -z "$PEER" ]; then
        echo "-w needs an ssh-able peer (-H): the capture is taken there" >&2
        exit 2
    fi
    peercap_start "$PEER" "$PORT" "$CAPDIR" "$TAG" || exit 2
    CAPTURING=1
fi

set +e
if [ "$SLIRP" = "1" ]; then
    HD="$ROOT/build/amiberry-testhd-$TAG"
    "$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" -t "$TIMEOUT" "${CPUARG[@]}" \
        "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
        "$STAGE/AddNetInterface" "$STAGE/NetStat" "$STAGE/fitz" "$STAGE/FitzBench"
elif [ "$USE_AMIBERRY" = "1" ]; then
    # Amiberry is local, so this branch only works ON the machine it is
    # installed on, there is no ssh half the way winuae-run.sh has one.
    HD="$ROOT/build/amiberry-testhd-$TAG"
    "$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
        -t "$TIMEOUT" ${CPU:+-c "$CPU"} \
        "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
        "$STAGE/AddNetInterface" "$STAGE/NetStat" "$STAGE/fitz" "$STAGE/FitzBench"
else
    HD="$ROOT/build/winuae-testhd-$TAG"
    export AMINETXDUO_WINUAE_EXE="${AMINETXDUO_WINUAE_EXE:-C:\\winuae-patched\\winuae64.exe}"
    export AMINETXDUO_WINUAE_A2065="${AMINETXDUO_WINUAE_A2065:-\\Device\\NPF_{B0F2CE29-E3DB-4AB0-B55A-0BEDA6D1A48C}}"
    "$ROOT/tools/winuae-run.sh" -n -m "$MODEL" -t "$TIMEOUT" "${CPUARG[@]}" \
        "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
        "$STAGE/AddNetInterface" "$STAGE/NetStat" "$STAGE/fitz" "$STAGE/FitzBench"
fi
RUN_RC=$?
set -e

CAPTURE_LOST=0
if [ "$CAPTURING" = "1" ]; then
    peercap_stop "$PEER" "$CAPDIR" "$TAG" || { CAPTURING=0; CAPTURE_LOST=1; }
fi

REPORT="$HD/tools.txt"
[ -f "$REPORT" ] || { echo "FAIL: the guest wrote no $REPORT (run rc=$RUN_RC)" >&2; exit 1; }

if [ "$RUN_RC" != "0" ]; then
    echo "FAIL: the emulator run failed (rc=$RUN_RC); no figure from it is" >&2
    echo "usable.  Its own output above says which assertion refused it." >&2
    exit "$RUN_RC"
fi

echo
echo "===================== what the commands printed ====================="
cat "$REPORT"
echo "====================================================================="
echo

FIGURES=$(awk '
    # The arm each line belongs to.  fitzbench names it itself further down
    # ("fitzbench: file=FITZ:fitzbench.dat"), but the header is what brackets
    # the whole arm including a failure printed before any name.
    # if/else and not a ternary split over two lines: the awk macOS ships is
    # the one true awk, which will not parse that, and the default path of this
    # script is a Mac driving WinUAE over ssh.
    /^===== / { arm = ""
                if ($0 ~ /FitzBench FITZ:/)     arm = "fitz"
                else if ($0 ~ /FitzBench RAM:/) arm = "ram" }

    # Cut out of the line rather than taken as $3: a transcript line can carry a
    # prefix, and a field index that is off by one silently records the
    # direction as something no lookup below will ever match.
    arm && /fitzbench: RESULT [a-z]+ FAILED/ {
        d = $0; sub(/.*fitzbench: RESULT /, "", d); sub(/[^a-z].*/, "", d)
        failed[arm " " d] = 1 }
    arm && /fitzbench: RESULT [a-z]+ kbs_mean=/ {
        d = $0; sub(/.*fitzbench: RESULT /, "", d); sub(/[^a-z].*/, "", d)
        v = $0; sub(/.*kbs_mean=/, "", v); sub(/[^0-9].*/, "", v)
        kbs[arm " " d] = v + 0 }

    END {
        split("fitz ram", arms, " ")
        split("write read", dirs, " ")
        for (a = 1; a <= 2; a++) for (d = 1; d <= 2; d++) {
            k = arms[a] " " dirs[d]
            if (failed[k])
                bad = bad sprintf("  %s %s: the guest printed RESULT %s FAILED\n",
                                  arms[a], dirs[d], dirs[d])
            else if (!(k in kbs))
                bad = bad sprintf("  %s %s: no RESULT line for it at all\n",
                                  arms[a], dirs[d])
            else if (kbs[k] <= 0)
                bad = bad sprintf("  %s %s: kbs_mean=0\n", arms[a], dirs[d])
            else
                printf "%s_%s_kbs=%d\n", arms[a], dirs[d], kbs[k]
        }
        if (bad != "") { printf "%s", bad > "/dev/stderr"; exit 1 }
    }
' "$REPORT") || {
    echo "FAIL: this run measured nothing it can report.  fitz is the network" >&2
    echo "arm and ram is the control that runs beside it in the same boot; the" >&2
    echo "transcript above says what the guest did instead." >&2
    printf '%s\n' "$FIGURES"
    exit 1
}

echo "==> results ($MODEL${CPU:+/$CPU}, $KB KB, chunk $CHUNK, $REPS reps)"
grep "fitzbench: RESULT\|fitzbench: file=" "$REPORT" | sed 's/^/    /'
printf '%s\n' "$FIGURES"

echo
awk -v kb="$KB" -v reps="$REPS" -v board="$BOARD" '
    /^===== / { cmd = $0; infitz = (cmd ~ /FitzBench FITZ:/); inip = 0; inif = 0 }

    cmd ~ /NetStat/ && /^ip:/    { inip = 1; next }
    cmd ~ /NetStat/ && /^eth[0-9]/ { inif = 1; next }
    /^[^\t ]/                    { inip = 0; inif = 0 }

    inip && /packets sent/     { b = $4; gsub(/[()]/, "", b)
                                 ns[n_s++] = $1 + 0; nb[n_s - 1] = b + 0 }
    inip && /packets received/ { b = $4; gsub(/[()]/, "", b)
                                 nr[n_r++] = $1 + 0; nrb[n_r - 1] = b + 0 }
    inif && /packets received/ { fr[n_f] = $3 + 0; fs[n_f++] = $6 + 0 }

    infitz && /RESULT write kbs_mean=/ { sub(/.*kbs_mean=/, ""); wkbs = $1 + 0 }
    infitz && /RESULT read kbs_mean=/  { sub(/.*kbs_mean=/, ""); rkbs = $1 + 0 }

    END {
        # Says so rather than printing nothing.  The block needs the NetStat
        # pair that brackets the network arm, and a stack that ships no NetStat
        # of ours (-R, -G) or a run whose second snapshot never printed has
        # neither -- which looked exactly like a run where the wire was idle.
        if (n_s < 2 || n_r < 2 || wkbs == 0) {
            printf "==> no packet-rate block: ip: tx counted %d time(s) and ip: rx %d, two of each are needed\n", n_s, n_r
            exit 0
        }
        dps = ns[1] - ns[0];  dbs = nb[1] - nb[0]
        dpr = nr[1] - nr[0];  dbr = nrb[1] - nrb[0]
        printf "==> what crossed the wire (NetStat pair around the network arm)\n"
        printf "    ip tx %d packets / %d bytes", dps, dbs
        if (dps > 0) printf ", mean %d bytes", dbs / dps
        printf "\n"
        printf "    ip rx %d packets / %d bytes", dpr, dbr
        if (dpr > 0) printf ", mean %d bytes", dbr / dpr
        printf "\n"
        if (n_f >= 2)
            printf "    %s frames: %d sent, %d received\n",
                   board, fs[1] - fs[0], fr[1] - fr[0]
        # FitzBench moves KB each way once to warm up and then once per rep, so
        # the bytes in the bracket are known and the measured rates turn them
        # into the seconds they took.  Both directions ran back to back, so
        # this is the frame rate the card sustained across the arm rather than
        # either direction on its own.
        secs = (reps + 1) * (kb / wkbs + kb / rkbs) + 64 / rkbs
        if (secs > 0 && n_f >= 2)
            printf "    ~%d frames/s sustained over ~%.1f s of transfer\n",
                   ((fs[1] - fs[0]) + (fr[1] - fr[0])) / secs, secs
    }
' "$REPORT"

# The scheduler block is only there against a library that has NETSTATUS_HEALTH.
if grep -q "^scheduler:" "$REPORT"; then
    echo
    echo "==> was the machine ever held (last NetStat)"
    awk '/^scheduler:/ { s = ""; g = 1; next }
         g && /^\t/    { s = s $0 "\n"; next }
         g             { g = 0; last = s }
         END           { printf "%s", (g ? s : last) }' "$REPORT" | sed 's/^/  /'
fi

LOSS_RC=0
if [ "$CAPTURING" = "1" ]; then
    LOSSARGS=(--per-phase)
    [ -z "$MAXLOSS" ] || LOSSARGS+=(--max-loss "$MAXLOSS")
    [ -z "$MAXEFF" ]  || LOSSARGS+=(--max-effective-loss "$MAXEFF")
    set +e
    peercap_report "$CAPDIR" "$TAG" "${LOSSARGS[@]}"
    LOSS_RC=$?
    set -e
elif [ "$CAPTURE_LOST" = "1" ]; then
    echo "the capture started and did not come back: this run has a throughput" >&2
    echo "figure and no loss rate, and any -l/-L asked for did not run." >&2
    LOSS_RC=2
fi

exit "$LOSS_RC"
