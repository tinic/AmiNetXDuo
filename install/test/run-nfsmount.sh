#!/usr/bin/env bash
#
# MOUNT AN NFS EXPORT WITH THE HANDLER THE REPORTS ARE ABOUT, ON A REAL
# WORKBENCH.
#
#   install/test/run-nfsmount.sh -P PEERHOST [-m MODEL] [-B backend]
#                                [-t seconds] [-T tag] [-b BUILDDIR] [-k]
#
# tests/tools/run-nfsprobe.sh proves OUR calls are right: it builds the RPC
# itself, so it can only ever ask what we thought to ask.  This runs Carsten
# Heyl's ch_nfsc 1.02BETA -- the handler bifat mounts with, and the one whose
# failures opened the 0.26.5 reports -- against a real export, and reads a file
# back through the AmigaDOS filesystem layer.
#
# WHY A WHOLE WORKBENCH AND NOT ToolsSmoke.  ch_nfsc inspects its own CLI's
# cli_DefaultStack and refuses under 30,000 bytes.  `Stack` is a Shell
# BUILT-IN: it sets the cli_DefaultStack of the Shell that runs it, and a
# child started with `Run` from that Shell inherits it.  Nothing a parent
# PROCESS sets reaches a grandchild CLI, which is why ToolsSmoke's NP_StackSize
# and its own cli_DefaultStack both left ch_nfsc reading DOS's ~4 KB default
# and dying after it had already created the NFS: entry -- a `List NFS:` then
# blocks on a handler that is gone.  So this assembles Workbench 3.1 the way
# install/test/run-smbmount.sh does, boots it, and drives a real Shell.
#
# WHAT IT LEANS ON, and why it is the interesting test:
#
#   usergroup.library   ch_nfsmount looks the USER up in AmiTCP:db/passwd and
#                       builds the AUTH_UNIX credentials from it.  That file is
#                       AmiTCP 4's own PIPE-delimited format, which
#                       src/usergroup/ug_parse.c learned to read in 0.26.6
#                       (e93e8846).  A stack that cannot parse it mounts as
#                       nobody.
#   bsdsocket.library   RPC over UDP from a reserved port, through the peer's
#                       real rpcbind, and WaitSelect over an AmiTCP fd_set --
#                       64 descriptors wide, which is what 24a92828 made
#                       getdtablesize() answer.
#
# THE SERVER IS REAL AND USERSPACE.  The peer is a container: nfs-kernel-server
# will not start and `modprobe nfsd` wants a password.  tests/tools/nfsserver.py
# serves a directory on unprivileged ports and REGISTERS with the peer's
# running rpcbind, which answers ch_nfsc's GETPORT.
#
# NOTHING HANGS THE HARNESS.  Every step writes its own marker with its own
# `Echo >file`, because a process that hangs never flushes a file it left open,
# and `List`/`Type` -- the two that block on a dead handler -- run DETACHED so
# the boot Shell still reaches the end and says so.
#
# THE VERDICT IS THREE FACTS, not one: the mount appeared, the file's bytes
# came back byte for byte, and the peer saw the USER's uid on the NFS calls.
# A handler that fell back to nobody would satisfy the first two.
#
# Exit 0 all three, 1 not, 2 an ingredient missing.
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
MAC="${AMINETXDUO_EMU_MAC:-}"
# Seconds the boot Shell gives ch_nfsc to answer the mount before it looks,
# and then the detached half to finish before it declares the run over.
MOUNTWAIT=12
READWAIT=45

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
export AMINETXDUO_RUN_TAG="$TAG"

# One MAC per tag rather than a pinned address: a fixed one puts every run of
# every arm on the bridge under the same hardware address, beside other
# checkouts' guests, and a peer's neighbour cache then keeps whichever
# answered last.
# shellcheck source=../../tools/emu-mac.sh
. "$ROOT/tools/emu-mac.sh"
[ -n "$MAC" ] || MAC=$(emu_mac_for_tag "$TAG")

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

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for c in "$ROOT/build/a2065.device" "$HOME/amiga-assets/devs/a2065.device"; do
        [ -f "$c" ] && { A2065="$c"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "!! no a2065.device; set AMINETXDUO_A2065" >&2; exit 2; }

