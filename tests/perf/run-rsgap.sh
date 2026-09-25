#!/usr/bin/env bash
#
# Roadshow vs AmiNetXDuo receive, one stack per boot: docs/plans/roadshow-rx-gap.md.
#
#   tests/perf/run-rsgap.sh [-s R,A,A,R | -n BOOTS_PER_ARM] [-k TRANSFERS]
#                           [-z BYTES] [-r READSIZE] [-w WD_SECS] [-x FAULT]
#                           [-P PEERHOST] [-A PEERADDR] [-p PORT] [-H HTTPPORT]
#                           [-R ROADSHOWDIR] [-b BUILD] [-D DRIVER]
#                           [-m MODEL] [-N BOARD] [-B IFACE] [-t SECS]
#                           [-T TAG] [-o OUTDIR] [-F]
#
# THE BOOT IS THE UNIT, and the guest runs the whole schedule by itself:
#
#   * S:rsgap-arm is the selector and it is ONE-SHOT.  `RsGapBoot SELECT`
#     reads it and deletes it before any stack starts.  No selector means
#     AmiNetXDuo in door mode, so a boot that dies can only come back as A.
#   * A watchdog (`RsGapBoot WATCHDOG`, started with Run before the stack)
#     ColdReboot()s after -w seconds.  Measurement boots never cancel it; they
#     reboot themselves when done.  The door boot's is cancelled only by the
#     HOST, over the door, after it has fetched a file through it.
#   * A measurement boot runs -k transfers of -z bytes, records, pops the next
#     arm off S:rsgap-queue into the selector, and reboots.  When the queue
#     is empty nothing is written, so the next boot is the door boot: that is
#     the rollback (selector back to A, one boot, hashes recorded again).
#
# ORDER.  -n N gives R/A/A/R blocks, so neither arm always follows power-on;
# -s names the order outright (-s A,A is a null control).  Every staged image
# is hashed and printed before the first boot, and every boot hashes what it
# actually loaded, so a mismatch is visible per boot.
#
# -z defaults to the protocol's 16 MB client transfer; every transfer rewrites
# the same RAM: file, so the machine needs that much free RAM once.  The
# 100,000,000-byte figure is the Fitz cross-check's, not this client's.
#
# -x M makes measurement boot M hang: the peer accepts its first transfer and
# never sends.  The watchdog must reboot it within -w, the next boot must be
# A with no selector, and the host must reach A's door.  That is the recovery
# proof the hardware GO waits on.
#
# -F prints the per-boot Startup-Sequence fragment for a real machine and
# exits: the same lines this boots, without the emulator-only HOLD.
#
# THE EMULATOR PRICES NOTHING HERE.  Amiberry services an X-Surf longword read
# as two word calls; its figures prove the procedure, never the gap.
#
# Output is key=value; the exit code is rsgap_collect.py's (0 = the session did
# what the schedule asked).  2 = the rig refused before any boot.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT" || exit 2

ENVFILE="${AMINETXDUO_RSGAP_ENV:-$HOME/amiga-assets/env.sh}"
# shellcheck disable=SC1090
[ ! -f "$ENVFILE" ] || . "$ENVFILE"

SCHED=""
PER_ARM=2
XFERS=4
BYTES=16000000
READSIZE=16384
WD=900
FAULT=0
PEER="${AMINETXDUO_RSGAP_PEER:-playhouse4}"
PEER_ADDR=""
PORT=17810
HTTPPORT=8080
RSDIR="${AMINETXDUO_CMP_ROADSHOW:-}"
BUILD="${AMINETXDUO_BUILD:-build/cm}"
DRIVER="${AMINETXDUO_RSGAP_DRIVER:-$HOME/amiga-assets/devs/x-surf-100.device}"
MODEL=A3000
BOARD=xsurf100z3
IFACE="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"
TIMEOUT=""
TAG="rsgap-$(date +%H%M%S)"
OUT=""
FRAGMENT_ONLY=0

