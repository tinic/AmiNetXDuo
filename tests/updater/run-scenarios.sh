#!/usr/bin/env bash
#
# The updater scenarios of tests/updater/scenarios.tsv, end to end.
#
#   tests/updater/run-scenarios.sh [-Y <id,...|all>] [-H user@host]
#                                  [-O <old dist>] [-R <releases>] [-p port]
#
# 1. The releases: tests/updater/make-releases.sh (old = this tree, new = the
#    same with the version bumped, the signed mirror trees), or -R, a
#    directory it already wrote.  -O passes it an old dist to reuse.
# 2. The mirror: tests/updater/mirror.py on -H (default AMINETXDUO_PEER),
#    plain HTTP.  A PEER, not this host: a bridged guest cannot reach the
#    machine running its emulator.  Checked from here before any boot: it
#    serves the good manifest byte for byte and the drop tree cuts a
#    container short.
# 3. install/test/run-workbench.sh -Y with the old archive, once per
#    Workbench the selected rows name (3.1, and 3.2 with its own Kickstart),
#    bridged, a fresh MAC per run.
#
# Prints the arm's lines and
#
#   mirror=up address=<a> port=<n>  mirror_serves=yes|no  mirror_drop=yes|no
#   scenario=<id> result=PASS|FAIL|XFAIL|XPASS reason=<key> wall=<s> ...
#   summary scenarios=<n> pass=<n> fail=<n> xfail=<n> xpass=<n>
#
# Exit 0 with no FAIL and no XPASS, 1 otherwise, 2 an ingredient is missing.
#
# SPDX-License-Identifier: MIT

set -euo pipefail
export LC_ALL=C

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
SEL=all
PEER="${AMINETXDUO_UPDATE_PEER:-${AMINETXDUO_PEER:-}}"
OLD_DIST=""
REL=""
PORT="${AMINETXDUO_UPDATE_PORT:-8480}"
while getopts 'Y:H:O:R:p:' opt; do
    case "$opt" in
        Y) SEL="$OPTARG" ;;
        H) PEER="$OPTARG" ;;
        O) OLD_DIST="$OPTARG" ;;
        R) REL="$OPTARG" ;;
        p) PORT="$OPTARG" ;;
        *) echo "result=refused reason=usage"; exit 2 ;;
    esac
done

kv() { printf '%s=%s\n' "$1" "$2"; }
refuse() { kv reason "$1"; kv result refused; exit 2; }

[ -n "$PEER" ] || refuse "no_peer:set_AMINETXDUO_PEER_or_-H"
IFACE="${AMINETXDUO_EMU_BACKEND:-${AMINETXDUO_AMIBERRY_BACKEND:-ens18}}"
case "$IFACE" in slirp*|none) refuse "bridged_only:$IFACE" ;; esac

# The lab's ingredients, where this host keeps them, unless named.
_def() { # var path
    [ -n "${!1:-}" ] || [ ! -e "$2" ] || export "$1=$2"
}
_tc=$HOME/.cache/aminetxduo/toolchain/current
_def AMIGA_GCC "$_tc/bin/m68k-amigaos-gcc"
_def AMIGA_NDK "$_tc/m68k-amigaos/ndk-include"
_def AMINETXDUO_ADF_DIR "$HOME/amiga-assets/adf-wb31"
_def AMINETXDUO_KICKSTART "$HOME/amiga-assets/roms/Kickstart v3.1 r40.68 (1993)(Commodore)(A1200)[!].rom"
_def AMINETXDUO_KICKSTART_A1200_32 "$HOME/amiga-assets/os32/rom/kicka1200.rom"
_def AMINETXDUO_XDFTOOL "$HOME/venv-amitools/bin/xdftool"
_def AMIBERRY "$HOME/amiberry/build/amiberry"
_def AMINETXDUO_A2065 "$HOME/amiga-assets/devs/a2065.device"

RUN="u$(date +%s)$$"
export AMINETXDUO_UPDATE_RUN="$RUN"

# ------------------------------------------------------------ releases --
if [ -z "$REL" ]; then
    REL="$ROOT/build/updscn/releases"
    "$ROOT/tests/updater/make-releases.sh" ${OLD_DIST:+-O "$OLD_DIST"} -o "$REL" |
        grep -E '^(releases|old_version|new_version)=' || true
fi
[ -f "$REL/releases.env" ] || refuse "no_releases:$REL"
# shellcheck source=/dev/null
. "$REL/releases.env"

# -------------------------------------------------------------- mirror --
PADDR=$(ssh -o BatchMode=yes -o ConnectTimeout=10 "$PEER" \
            "ip -o -4 addr show scope global | awk '{split(\$4,a,\"/\"); print a[1]}'" \
            2>/dev/null | head -1)
[ -n "$PADDR" ] || refuse "peer_has_no_address:$PEER"
ip -o route get "$PADDR" 2>/dev/null | grep -q "dev $IFACE " ||
    refuse "peer_not_on_segment:$PADDR"