KICKSTART="${AMINETXDUO_KICKSTART:-}"
eval "KICKSTART=\${AMINETXDUO_KICKSTART_$MODEL:-\$KICKSTART}"
[ -n "$KICKSTART" ] && [ -f "$KICKSTART" ] || {
    echo "!! no Kickstart for $MODEL; set AMINETXDUO_KICKSTART" >&2; exit 2; }

AMIBERRY="${AMIBERRY:-$(command -v amiberry || true)}"
[ -n "$AMIBERRY" ] || for c in "$HOME/amiberry/build/amiberry" "$HOME/amiberry/amiberry"; do
    [ -x "$c" ] && { AMIBERRY="$c"; break; }
done
[ -n "$AMIBERRY" ] || { echo "!! amiberry not found; set AMIBERRY=<path>" >&2; exit 2; }

PEERNAME="${PEERHOST#*@}"
PEERADDR=$(getent ahostsv4 "$PEERNAME" 2>/dev/null | awk 'NR==1{print $1}')
[ -n "$PEERADDR" ] || PEERADDR="$PEERNAME"

OUT="$ROOT/build/nfsmount-$TAG"
HD="$ROOT/build/nfshd-$TAG"
rm -rf "$OUT"; mkdir -p "$OUT"

echo "==> $MODEL, OS 3.1, $(basename "$KICKSTART")"

# --------------------------------------------------------------- the SYS: ---
#
# The same five ADFs install/test/run-smbmount.sh assembles, through the
# shared helper so both harnesses get one tree and one staleness rule.

WB="$ROOT/build/wb31-sys"
# shellcheck source=../../tests/tools/wb31-sys.sh
. "$ROOT/tests/tools/wb31-sys.sh"
wb31_assemble "$WB" || exit 2

# `Run`, `Echo`, `FailAt` and `Stack` are the Shell's own built-ins in
# AmigaDOS 2.0 and later, not files, so only the disk commands are looked for.
for want in C/Assign C/Execute C/List C/Type C/Wait S/Startup-Sequence; do
    [ -e "$WB/$want" ] || { echo "!! the assembled SYS: has no $want" >&2; exit 2; }
done

# --------------------------------------------------------------- the drive --

rm -rf "$HD"; mkdir -p "$HD"
cp -R "$WB/." "$HD/"
mkdir -p "$HD/C" "$HD/Libs" "$HD/S" "$HD/Devs/NetInterfaces" \
         "$HD/AmiTCP/bin" "$HD/AmiTCP/db" "$HD/AmiTCP/libs"

cp "$A2065"  "$HD/Devs/a2065.device"
cp "$LIBBSD" "$HD/Libs/bsdsocket.library"
cp "$LIBUG"  "$HD/Libs/usergroup.library"
# bifat's report says ch_nfsmount opens AmiTCP:libs/usergroup.library by path,
# so it goes in both places rather than one.
cp "$LIBUG"  "$HD/AmiTCP/libs/usergroup.library"
cp "$CHNFS/bin/ch_nfsc"     "$HD/AmiTCP/bin/ch_nfsc"
cp "$CHNFS/bin/ch_nfsmount" "$HD/C/ch_nfsmount"
cp "$CMDDIR/AddNetInterface" "$HD/C/AddNetInterface"
# Diagnostics, if this build has them: an interface that never came up and a
# server that never answered are different failures and should not read alike.
HAVE_PING=no
for t in ShowNetStatus ping netstat; do
    [ -f "$CMDDIR/$t" ] || continue
    cp "$CMDDIR/$t" "$HD/C/$t"
    [ "$t" = ping ] && HAVE_PING=yes
done
chmod 755 "$HD/C/"* "$HD/AmiTCP/bin/"* "$HD/Devs/a2065.device" \
          "$HD/Libs/bsdsocket.library" "$HD/Libs/usergroup.library" \
          "$HD/AmiTCP/libs/usergroup.library" 2>/dev/null || true

# AmiTCP 4's own database format: '|' between fields, not ':'.  This is the
# file src/usergroup/ug_parse.c learned to read in e93e8846, and mounting as
# the named user is what proves it did.
cat > "$HD/AmiTCP/db/passwd" <<PWEOF
root||0|0|Superuser|SYS:|
$NFSUSER||$NFSUID|$NFSGID|NFS test user|SYS:|
PWEOF
cat > "$HD/AmiTCP/db/group" <<GREOF
wheel||0|root
users||$NFSGID|$NFSUSER
GREOF
cat > "$HD/AmiTCP/db/ch_nfstab" <<TABEOF
$PEERADDR:$EXPORT_NAME NFS: USER $NFSUSER UMASK 022
TABEOF

