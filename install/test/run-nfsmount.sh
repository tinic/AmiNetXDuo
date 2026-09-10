#!/usr/bin/env bash
#
# MOUNT AN NFS EXPORT WITH THE HANDLER THE REPORTS ARE ABOUT.
#
#   install/test/run-nfsmount.sh -P PEERHOST [-m MODEL] [-B backend]
#                                [-t seconds] [-T tag] [-k]
#
# tests/tools/run-nfsprobe.sh proves OUR calls are right: it builds the RPC
# itself, so it can only ever ask what we thought to ask.  This runs Carsten
# Heyl's ch_nfsc 1.02BETA -- the handler bifat mounts with, and the one whose
# failures opened the 0.26.5 reports -- against a real export, and reads a file
# back through the AmigaDOS filesystem layer.
#
# WHAT IT LEANS ON, and why it is the interesting test:
#
#   usergroup.library   ch_nfsmount looks the USER up in AmiTCP:db/passwd and
#                       builds the AUTH_UNIX credentials from it.  That file is
#                       AmiTCP 4's own PIPE-delimited format, which
#                       src/usergroup/ug_parse.c learned to read in 0.26.6.  A
#                       stack that cannot parse it mounts as nobody.
#   bsdsocket.library   RPC over UDP from a reserved port, through the peer's
#                       real rpcbind.
#
# THE SERVER IS REAL AND USERSPACE.  The peer is a container: nfs-kernel-server
# will not start and `modprobe nfsd` wants a password.  tests/tools/nfsserver.py
# serves a directory on unprivileged ports and REGISTERS with the peer's
# running rpcbind, which answers ch_nfsc's GETPORT.
#
# Exit 0 mounted and the file's bytes matched, 1 not, 2 an ingredient missing.
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD="${AMINETXDUO_BUILD:-build/cm}"

MODEL=A1200
TIMEOUT=420
BACKEND="${AMINETXDUO_EMU_BACKEND:-ens18}"
TAG="${AMINETXDUO_RUN_TAG:-nfsmount}"
PEERHOST="${AMINETXDUO_FITZ_PEER:-}"
KEEP=0
EXPORT_NAME="/export"
CONTENT='AmiNetXDuo NFS payload, 0123456789'
NFSUSER=ch
NFSUID=1000
NFSGID=100
ADDRESS="${AMINETXDUO_NFSMOUNT_ADDRESS:-192.168.1.237}"
GATEWAY="${AMINETXDUO_NFSMOUNT_GATEWAY:-192.168.1.1}"

while getopts "m:B:t:T:P:b:kh" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        B) BACKEND="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        T) TAG="$OPTARG" ;;
        P) PEERHOST="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        k) KEEP=1 ;;
        h) sed -n '3,7p' "$0"; exit 0 ;;
        *) sed -n '3,7p' "$0" >&2; exit 2 ;;
    esac
done