while getopts "s:n:k:z:r:w:x:P:A:p:H:R:b:D:m:N:B:t:T:o:F" opt; do
    case "$opt" in
        s) SCHED="$OPTARG" ;;
        n) PER_ARM="$OPTARG" ;;
        k) XFERS="$OPTARG" ;;
        z) BYTES="$OPTARG" ;;
        r) READSIZE="$OPTARG" ;;
        w) WD="$OPTARG" ;;
        x) FAULT="$OPTARG" ;;
        P) PEER="$OPTARG" ;;
        A) PEER_ADDR="$OPTARG" ;;
        p) PORT="$OPTARG" ;;
        H) HTTPPORT="$OPTARG" ;;
        R) RSDIR="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        D) DRIVER="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        T) TAG="$OPTARG" ;;
        o) OUT="$OPTARG" ;;
        F) FRAGMENT_ONLY=1 ;;
        *) sed -n '3,10p' "$0" >&2; exit 2 ;;
    esac
done

refuse() { echo "rsgap_refused reason=$1${2:+ detail=$2}"; exit 2; }

# ---------------------------------------------------------------- schedule --
if [ -z "$SCHED" ]; then
    i=0
    while [ "$i" -lt "$PER_ARM" ]; do
        if [ $((i % 2)) = 0 ]; then SCHED="$SCHED,R,A"; else SCHED="$SCHED,A,R"; fi
        i=$((i + 1))
    done
    SCHED="${SCHED#,}"
fi
IFS=, read -r -a ARMS <<< "$SCHED"
for a in "${ARMS[@]}"; do
    case "$a" in R|A) ;; *) refuse bad_schedule "$SCHED" ;; esac