cat > "$HD/Devs/NetInterfaces/eth0" <<IFEOF
DEVICE=a2065.device
UNIT=0
CONFIGURE=STATIC
ADDRESS=$ADDRESS
NETMASK=255.255.255.0
GATEWAY=$GATEWAY
IFEOF

# ------------------------------------------------------------ the scripts ---
#
# The detached half.  `List NFS:` and `Type NFS:payload.txt` are the two calls
# that block forever on a device node whose handler died, so they run here and
# not in the boot Shell: a step that never returned is then a marker that is
# not there, rather than a run that timed out with nothing to read.

cat > "$HD/S/NFS-Read" <<'READEOF'
FailAt 9999
C:List NFS: >DH0:r1-list-out.txt
Echo >DH0:r2-list-rc.txt "$RC"
C:Type NFS:payload.txt >DH0:r3-type-out.txt
Echo >DH0:r4-type-rc.txt "$RC"
Echo >DH0:r5-end.txt "done"
READEOF

# The boot Shell's half.
#
# THE STACK ch_nfsc READS IS ITS OWN CLI'S.  `Stack 65536` here sets THIS
# Shell's cli_DefaultStack, and the `Run` below hands it to ch_nfsc's CLI.
# That is the whole reason this harness boots a Workbench.
#
# `Run AmiTCP:bin/ch_nfsc` AND NOT `ch_nfsmount NFS:`, deliberately:
# ch_nfsmount reads the table and then starts the handler ITSELF, and that
# child does not get this Shell's cli_DefaultStack -- so ch_nfsc would still
# read ~4 KB, refuse, and leave an NFS: entry with no handler behind it.
#
# ch_nfsmount LIST stays, because it is what proves the two things this test
# exists for: that AmiTCP:db/ch_nfstab parses, and that the USER in it was
# looked up in AmiTCP:db/passwd -- the pipe-delimited AmiTCP 4 database
# src/usergroup/ug_parse.c learned to read.
#
# ch_nfsc's own console output goes to a file rather than NIL:, because when
# it refuses that message is the only place the reason appears.  On a mount
# that WORKS the handler stays resident and never closes it, so an empty
# r0/m6 file is the good case, not a missing one.
{
    echo 'FailAt 9999'
    echo 'Stack 65536'
    echo 'Echo >DH0:m0-start.txt "begin"'
    echo 'C:Assign AmiTCP: SYS:AmiTCP'
    echo 'Echo >DH0:m1-assign-rc.txt "$RC"'
    echo 'C:AddNetInterface eth0 >DH0:m2-addnet-out.txt'
    echo 'Echo >DH0:m3-addnet-rc.txt "$RC"'
    [ -f "$HD/C/ShowNetStatus" ] && echo 'C:ShowNetStatus >DH0:m3b-netstatus.txt'
    if [ "$HAVE_PING" = yes ]; then
        echo "C:ping $PEERADDR -c 2 -t 10 >DH0:m3c-ping-out.txt"
        echo 'Echo >DH0:m3d-ping-rc.txt "$RC"'
    fi
    echo 'C:ch_nfsmount LIST >DH0:m4-nfstab-out.txt'
    echo 'Echo >DH0:m5-nfstab-rc.txt "$RC"'
    echo "Run >DH0:m6-chnfsc-out.txt <NIL: AmiTCP:bin/ch_nfsc $PEERADDR:$EXPORT_NAME NFS: USER $NFSUSER UMASK 022"
    echo 'Echo >DH0:m7-run-rc.txt "$RC"'
    echo "C:Wait $MOUNTWAIT"
    echo 'Run >NIL: <NIL: C:Execute S:NFS-Read'
    echo 'Echo >DH0:m8-read-started.txt "$RC"'
    echo "C:Wait $READWAIT"
    echo 'Echo >DH0:m9-end.txt "done"'
} > "$HD/S/NFS-Mount"
chmod 755 "$HD/S/NFS-Mount" "$HD/S/NFS-Read"