case "$BUILD" in /*) ;; *) BUILD="$ROOT/${BUILD#./}" ;; esac

need() { [ -e "$1" ] || { echo "!! missing $1${2:+ -- $2}" >&2; exit 2; }; }

[ -n "$PEERHOST" ] || {
    echo "-P is required: the NFS server must be on a THIRD machine; the host" >&2
    echo "running amiberry cannot be reached by its own bridged guest." >&2
    exit 2; }

# ------------------------------------------------------------- ingredients ---

CHNFS="${AMINETXDUO_CHNFSC:-$HOME/amiga-assets/apps/chnfsc-1.02beta}"
need "$CHNFS/bin/ch_nfsc"     "the handler; fetch comm/net/chnfsc102-30b2.lha into the asset store"
need "$CHNFS/bin/ch_nfsmount" "same archive"

LIBBSD="$BUILD/src/bsdsocket/bsdsocket.library"
LIBUG="$BUILD/src/usergroup/usergroup.library"
CMDDIR="$BUILD/src/tools"
need "$LIBBSD" "build the tree first"
need "$LIBUG"  "build the tree first"
need "$CMDDIR/AddNetInterface"
need "$CMDDIR/ToolsSmoke"

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for c in "$ROOT/build/a2065.device" "$HOME/amiga-assets/devs/a2065.device"; do
        [ -f "$c" ] && { A2065="$c"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "!! no a2065.device; set AMINETXDUO_A2065" >&2; exit 2; }

[ -n "${AMINETXDUO_KICKSTART:-}" ] || {
    echo "!! no Kickstart; set AMINETXDUO_KICKSTART" >&2; exit 2; }

# The three AmigaDOS commands this needs.  Taken from whichever tree on this
# machine has them: the assembled Workbench that install/test/run-smbmount.sh
# builds, or the asset store's own OS trees.  Only Assign, List and Type are
# wanted, so requiring a full Workbench assembly would refuse a machine that
# can plainly run the test.
find_cmd() {
    local name="$1" d
    for d in "$ROOT/build/wb31-sys/C" \
             "$HOME/amiga-assets/os32/Workbench/C" \
             "$HOME/amiga-assets/classicwb/snapshots/full/tree/C" \
             "$HOME/amiga-assets/wb/C"; do
        [ -f "$d/$name" ] && { printf '%s' "$d/$name"; return 0; }
    done
    return 1
}
DOSCMDS=""
for c in Assign List Type; do
    f=$(find_cmd "$c") || {
        echo "!! no AmigaDOS '$c' on this machine.  Assemble a Workbench" >&2
        echo "   (install/test/run-smbmount.sh does, from the 3.1 ADFs) or" >&2
        echo "   point AMINETXDUO_ADF_DIR at them." >&2
        exit 2; }
    DOSCMDS="$DOSCMDS $f"
done

PEERNAME="${PEERHOST#*@}"
PEERADDR=$(getent ahostsv4 "$PEERNAME" 2>/dev/null | awk 'NR==1{print $1}')
[ -n "$PEERADDR" ] || PEERADDR="$PEERNAME"

# ------------------------------------------------------------------ staging --

STAGE="$ROOT/build/nfsmount-stage-$TAG"
OUT="$ROOT/build/nfsmount-$TAG"
HD="$ROOT/build/amiberry-testhd-$TAG"
REPORT="$HD/tools.txt"
rm -rf "$STAGE" "$OUT"; mkdir -p "$OUT" "$STAGE/c" "$STAGE/libs" \
        "$STAGE/devs/NetInterfaces" "$STAGE/AmiTCP/bin" "$STAGE/AmiTCP/db" \
        "$STAGE/AmiTCP/libs"

for f in $DOSCMDS; do cp "$f" "$STAGE/c/"; done
cp "$A2065"  "$STAGE/devs/a2065.device"
cp "$LIBBSD" "$STAGE/libs/bsdsocket.library"
cp "$LIBUG"  "$STAGE/libs/usergroup.library"
# bifat's report says ch_nfsmount opens AmiTCP:libs/usergroup.library by path,
# so it goes in both places rather than one.
cp "$LIBUG"  "$STAGE/AmiTCP/libs/usergroup.library"
cp "$CMDDIR/AddNetInterface" "$STAGE/AddNetInterface"
cp "$CHNFS/bin/ch_nfsc"      "$STAGE/AmiTCP/bin/ch_nfsc"
cp "$CHNFS/bin/ch_nfsmount"  "$STAGE/c/ch_nfsmount"
chmod 755 "$STAGE/c/"* "$STAGE/AmiTCP/bin/"* "$STAGE/AddNetInterface" \
          "$STAGE/devs/a2065.device" 2>/dev/null || true

# AmiTCP 4's own database format: '|' between fields, not ':'.  This is the
# file src/usergroup/ug_parse.c learned to read in 0.26.6, and mounting as the
# named user is what proves it did.
cat > "$STAGE/AmiTCP/db/passwd" <<PWEOF
root||0|0|Superuser|SYS:|
$NFSUSER||$NFSUID|$NFSGID|NFS test user|SYS:|
PWEOF
cat > "$STAGE/AmiTCP/db/group" <<GREOF
wheel||0|root
users||$NFSGID|$NFSUSER
GREOF
cat > "$STAGE/AmiTCP/db/ch_nfstab" <<TABEOF
$PEERADDR:$EXPORT_NAME NFS: USER $NFSUSER UMASK 022
TABEOF

cat > "$STAGE/devs/NetInterfaces/eth0" <<IFEOF
DEVICE=a2065.device
UNIT=0
CONFIGURE=STATIC
ADDRESS=$ADDRESS
NETMASK=255.255.255.0
GATEWAY=$GATEWAY
IFEOF

# Each line is one command and ToolsSmoke records its rc, so a failure names
# the step rather than the run.
cat > "$STAGE/commands.txt" <<CMDEOF
SYS:c/Assign AmiTCP: SYS:AmiTCP
SYS:AddNetInterface eth0
SYS:c/ch_nfsmount NFS: from AmiTCP:db/ch_nfstab
SYS:c/List NFS:
SYS:c/Type NFS:payload.txt
CMDEOF

# --------------------------------------------------------------------- run ---

echo "==> booting $MODEL, a2065 bridged on $BACKEND, guest static at $ADDRESS"
echo "==> mounting NFS: as $NFSUSER (uid $NFSUID) with ch_nfsc 1.02BETA"
set +e
"$ROOT/tools/amiberry-run.sh" -m "$MODEL" -N a2065 -B "$BACKEND" \
    -t "$TIMEOUT" \
    "$CMDDIR/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/c" "$STAGE/libs" \
    "$STAGE/devs" "$STAGE/AmiTCP" "$STAGE/AddNetInterface"
RUN_RC=$?
set -e

# --------------------------------------------------------------- verdict -----
#
# ToolsSmoke prints a header per command and closes it with its rc, so each
# step is read back by name.  A step that never ran has no block at all, which
# is a different fact from one that ran and failed.

step_rc() {   # step_rc <substring of the command line>
    awk -v want="$1" '
        index($0, "===== ") == 1 && index($0, want) > 0 { on = 1; next }
        on && /^----- rc / { sub(/^----- rc /, ""); sub(/,.*/, ""); print; exit }
    ' "$REPORT" 2>/dev/null
}
step_out() {  # everything the named command printed
    awk -v want="$1" '
        index($0, "===== ") == 1 && index($0, want) > 0 { on = 1; next }
        on && /^----- rc / { exit }
        on { print }
    ' "$REPORT" 2>/dev/null
}