PDIR="ai/updscn-mirror-$RUN"
PLOG="$PDIR/requests.log"
# [m]: the pattern must not match the ssh shell that runs pkill.
KILLPAT="[m]irror.py --root $PDIR/"
stop_mirror() {
    ssh -o BatchMode=yes "$PEER" "pkill -f '$KILLPAT' ; rm -rf '$PDIR'" \
        >/dev/null 2>&1 || true
}
trap stop_mirror EXIT
ssh -o BatchMode=yes "$PEER" "mkdir -p '$PDIR/tree'" || refuse "peer_unreachable:$PEER"
tar -C "$REL/mirror" -cf - . | ssh -o BatchMode=yes "$PEER" "tar -C '$PDIR/tree' -xf -" ||
    refuse "mirror_stage_failed"
scp -q "$ROOT/tests/updater/mirror.py" "$PEER:$PDIR/mirror.py" || refuse "mirror_stage_failed"
ssh -o BatchMode=yes "$PEER" "nohup python3 $PDIR/mirror.py --root $PDIR/tree --bind $PADDR \
    --port $PORT --log $PLOG </dev/null >$PDIR/stdout.log 2>&1 & sleep 1" >/dev/null 2>&1 || true
for _ in 1 2 3 4 5 6 7 8 9 10; do
    ssh -o BatchMode=yes "$PEER" "grep -q '^ready' $PDIR/stdout.log" 2>/dev/null && break
    sleep 1
done
ssh -o BatchMode=yes "$PEER" "grep -q '^ready' $PDIR/stdout.log" 2>/dev/null ||
    refuse "mirror_did_not_start"
echo "mirror=up address=$PADDR port=$PORT peer=$PEER"
URL="http://$PADDR:$PORT"

# Served byte for byte, and the drop tree cut short -- from this host, which
# reaches the peer the ordinary way.
_m="$REL/mirror/good/$NEW_CHANNEL/update.manifest"
_got=$(curl -s --max-time 10 "$URL/t/$RUN-host/good/$NEW_CHANNEL/update.manifest" | sha256sum | cut -d' ' -f1)
[ "$_got" = "$(sha256sum "$_m" | cut -d' ' -f1)" ] && kv mirror_serves yes || {
    kv mirror_serves no; refuse mirror_does_not_serve; }
_c=$(ls "$REL/mirror/drop/$NEW_CHANNEL"/*-full.update.lha)
# curl exits 18 on the short body, which is the point.
_n=$( { curl -s --max-time 20 "$URL/t/$RUN-host/drop/$NEW_CHANNEL/${_c##*/}" || true; } |
      wc -c | tr -d ' ')
[ "$_n" -lt "$(wc -c < "$_c")" ] && [ "$_n" -gt 0 ] && kv mirror_drop "yes sent=$_n of=$(wc -c < "$_c")" || {
    kv mirror_drop no; refuse mirror_drop_not_short; }

export AMINETXDUO_UPDATE_RELEASES="$REL"
export AMINETXDUO_UPDATE_MIRROR="$URL"
export AMINETXDUO_UPDATE_PEER="$PEER"
export AMINETXDUO_UPDATE_PEER_LOG="$PLOG"
export AMINETXDUO_MAC_PER_RUN=1

# ------------------------------------------------------------ the arms --
oss=$(awk -F'\t' -v sel=",$SEL," '!/^#/ && $1 != "id" && NF > 1 &&
          (sel == ",all," || index(sel, "," $1 ",")) { print $2 }' \
          "$ROOT/tests/updater/scenarios.tsv" | sort -u)
[ -n "$oss" ] || refuse "no_rows:$SEL"
LOGS=()
rc_all=0
for os in $oss; do
    LOG="$ROOT/build/updscn/run-$RUN-wb$os.log"
    mkdir -p "${LOG%/*}"
    LOGS+=("$LOG")
    set +o pipefail
    AMINETXDUO_WB="$os" AMINETXDUO_RUN_TAG="updscn${os//./}" \
        "$ROOT/install/test/run-workbench.sh" -Y "$SEL" -a "$OLD_ARCHIVE" 2>&1 |
        tee "$LOG" |
        grep --line-buffered -E '^(base|setup|check|unchanged|scenario|scenarios|updater_scenarios)='
    rc=${PIPESTATUS[0]}
    set -o pipefail
    kv "arm_wb$os" "rc=$rc"
    [ "$rc" = 0 ] || [ "$rc_all" = 2 ] || rc_all=1
    [ "$rc" != 2 ] || rc_all=2
done

# One summary over every arm.
cat "${LOGS[@]}" | awk '/^scenario=/ {
        for (i = 1; i <= NF; i++) if ($i ~ /^result=/) r[substr($i, 8)]++
        n++ }
     END { printf "summary scenarios=%d pass=%d fail=%d xfail=%d xpass=%d\n",
                  n, r["PASS"], r["FAIL"], r["XFAIL"], r["XPASS"] }'
kv logs "${LOGS[*]}"
exit "$rc_all"