# The stock Startup-Sequence with its tail replaced.  `EndCLI` would take the
# boot Shell away before any of this ran.
SS=$(find "$HD/S" -maxdepth 1 -iname 'startup-sequence' | head -1)
[ -n "$SS" ] || { echo "!! no S/Startup-Sequence on the drive" >&2; exit 2; }
sed -e '/^EndCLI/d' -e '/^ *EndShell/d' "$SS" > "$SS.new"
cat >> "$SS.new" <<'SSEOF'

FailAt 9999
Execute S:NFS-Mount >DH0:boot-console.txt
Echo >DH0:.done "$RC"
SSEOF
mv "$SS.new" "$SS"
chmod 755 "$SS"

# ------------------------------------------------------------- the server ----
#
# By PID from a file the peer writes, never `pkill -f`: the pattern would match
# the very shell that starts it.  The `timeout` on the far side is the other
# half -- killing the local ssh does not kill what it started there.

RSRV="/tmp/nfsserver-$TAG.py"
RROOT="/tmp/nfsroot-$TAG"
RLOG="/tmp/nfsserver-$TAG.log"
RPID="/tmp/nfsserver-$TAG.pid"

stop_server() {
    scp -q "$PEERHOST:$RLOG" "$OUT/server.log" 2>/dev/null || true
    # The log goes too, once it is here: it is copied back on the line above,
    # and a peer that keeps one per tag accumulates them forever.
    ssh -o BatchMode=yes -o ConnectTimeout=10 -n "$PEERHOST" \
        "[ -f $RPID ] && kill \$(cat $RPID) 2>/dev/null; \
         rm -rf $RSRV $RPID $RROOT $RLOG; exit 0" >/dev/null 2>&1 || true
    return 0
}
trap stop_server EXIT INT TERM HUP

scp -q "$ROOT/tests/tools/nfsserver.py" "$PEERHOST:$RSRV" || {
    echo "!! cannot copy the server to $PEERHOST" >&2; exit 2; }

ssh -o BatchMode=yes -o ConnectTimeout=10 -n "$PEERHOST" \
    "rm -rf $RROOT && mkdir -p $RROOT && \
     printf '%s\\n' '$CONTENT' > $RROOT/payload.txt && chmod -R a+rX $RROOT" || {
    echo "!! cannot stage the export on $PEERHOST" >&2; exit 2; }

ssh -o BatchMode=yes -o ConnectTimeout=10 -n "$PEERHOST" \
    "nohup timeout $((TIMEOUT + 120)) python3 $RSRV --root $RROOT \
         --export $EXPORT_NAME --seconds $((TIMEOUT + 60)) \
         > $RLOG 2>&1 & echo \$! > $RPID" >/dev/null 2>&1 || {
    echo "!! cannot start the server on $PEERHOST" >&2; exit 2; }

for _ in $(seq 1 20); do
    ssh -o BatchMode=yes -o ConnectTimeout=10 -n "$PEERHOST" \
        "grep -q '^rpcbind_registered=' $RLOG" 2>/dev/null && break
    sleep 1
done
REG=$(ssh -o BatchMode=yes -o ConnectTimeout=10 -n "$PEERHOST" \
      "sed -n 's/^rpcbind_registered=//p' $RLOG" 2>/dev/null || true)
[ -n "$REG" ] && [ "$REG" != none ] || {
    echo "!! the server did not register with the peer's rpcbind:" >&2
    ssh -o BatchMode=yes -n "$PEERHOST" "cat $RLOG" >&2 2>/dev/null || true
    exit 2; }
echo "==> serving $RROOT as $EXPORT_NAME from $PEERADDR (rpcbind: $REG)"

# ------------------------------------------------------------- the machine --

export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-dummy}"
export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-dummy}"
[ "$SDL_VIDEODRIVER" = dummy ] && unset DISPLAY WAYLAND_DISPLAY || true

CFG="$ROOT/build/nfsmount-$TAG.uae"
SERIAL="$OUT/serial.log"
# ALLOCATED, not hashed: a hashed slot can collide with an unrelated arm in
# another checkout and the two guests then read each other's console.
# shellcheck source=../../tools/emu-rig-lock.sh
. "$ROOT/tools/emu-rig-lock.sh"
rig_claim_port "run-nfsmount $TAG" || exit 2
PORT="$RIG_PORT"
: > "$SERIAL"