ASSIGN_RC=none; IFACE_RC=none; MOUNT_RC=none; LIST_RC=none; TYPE_RC=none
GOT=""
if [ -f "$REPORT" ]; then
    cp "$REPORT" "$OUT/tools.txt"
    echo
    echo "===================== what the guest printed ======================"
    cat "$REPORT"
    echo "==================================================================="
    ASSIGN_RC=$(step_rc "Assign AmiTCP:");        ASSIGN_RC="${ASSIGN_RC:-none}"
    IFACE_RC=$(step_rc "AddNetInterface eth0");   IFACE_RC="${IFACE_RC:-none}"
    MOUNT_RC=$(step_rc "ch_nfsmount");            MOUNT_RC="${MOUNT_RC:-none}"
    LIST_RC=$(step_rc "List NFS:");               LIST_RC="${LIST_RC:-none}"
    TYPE_RC=$(step_rc "Type NFS:payload.txt");    TYPE_RC="${TYPE_RC:-none}"
    GOT=$(step_out "Type NFS:payload.txt" | tr -d '\r' | sed '/^$/d' | head -1)
fi

MATCH=no
[ "$GOT" = "$CONTENT" ] && MATCH=yes

stop_server
trap - EXIT INT TERM HUP

SRV_MNT=$(sed -n 's/^mnt_path=//p' "$OUT/server.log" 2>/dev/null | head -1)
SRV_CRED=$(sed -n 's/^cred_flavour=//p' "$OUT/server.log" 2>/dev/null | head -1)
SRV_COUNTS=$(sed -n 's/^nfsserver_counts=//p' "$OUT/server.log" 2>/dev/null | head -1)

STATUS=fail
[ "$MOUNT_RC" = 0 ] && [ "$MATCH" = yes ] && STATUS=pass

printf 'nfsmount: status=%s run_rc=%s assign_rc=%s iface_rc=%s mount_rc=%s list_rc=%s type_rc=%s content_match=%s out=%s\n' \
       "$STATUS" "$RUN_RC" "$ASSIGN_RC" "$IFACE_RC" "$MOUNT_RC" "$LIST_RC" \
       "$TYPE_RC" "$MATCH" "$OUT"
[ "$MATCH" = no ] && [ -n "$GOT" ] && printf 'nfsmount: read back %s\n' "$(printf '%s' "$GOT" | head -c 80)"
[ -n "$SRV_MNT" ]  && printf 'nfsmount: server saw mnt_path=%s\n' "$SRV_MNT"
[ -n "$SRV_CRED" ] && printf 'nfsmount: server saw %s\n' "$SRV_CRED"
[ -n "$SRV_COUNTS" ] && printf 'nfsmount: server counts %s\n' "$SRV_COUNTS"

[ "$KEEP" = 1 ] || rm -rf "$STAGE"
[ "$STATUS" = pass ] || exit 1
exit 0