done
NBOOTS=${#ARMS[@]}
[ "$FAULT" -le "$NBOOTS" ] || refuse fault_past_schedule "$FAULT"
[ -n "$PEER_ADDR" ] || PEER_ADDR=$(ssh -o BatchMode=yes -o ConnectTimeout=10 "$PEER" \
    "ip -4 -o addr show scope global | awk '{print \$4}' | cut -d/ -f1 | head -1" \
    2>/dev/null)
[ -n "$PEER_ADDR" ] || refuse no_peer_addr "$PEER"
DOOR_PORT=$((PORT + 1))

# Every boot is bounded by its watchdog, so the session is bounded by the
# boots (the schedule plus the door boot) times the watchdog plus a reboot.
# Inside that, a boot that has not advanced the guest's boot counter within
# WD + 90 s is a defect and the session stops there (rsgap_stall).
[ -n "$TIMEOUT" ] || TIMEOUT=$(( (NBOOTS + 1) * (WD + 60) + 60 ))
STALL_S=$(( WD + 90 ))

GUEST_FILES="RSGAPARM:libs/bsdsocket.library RSGAPARM:libs/usergroup.library RSGAPARM:C/AddNetInterface DEVS:x-surf-100.device DEVS:NetInterfaces/eth0 C:RsGapRx C:RsGapBoot S:Startup-Sequence"

# ---------------------------------------------------------------- fragment --
fragment() { # emulator(0|1)
    local i
    echo "; rsgap boot fragment (docs/plans/roadshow-rx-gap.md): before any network start"
    echo "C:RsGapBoot SELECT"
    echo "If WARN"
    echo "  Run >NIL: C:RsGapBoot WATCHDOG SECS=$WD CANCELFILE=SYS:rsgap/wd-cancel"
    echo "  RSGAPARM:C/AddNetInterface eth0 >>SYS:rsgap/cur.log"
    echo "  C:RsGapBoot RECORD FILES $GUEST_FILES >>SYS:rsgap/cur.kv"
    echo "  C:RsGapRx HOST=$PEER_ADDR PORT=$DOOR_PORT BYTES=4096 TO=RAM:rsgap-door.dat TAG=door >>SYS:rsgap/cur.kv"
    echo "  Run >NIL: RSGAPARM:C/httpd SYS:rsgap PORT $HTTPPORT"
    [ "$1" = 0 ] || echo "  C:RsGapBoot HOLD SECS=20 >>SYS:rsgap/cur.kv"
    echo "Else"
    echo "  Run >NIL: C:RsGapBoot WATCHDOG SECS=$WD"
    echo "  RSGAPARM:C/AddNetInterface eth0 >>SYS:rsgap/cur.log"
    echo "  C:RsGapBoot RECORD FILES $GUEST_FILES >>SYS:rsgap/cur.kv"
    i=1
    while [ "$i" -le "$XFERS" ]; do
        echo "  C:RsGapRx HOST=$PEER_ADDR PORT=$PORT BYTES=$BYTES TO=RAM:rsgap.dat READSIZE=$READSIZE TAG=$i >>SYS:rsgap/cur.kv"
        i=$((i + 1))
    done
    echo "  C:RsGapBoot NEXT >>SYS:rsgap/cur.kv"
    echo "  C:RsGapBoot REBOOT >>SYS:rsgap/cur.kv"
    echo "EndIf"
}

if [ "$FRAGMENT_ONLY" = 1 ]; then
    fragment 0
    exit 0
fi

# ------------------------------------------------------------------- stage --
OUT="${OUT:-$ROOT/build/rsgap-$TAG}"
STAGE="$OUT/stage"
rm -rf "$OUT"
mkdir -p "$STAGE/c" "$STAGE/libs" "$STAGE/s" "$STAGE/devs/NetInterfaces" \
         "$STAGE/rsgap/arm/R/C" "$STAGE/rsgap/arm/R/libs" \
         "$STAGE/rsgap/arm/A/C" "$STAGE/rsgap/arm/A/libs" "$OUT/results"

TOOLS="$ROOT/$BUILD/src/tools"
need() { [ -f "$1" ] || refuse missing "$1"; }
for f in "$ROOT/$BUILD/tests/perf/RsGapRx" "$ROOT/$BUILD/tests/perf/RsGapBoot" \
         "$ROOT/$BUILD/src/bsdsocket/bsdsocket.library" \
         "$ROOT/$BUILD/src/usergroup/usergroup.library" \
         "$TOOLS/AddNetInterface" "$TOOLS/httpd" "$DRIVER" \
         "$RSDIR/Libs/bsdsocket.library" "$RSDIR/Libs/usergroup.library" \
         "$RSDIR/C/AddNetInterface"; do
    need "$f"
done
cp "$ROOT/$BUILD/tests/perf/RsGapRx" "$ROOT/$BUILD/tests/perf/RsGapBoot" "$STAGE/c/"
cp "$RSDIR/Libs/bsdsocket.library" "$RSDIR/Libs/usergroup.library" "$STAGE/rsgap/arm/R/libs/"
cp "$RSDIR/C/AddNetInterface" "$STAGE/rsgap/arm/R/C/"
cp "$ROOT/$BUILD/src/bsdsocket/bsdsocket.library" \
   "$ROOT/$BUILD/src/usergroup/usergroup.library" "$STAGE/rsgap/arm/A/libs/"
cp "$TOOLS/AddNetInterface" "$TOOLS/httpd" "$STAGE/rsgap/arm/A/C/"
cp "$DRIVER" "$STAGE/devs/x-surf-100.device"
cp -R "$ROOT/tests/netstack/devs/Internet" "$STAGE/devs/Internet"
printf 'DEVICE=x-surf-100.device\nUNIT=0\nCONFIGURE=DHCP\n' > "$STAGE/devs/NetInterfaces/eth0"
: > "$STAGE/libs/.keep"

# The first arm is the selector; the rest is the queue NEXT pops from.
printf '%s\n' "${ARMS[0]}" > "$STAGE/s/rsgap-arm"
: > "$STAGE/s/rsgap-queue"
for a in "${ARMS[@]:1}"; do printf '%s\n' "$a" >> "$STAGE/s/rsgap-queue"; done
[ -s "$STAGE/s/rsgap-queue" ] || rm -f "$STAGE/s/rsgap-queue"
fragment 1 > "$STAGE/fragment.txt"

echo "rsgap_plan tag=$TAG schedule=$SCHED boots=$NBOOTS transfers=$XFERS bytes=$BYTES read_size=$READSIZE wd_secs=$WD fault_boot=$FAULT peer=$PEER peer_addr=$PEER_ADDR port=$PORT door_port=$DOOR_PORT http_port=$HTTPPORT model=$MODEL board=$BOARD iface=$IFACE timeout=$TIMEOUT out=$OUT"

# ------------------------------------------------ hashes, before any boot --
IMAGES="$OUT/images.kv"
img() { # arm guestpath hostfile
    printf 'rsgap_image arm=%s guest=%s md5=%s size=%s host=%s\n' "$1" "$2" \
        "$(md5sum "$3" | cut -c1-32)" "$(stat -c %s "$3")" "$3"
}
{
    for a in R A; do
        img "$a" RSGAPARM:libs/bsdsocket.library "$STAGE/rsgap/arm/$a/libs/bsdsocket.library"
        img "$a" RSGAPARM:libs/usergroup.library "$STAGE/rsgap/arm/$a/libs/usergroup.library"
        img "$a" RSGAPARM:C/AddNetInterface "$STAGE/rsgap/arm/$a/C/AddNetInterface"
    done
    img A RSGAPARM:C/httpd "$STAGE/rsgap/arm/A/C/httpd"
    img '*' DEVS:x-surf-100.device "$STAGE/devs/x-surf-100.device"
    img '*' DEVS:NetInterfaces/eth0 "$STAGE/devs/NetInterfaces/eth0"
    img '*' C:RsGapRx "$STAGE/c/RsGapRx"
    img '*' C:RsGapBoot "$STAGE/c/RsGapBoot"
} > "$IMAGES"
cat "$IMAGES"

# -------------------------------------------------------------------- peer --
PEER_PY="/tmp/rsgappeer-$TAG.py"
PEER_LOG="/tmp/rsgappeer-$TAG.log"
STALL=0
[ "$FAULT" = 0 ] || STALL=$(( (FAULT - 1) * XFERS + 1 ))
scp -q "$ROOT/tests/perf/rsgappeer.py" "$PEER:$PEER_PY" || refuse peer_copy "$PEER"
# A pid file, not pkill -f: the pattern would match the ssh shell carrying it.
PEER_PID="/tmp/rsgappeer-$TAG.pid"
ssh "$PEER" "[ ! -f $PEER_PID ] || kill \$(cat $PEER_PID) 2>/dev/null; nohup python3 $PEER_PY serve --port $PORT --bytes $BYTES --door-port $DOOR_PORT --stall-from $STALL --lifetime $((TIMEOUT + 120)) > $PEER_LOG 2>&1 < /dev/null & echo \$! > $PEER_PID; sleep 1; head -1 $PEER_LOG" \
    > "$OUT/peer-start.txt" 2>&1
grep -q 'event=start' "$OUT/peer-start.txt" || refuse peer_start "$(tr '\n ' '_' < "$OUT/peer-start.txt")"
peer_stop() { ssh "$PEER" "[ ! -f $PEER_PID ] || { p=\$(cat $PEER_PID); kill \$p 2>/dev/null; for i in \$(seq 1 20); do kill -0 \$p 2>/dev/null || break; sleep 1; done; }; rm -f $PEER_PID" >/dev/null 2>&1 || true
              scp -q "$PEER:$PEER_LOG" "$OUT/peer.log" 2>/dev/null || true; }
PEER_IF=$(ssh "$PEER" "ip -o route get $PEER_ADDR 2>/dev/null | sed -n 's/.* dev \\([^ ]*\\).*/\\1/p'; ip -o -4 addr show | awk '\$4 ~ /^$PEER_ADDR\\//{print \$2}'" 2>/dev/null | grep -v '^lo$' | head -1)
ssh "$PEER" "printf 'rsgap_peer host=%s kernel=%s iface=%s ' \$(hostname) \$(uname -r) $PEER_IF; /usr/sbin/ethtool -k $PEER_IF 2>/dev/null | awk -F': ' '/^(tx-checksumming|rx-checksumming|tcp-segmentation-offload|generic-segmentation-offload|generic-receive-offload|large-receive-offload):/{gsub(/-/,\"_\",\$1); sub(/ .*/,\"\",\$2); printf \"%s=%s \", \$1, \$2}'; /usr/sbin/ethtool $PEER_IF 2>/dev/null | awk -F': ' '/Speed:|Duplex:/{gsub(/^[ \\t]+/,\"\",\$1); sub(/ .*/,\"\",\$2); printf \"%s=%s \", tolower(\$1), \$2}'; echo" 2>/dev/null

# ---------------------------------------------------------------- the boots --
HD="$ROOT/build/amiberry-testhd-$TAG"
RUNLOG="$OUT/amiberry-run.txt"
echo "rsgap_session state=start tag=$TAG hd=$HD runlog=$RUNLOG"
AMINETXDUO_RUN_TAG="$TAG" AMINETXDUO_AMIBERRY_STARTUP="$STAGE/fragment.txt" \
AMINETXDUO_STALL_SECS="$TIMEOUT" \
    "$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" -t "$TIMEOUT" \
    "$STAGE/c/RsGapBoot" "$STAGE/c" "$STAGE/libs" "$STAGE/s" "$STAGE/devs" \
    "$STAGE/rsgap" > "$RUNLOG" 2>&1 &
RUN_PID=$!
trap 'kill "$RUN_PID" 2>/dev/null; peer_stop' EXIT INT TERM HUP

# ------------------------------------------------------------------ the door --
# The door boot connects to the peer's door port first, so the peer log names
# its address.  Then: fetch through httpd, and only then cancel the watchdog by
# writing its cancel file THROUGH the door, and read back that it took.
DOOR="$OUT/door.kv"
: > "$DOOR"
door_done=0
t0=$(date +%s)
bc_t=$t0
while kill -0 "$RUN_PID" 2>/dev/null; do
    if [ "$door_done" = 0 ]; then
        dl=$(ssh -o ConnectTimeout=10 "$PEER" "grep 'kind=door' $PEER_LOG | tail -1" 2>/dev/null)
        if [ -n "$dl" ]; then
            ip=$(printf '%s\n' "$dl" | sed -n 's/.* client=\([0-9.]*\):.*/\1/p')
            url="http://$ip:$HTTPPORT"
            dget() { ssh -o ConnectTimeout=10 "$PEER" "python3 $PEER_PY http get $url/$1" > "$2" 2>/dev/null; }
            code=000; td=$(date +%s)
            for _ in $(seq 1 30); do
                dget cur.kv "$OUT/results/door-cur.kv" &&
                    grep -q 'rec=select' "$OUT/results/door-cur.kv" && { code=200; break; }
                sleep 2
            done
            door=down; [ "$code" = 200 ] && door=up
            fetched=0
            if [ "$door" = up ]; then
                last=$(sed -n 's/.*rec=select boot=\([0-9]*\).*/\1/p' "$OUT/results/door-cur.kv" | head -1)
                n=1
                while [ "$n" -lt "${last:-1}" ]; do
                    for ext in kv wd log; do
                        if dget "boot-$n.$ext" "$OUT/results/boot-$n.$ext"; then
                            fetched=$((fetched + 1))
                        else
                            rm -f "$OUT/results/boot-$n.$ext"
                        fi
                    done
                    n=$((n + 1))
                done
                mv "$OUT/results/door-cur.kv" "$OUT/results/cur.kv"
            fi
            put=000; confirmed=0
            if [ "$door" = up ]; then
                put=$(printf 'cancel\n' | ssh -o ConnectTimeout=10 "$PEER" "python3 $PEER_PY http put $url/wd-cancel" 2>/dev/null |
                      sed -n 's/^http_status=//p')
                for _ in $(seq 1 10); do
                    sleep 2
                    dget cur.wd "$OUT/results/cur.wd"
                    grep -q 'state=cancelled via=file' "$OUT/results/cur.wd" 2>/dev/null && { confirmed=1; break; }
                done
                dget cur.kv "$OUT/results/cur.kv"
            fi
            printf 'door=%s door_ip=%s door_http=%s door_fetch=%s cancel_put=%s cancel_confirmed=%s door_wait_s=%s\n' \
                "$door" "$ip" "$code" "$fetched" "$put" "$confirmed" "$(( $(date +%s) - td ))" > "$DOOR"
            door_done=1
        fi
    fi
    # Emulator-side progress: the guest's boot counter on the shared drive.
    bc=$(cat "$HD/rsgap/bootcount" 2>/dev/null | tr -dc 0-9)
    if [ "${bc:-0}" != "${last_bc:-x}" ]; then
        last_bc=${bc:-0}; bc_t=$(date +%s)
        echo "rsgap_progress boot=${bc:-0} t_s=$(( bc_t - t0 ))"
    elif [ "$door_done" = 0 ] && [ $(( $(date +%s) - bc_t )) -gt "$STALL_S" ]; then
        echo "rsgap_stall boot=${bc:-0} quiet_s=$(( $(date +%s) - bc_t )) bound_s=$STALL_S"
        kill "$RUN_PID" 2>/dev/null
        break
    fi
    [ $(( $(date +%s) - t0 )) -lt $((TIMEOUT + 60)) ] || break
    sleep 3
done
wait "$RUN_PID"; RUN_RC=$?
trap - EXIT INT TERM HUP
peer_stop
echo "rsgap_session state=end amiberry_rc=$RUN_RC wall_s=$(( $(date +%s) - t0 ))"
cat "$DOOR" | sed 's/^/rsgap_doorcheck /'

# The emulator's own copy of SYS:rsgap, for what the door could not fetch and
# as a cross-check of what it did.
cp -R "$HD/rsgap" "$OUT/hd-rsgap" 2>/dev/null
for f in "$OUT"/hd-rsgap/boot-*.* ; do
    [ -f "$f" ] || continue
    b=$(basename "$f")
    if [ ! -f "$OUT/results/$b" ]; then
        cp "$f" "$OUT/results/$b"; echo "rsgap_fallback file=$b source=emulator_disk"
    elif ! cmp -s "$f" "$OUT/results/$b"; then
        echo "rsgap_fallback file=$b door_copy_differs=1"
    fi
done
[ -f "$OUT/results/cur.kv" ] || cp "$OUT/hd-rsgap/cur.kv" "$OUT/results/" 2>/dev/null
[ -f "$OUT/results/cur.wd" ] || cp "$OUT/hd-rsgap/cur.wd" "$OUT/results/" 2>/dev/null
# The guest hashed S:Startup-Sequence as booted; the host's copy is the same file.
img '*' S:Startup-Sequence "$HD/s/Startup-Sequence" >> "$IMAGES" 2>/dev/null

python3 "$ROOT/tests/perf/rsgap_collect.py" --results "$OUT/results" \
    --images "$IMAGES" --peer-log "$OUT/peer.log" --door "$DOOR" \
    --schedule "$SCHED" --transfers "$XFERS" --bytes "$BYTES" \
    --wd-secs "$WD" --fault-boot "$FAULT" | tee "$OUT/boots.kv"
RC=${PIPESTATUS[0]}
grep '^rsgap_peer\|event=conn' "$OUT/peer.log" 2>/dev/null | sed 's/^/rsgap_peerlog /'

# One arm only is a null control: consecutive boots of it are paired.
NULLARG=()
case ",$SCHED," in
    *,R,*,A,*|*,A,*,R,*) ;;
    *,A,*) NULLARG=(--null A) ;;
    *,R,*) NULLARG=(--null R) ;;
esac
echo "rsgap_rig emulator=amiberry prices=nothing note=procedure_only"
python3 "$ROOT/tests/perf/rsgap_verdict.py" "${NULLARG[@]}" "$OUT/boots.kv" | tee "$OUT/verdict.kv"
exit "$RC"