cat > "$CFG" <<UAEEOF
config_description=AmiNetXDuo nfsmount $TAG
use_gui=no
headless=true
quickstart=$MODEL,0
kickstart_rom_file=$KICKSTART
fastmem_size=8
floppy0type=-1
nr_floppies=0
uaehf0=dir,rw,DH0:DH0:$HD,0
serial_port=tcp://127.0.0.1:$PORT/wait
a2065_rom_file=:ENABLED
a2065_rom_options=mac=$MAC,$BACKEND
UAEEOF

EMU_PID=""; SERIAL_PID=""
cleanup() {
    stop_server
    [ -n "$EMU_PID" ] && { kill -TERM "$EMU_PID" 2>/dev/null || true; sleep 1
                           kill -KILL "$EMU_PID" 2>/dev/null || true; }
    [ -n "$SERIAL_PID" ] && kill -TERM "$SERIAL_PID" 2>/dev/null || true
    EMU_PID=""; SERIAL_PID=""
}
trap cleanup EXIT INT TERM HUP

echo "==> booting, ${TIMEOUT}s budget, a2065 bridged on $BACKEND as $MAC"
echo "==> guest static at $ADDRESS, mounting NFS: as $NFSUSER (uid $NFSUID)"
( trap '' PIPE; exec "$AMIBERRY" --log -f "$CFG" ) \
    > "$OUT/amiberry.log" 2>&1 &
EMU_PID=$!
(
    for _ in $(seq 1 60); do
        kill -0 "$EMU_PID" 2>/dev/null || exit 0
        nc 127.0.0.1 "$PORT" >> "$SERIAL" 2>/dev/null && exit 0
        sleep 0.5
    done
) &
SERIAL_PID=$!

elapsed=0; BOOT_STATUS=124
while [ "$elapsed" -lt "$TIMEOUT" ]; do
    # The detached half finishing is the real end of the run; the boot Shell
    # is still sitting out its Wait when that happens, and there is no reason
    # to pay for the rest of it.
    if [ -f "$HD/r5-end.txt" ]; then
        sleep 2; BOOT_STATUS=early; break
    fi
    if [ -f "$HD/.done" ]; then
        BOOT_STATUS=$(tr -dc '0-9' < "$HD/.done" | head -c 4)
        BOOT_STATUS=${BOOT_STATUS:-0}; break
    fi
    kill -0 "$EMU_PID" 2>/dev/null || {
        echo "!! amiberry exited after ${elapsed}s" >&2; break; }
    sleep 1; elapsed=$((elapsed + 1))
done
echo "    (finished after ${elapsed}s, boot status $BOOT_STATUS)"
cleanup
trap - EXIT INT TERM HUP

# --------------------------------------------------------------- the result --

for f in "$HD"/m?-*.txt "$HD"/m??-*.txt "$HD"/r?-*.txt "$HD"/boot-console.txt; do
    [ -e "$f" ] && cp "$f" "$OUT/" 2>/dev/null || true
done

echo
echo "================= what the machine did ================="
for f in m0-start m1-assign-rc m2-addnet-out m3-addnet-rc m3b-netstatus \
         m3c-ping-out m3d-ping-rc m4-nfstab-out m5-nfstab-rc m6-chnfsc-out \
         m7-run-rc m8-read-started m9-end \
         r1-list-out r2-list-rc r3-type-out r4-type-rc r5-end; do
    if [ -e "$HD/$f.txt" ]; then
        printf -- '---- %s ----\n' "$f"
        cat "$HD/$f.txt"
    else
        printf -- '---- %s ---- NEVER WRITTEN\n' "$f"
    fi
done
if [ -s "$HD/boot-console.txt" ]; then
    echo "---- anything the boot Shell itself said ----"
    cat "$HD/boot-console.txt"
fi
echo "======================================================="

# A step that never returned wrote no rc file at all, and reading one that is
# not there must not end the run under `set -e` before the table is printed.
rcof() { [ -e "$HD/$1.txt" ] || { printf 'NEVER_RETURNED'; return 0; }
         tr -dc '0-9-' < "$HD/$1.txt" | head -c 6; }

ASSIGN_RC=$(rcof m1-assign-rc)
ADDNET_RC=$(rcof m3-addnet-rc)
NFSTAB_RC=$(rcof m5-nfstab-rc)
MOUNT_RC=$(rcof m7-run-rc)
LIST_RC=$(rcof r2-list-rc)
TYPE_RC=$(rcof r4-type-rc)

STOPPED=none
for step in "boot:m0-start" "assign:m1-assign-rc" "addnet:m3-addnet-rc" \
            "nfstab:m5-nfstab-rc" "mount:m7-run-rc" "list:r2-list-rc" \
            "type:r4-type-rc" "read:r5-end"; do
    [ -e "$HD/${step##*:}.txt" ] || { STOPPED="${step%%:*}"; break; }
done

# BY CONTENT, not by "a mount appeared": a handler that answers Examine but
# reads zeroes would list the file and pass every rc.  \r is stripped because
# an AmigaDOS console can add one; nothing else is.
MATCH=no; GOT=""; GOTBYTES=0
if [ -e "$HD/r3-type-out.txt" ]; then
    printf '%s\n' "$CONTENT" > "$OUT/expected.txt"
    tr -d '\r' < "$HD/r3-type-out.txt" > "$OUT/got.txt"
    GOTBYTES=$(wc -c < "$OUT/got.txt" | tr -d ' ')
    cmp -s "$OUT/expected.txt" "$OUT/got.txt" && MATCH=yes
    GOT=$(head -c 80 "$OUT/got.txt" | tr -d '\n')
fi

LISTED=no
grep -qi 'payload.txt' "$HD/r1-list-out.txt" 2>/dev/null && LISTED=yes

stop_server
trap - EXIT INT TERM HUP

SRV_MNT=$(sed -n 's/^mnt_path=//p' "$OUT/server.log" 2>/dev/null | head -1)
SRV_COUNTS=$(sed -n 's/^nfsserver_counts=//p' "$OUT/server.log" 2>/dev/null | head -1)
# THE CREDENTIAL ON THE FILE CALLS, not the one on the mount.  An NFS client
# sends MNT as root and the NFS calls as the user it was told to be, so the
# uid on prog=NFS is the one usergroup.library had to look up in the
# pipe-delimited AmiTCP:db/passwd -- and it is the only place on the wire
# where that lookup is visible.
SRV_UID=$(sed -n 's/^cred_flavour=.*prog=NFS .*uid=\([0-9]*\) .*/\1/p' \
          "$OUT/server.log" 2>/dev/null | head -1)
SRV_UID="${SRV_UID:-none}"
CRED_OK=no
[ "$SRV_UID" = "$NFSUID" ] && CRED_OK=yes

# Named only while it still exists: without -k the Workbench copy goes at the
# end of this block, and a path in the report that is not there reads as a
# drive somebody deleted.
DRIVE=removed
[ "$KEEP" = 1 ] && DRIVE="$HD"

STATUS=fail
[ "$MOUNT_RC" = 0 ] && [ "$LIST_RC" = 0 ] && [ "$TYPE_RC" = 0 ] \
    && [ "$LISTED" = yes ] && [ "$MATCH" = yes ] && [ "$CRED_OK" = yes ] \
    && STATUS=pass

printf 'nfsmount: status=%s stopped_at=%s boot_status=%s assign_rc=%s addnet_rc=%s nfstab_rc=%s mount_rc=%s list_rc=%s type_rc=%s listed_payload=%s content_match=%s content_bytes=%s nfs_uid=%s model=%s drive=%s out=%s\n' \
       "$STATUS" "$STOPPED" "$BOOT_STATUS" "$ASSIGN_RC" "$ADDNET_RC" \
       "$NFSTAB_RC" "$MOUNT_RC" "$LIST_RC" "$TYPE_RC" "$LISTED" "$MATCH" \
       "$GOTBYTES" "$SRV_UID" "$MODEL" "$DRIVE" "$OUT"
[ "$MATCH" = no ] && [ -n "$GOT" ] && printf 'nfsmount: read back %s\n' "$GOT"
[ -n "$SRV_MNT" ]    && printf 'nfsmount: server saw mnt_path=%s\n' "$SRV_MNT"
sed -n 's/^cred_flavour=/nfsmount: server saw cred /p' "$OUT/server.log" 2>/dev/null || true
[ -n "$SRV_COUNTS" ] && printf 'nfsmount: server counts %s\n' "$SRV_COUNTS"

[ "$KEEP" = 1 ] || rm -rf "$HD"
[ "$STATUS" = pass ] || exit 1
exit 0
