#!/usr/bin/env bash
#
# MOUNT AN SMB SHARE THE WAY A USER DOES: a real Workbench, a real Shell, a
# DOSDriver, `Mount` and `List`, over our stack.
#
#   install/test/run-smbmount.sh [-m MODEL] [-o 31|32] [-u URL] [-P PEERHOST]
#                                [-B backend] [-M mac] [-T tag] [-t seconds]
#                                [-w seconds] [-A] [-C] [-k]
#
# THE REPORTS THIS EXISTS FOR.  GitHub #4, "Mounting SMB freezes system", an
# A500 with a PiStorm on OS 3.2.2, no error printed, and the same DOSDriver
# working over AmiTCP 4.2.  GitHub #3, "opening SMB2 to the LAN never
# responds", while ping and SNTP work.  Neither is reachable from a socket
# test: tests/tools/run-smbprobe.sh already shows `nc` connecting to 445 and
# 139 from a booted A1200, so whatever fails happens above the TCP handshake,
# inside smb2-handler, and only a machine that can Mount can show it.
#
# WHY NOT ToolsSmoke.  smb2-handler is reached through a DOSDriver, and a
# DOSDriver needs `Mount`, which needs a C: with Commodore's commands in it.
# The staging drive tools/amiberry-run.sh builds has eight of our binaries and
# nothing else.  So this assembles a Workbench, the same way
# install/test/run-workbench.sh does, and boots that.
#
# NOTHING HANGS THE HARNESS.  Mount, List and Info run in a DETACHED process,
# each step writing its own file before and after, so the boot shell always
# reaches the end and always writes DH0:.done.  A step that never returns is
# then a file that is not there, with the netstat samples taken while it was
# still running beside it -- rather than a run that times out and produces
# nothing.  Each marker is its own `Echo >file`, opened and closed, because a
# process that hangs never flushes a file it left open.
#
# THE SAMPLES ARE THE POINT.  `netstat -a` is run four times while the
# detached process is working: whether a connection to the server's port 445
# is ESTABLISHED at that moment separates "the handler never connected" from
# "it connected and the exchange stopped".
#
#   -A   ACTIVATE=1 in the DOSDriver, so the handler starts at Mount time
#        rather than on first access.  Without it Mount only creates the
#        device node and `List` is what connects; with it, Mount is.  Both
#        arms are worth running: they put the connect in different processes.
#   -C   capture the exchange on the host with tcpdump, port 445/139, for the
#        whole run.  Needs a tcpdump this user can run on the bridge.
#
# INGREDIENTS, none of them ours:
#
#   Workbench 3.1 ADFs   AMINETXDUO_ADF_DIR (-o 31), and amitools' xdftool
#   Workbench 3.2 tree   AMINETXDUO_WB32_TARBALL or tools/mkwb32.sh (-o 32)
#   Kickstart            AMINETXDUO_KICKSTART_<MODEL>[_32]
#   a2065.device         AMINETXDUO_A2065
#   smb2-handler         AMINETXDUO_SMB2FS, the smb2fs archive unpacked
#   filesysbox.library   AMINETXDUO_FILESYSBOX
#   a share to mount     -P, which puts tests/tools/smb2-testserver.py on a
#                        named machine and mounts that.  -u names one that is
#                        already there instead.
#
# -P IS THE ONE TO USE IN A RUNNER.  A share that happens to be up on the LAN
# is not coverage: the day it is off, or its password changes, this test fails
# for a reason that has nothing to do with the stack, and the day it is on,
# nothing records what it was.  With -P the harness brings its own server, on a
# machine it is told about, and takes it away afterwards.  It has to be a THIRD
# machine for the reason tests/tools/smb2-testserver.py:20-23 gives: a frame
# the guest sends to the emulating host's own MAC does not come back round to
# that NIC's pcap.
#
# Exit status: 0 the share mounted and listed, 1 it did not, 2 an ingredient
# is missing.  A `List` that never returned is exit 1, not a timeout: the
# transcript says which step it was.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD="${AMINETXDUO_BUILD:-build/cm}"

MODEL=A1200
OSVER=31
TIMEOUT=420
LISTWAIT=90
URL="${AMINETXDUO_SMB_URL:-smb://turo:r2d23028@storage.local.tinic.net/retro}"
BACKEND="${AMINETXDUO_EMU_BACKEND:-ens18}"
MAC="${AMINETXDUO_EMU_MAC:-52:54:00:5b:2f:11}"
TAG="${AMINETXDUO_RUN_TAG:-smbmount}"
ACTIVATE=0
CAPTURE=0
KEEP=0
STACK=ours
EXTRA=""
CLIENT=smb2

PEERHOST=""
PEERPORT="${AMINETXDUO_SMB_PEER_PORT:-4445}"
URL_GIVEN=0

while getopts "m:o:u:P:B:M:T:t:w:s:x:H:ACk" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        o) OSVER="$OPTARG" ;;
        u) URL="$OPTARG"; URL_GIVEN=1 ;;
        P) PEERHOST="$OPTARG" ;;
        B) BACKEND="$OPTARG" ;;
        M) MAC="$OPTARG" ;;
        T) TAG="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        w) LISTWAIT="$OPTARG" ;;
        A) ACTIVATE=1 ;;
        C) CAPTURE=1 ;;
        s) STACK="$OPTARG" ;;
        x) EXTRA="$OPTARG" ;;
        H) CLIENT="$OPTARG" ;;
        k) KEEP=1 ;;
        *) sed -n '5,9p' "$0" >&2; exit 2 ;;
    esac
done

case "$OSVER" in 31|32) ;; *) echo "-o takes 31 or 32" >&2; exit 2 ;; esac
# WHICH CLIENT.  Two programs, two protocols, one share: smb2-handler speaks
# SMB2/3 through filesysbox and is reached with Mount, smbfs speaks SMB1/CIFS
# straight over bsdsocket and is reached by running it.  A share that one
# refuses and the other takes says the wall is the protocol, not the stack.
case "$CLIENT" in smb2|smbfs) ;; *) echo "-H takes smb2 or smbfs" >&2; exit 2 ;; esac
case "$BUILD" in /*) ;; *) BUILD="$ROOT/$BUILD" ;; esac

HD="$ROOT/build/smbhd-$TAG"

# ------------------------------------------------------------- the server --
#
# -P names a machine to run the server ON, and the guest is then given an
# ADDRESS: it has no resolver pointed at anything that knows "playhouse4", and
# an ssh destination may carry a user besides.  Resolved here, once, and the
# URL is built from the answer.
if [ -n "$PEERHOST" ]; then
    [ "$URL_GIVEN" = 0 ] || {
        echo "-P and -u are two different servers; pick one" >&2; exit 2; }
    PEERNAME="${PEERHOST#*@}"
    PEERADDR=$(getent ahostsv4 "$PEERNAME" 2>/dev/null | awk 'NR==1{print $1}')
    if [ -z "$PEERADDR" ]; then
        case "$PEERNAME" in
            *[!0-9.]*) echo "cannot resolve $PEERNAME to an address for the" \
                            "guest to mount" >&2; exit 2 ;;
            *) PEERADDR="$PEERNAME" ;;
        esac
    fi
    # The share, the user and the password are smb2-testserver.py's, which
    # serves ~/smbshare as RETRO to amiga/bantha.
    URL="smb://amiga:bantha@$PEERADDR:$PEERPORT/RETRO"
fi

# The host the URL names, for ping and for the tcpdump filter.  Not parsed out
# of a general URL grammar: the part between the last @ and the next / .
SMBAUTH=$(printf '%s' "$URL" | sed -e 's|^smb://||' -e 's|^[^@]*@||' -e 's|/.*$||')
SMBHOST=${SMBAUTH%%:*}
SMBPORT=445
case "$SMBAUTH" in *:*) SMBPORT=${SMBAUTH##*:} ;; esac
[ -n "$SMBHOST" ] || { echo "cannot read a host out of $URL" >&2; exit 2; }
# smbfs takes //host/share rather than a URL, so the share is wanted on its
# own as well: the first path component after the authority.
SMBSHARE=$(printf '%s' "$URL" | sed -e 's|^smb://||' -e 's|^[^/]*/||' -e 's|/.*$||')
[ -n "$SMBSHARE" ] || { echo "cannot read a share out of $URL" >&2; exit 2; }

# ------------------------------------------------------------ ingredients --

need() { [ -e "$1" ] || { echo "missing $1${2:+ -- $2}" >&2; exit 2; }; }

# WHICH STACK IS UNDERNEATH.  Both reporters say the same DOSDriver works over
# AmiTCP, so "does it work over another stack on this same machine" is the
# question that decides whether the defect is ours, and it is one flag.  Each
# stack brings its own AddNetInterface and its own ping: a stack that needed
# somebody else's command to start would not be the stack under test.
case "$STACK" in
    ours)
        for f in "$BUILD/src/bsdsocket/bsdsocket.library" \
                 "$BUILD/src/usergroup/usergroup.library" \
                 "$BUILD/src/tools/AddNetInterface" "$BUILD/src/tools/netstat" \
                 "$BUILD/src/tools/ShowNetStatus" "$BUILD/src/tools/ping" \
                 "$BUILD/src/tools/nc" "$BUILD/src/tools/host"; do
            need "$f" "build the tree first"
        done
        STACKDIR="$BUILD/src"
        LIBBSD="$BUILD/src/bsdsocket/bsdsocket.library"
        LIBUG="$BUILD/src/usergroup/usergroup.library"
        CMDDIR="$BUILD/src/tools"
        STACK_TOOLS="AddNetInterface ShowNetStatus netstat ping nc host NetTrace"
        ;;
    roadshow)
        RSDIR="${AMINETXDUO_CMP_ROADSHOW:-$HOME/amiga-assets/stacks/Roadshow-Demo-1.15/Workbench}"
        need "$RSDIR/Libs/bsdsocket.library" "set AMINETXDUO_CMP_ROADSHOW"
        LIBBSD="$RSDIR/Libs/bsdsocket.library"
        LIBUG="$RSDIR/Libs/usergroup.library"
        CMDDIR="$RSDIR/C"
        # No netstat and no nc in the Roadshow demo; the samples that need
        # them say so rather than disappearing.
        STACK_TOOLS="AddNetInterface ShowNetStatus ping"
        ;;
    *) echo "-s takes ours or roadshow" >&2; exit 2 ;;
esac

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for c in "$ROOT/build/a2065.device" "$HOME/amiga-assets/devs/a2065.device"; do
        [ -f "$c" ] && { A2065="$c"; break; }
    done
fi
need "${A2065:-/nonexistent}" "set AMINETXDUO_A2065"

SMB2FS="${AMINETXDUO_SMB2FS:-$HOME/amiga-assets/apps/smb2fs-53.10}"
FSBOX="${AMINETXDUO_FILESYSBOX:-$HOME/amiga-assets/apps/filesysbox-54.9}"

# The CPU suffix both packages carry, chosen the same way tools/demo.sh picks
# an lha: by the model, because a .020 handler traps on a 68000 and a .000 one
# on a 68040 machine merely runs slowly.
case "$MODEL" in
    A500*|A600*|A1000*|A2000*) CPUSFX=000 ;;
    A4000*)                    CPUSFX=060 ;;
    *)                         CPUSFX=020 ;;
esac
SMBFS="${AMINETXDUO_SMBFS:-$HOME/amiga-assets/apps/smbfs-2.22}"
if [ "$CLIENT" = smbfs ]; then
    need "$SMBFS/smbfs" "set AMINETXDUO_SMBFS"
else
    need "$SMB2FS/L/smb2-handler.$CPUSFX" "set AMINETXDUO_SMB2FS"
    need "$FSBOX/Libs/filesysbox.library.$CPUSFX" "set AMINETXDUO_FILESYSBOX"
fi

# THE ROM AND THE MODEL ARE A PAIR, and so are the ROM and the Workbench: a
# 3.2 tree on a 3.1 ROM boots but is not what either reporter is running.
if [ "$OSVER" = 32 ]; then
    eval "KICKSTART=\${AMINETXDUO_KICKSTART_${MODEL}_32:-}"
else
    eval "KICKSTART=\${AMINETXDUO_KICKSTART_$MODEL:-}"
    case "$MODEL" in
        A1200|A3000|A4000) KICKSTART="${KICKSTART:-${AMINETXDUO_KICKSTART:-}}" ;;
    esac
fi
[ -n "$KICKSTART" ] && [ -f "$KICKSTART" ] || {
    echo "no Kickstart for $MODEL / OS 3.$([ "$OSVER" = 32 ] && echo 2 || echo 1)." >&2
    echo "Set AMINETXDUO_KICKSTART_${MODEL}$([ "$OSVER" = 32 ] && echo _32)=<path>." >&2
    exit 2
}

AMIBERRY="${AMIBERRY:-$(command -v amiberry || true)}"
[ -n "$AMIBERRY" ] || for c in "$HOME/amiberry/build/amiberry" "$HOME/amiberry/amiberry"; do
    [ -x "$c" ] && { AMIBERRY="$c"; break; }
done
[ -n "$AMIBERRY" ] || { echo "amiberry not found; set AMIBERRY=<path>" >&2; exit 2; }

echo "==> $MODEL, OS 3.$([ "$OSVER" = 32 ] && echo 2 || echo 1), $(basename "$KICKSTART")"
echo "==> stack $STACK"
if [ "$CLIENT" = smbfs ]; then
    echo "==> share $URL (host $SMBHOST), client smbfs (SMB1/CIFS)"
else
    echo "==> share $URL (host $SMBHOST), client smb2-handler.$CPUSFX (SMB2/3)"
fi

# --------------------------------------------------------------- the SYS: --

if [ "$OSVER" = 32 ]; then
    WB="$ROOT/build/wb32-sys"
    TARBALL="${AMINETXDUO_WB32_TARBALL:-$HOME/amiga-assets/os32/wb32-sys-A1200.tar.gz}"
    if [ ! -d "$WB" ]; then
        if [ -f "$TARBALL" ]; then
            echo "==> unpacking $(basename "$TARBALL")"
            mkdir -p "$WB"
            tar xzf "$TARBALL" -C "$WB"
        else
            echo "==> assembling Workbench 3.2 from the CD's ADFs"
            "$ROOT/tools/mkwb32.sh" -o "$WB"
        fi
        chmod -R a+rx "$WB"
    else
        echo "==> Workbench 3.2 tree is current ($WB)"
    fi
else
    WB="$ROOT/build/wb31-sys"
    ADFDIR="${AMINETXDUO_ADF_DIR:-$HOME/amigaos/adf}"
    XDFTOOL="${AMINETXDUO_XDFTOOL:-$(command -v xdftool || true)}"
    if [ ! -d "$WB" ]; then
        [ -n "$XDFTOOL" ] && [ -x "$XDFTOOL" ] || {
            echo "amitools' xdftool not found; pip install amitools" >&2; exit 2; }
        echo "==> assembling Workbench 3.1 into $WB (five ADFs)"
        SCRATCH="$ROOT/build/.smb-wb31-unpack"
        rm -rf "$SCRATCH" "$WB"; mkdir -p "$SCRATCH" "$WB"
        for disk in workbench fonts locale storage extras; do
            adf="$ADFDIR/amiga-wb31_$disk.adf"
            need "$adf" "set AMINETXDUO_ADF_DIR"
            into="$SCRATCH/$disk"; mkdir -p "$into"
            "$XDFTOOL" "$adf" unpack "$into" >/dev/null
            inner=$(find "$into" -maxdepth 1 -mindepth 1 -type d | head -1)
            if [ -n "$inner" ]; then
                mv "$inner"/* "$into"/ 2>/dev/null || true
                rmdir "$inner" 2>/dev/null || true
            fi
            rm -f "$into"/*.blkdev "$into"/*.bootcode "$into"/*.xdfmeta
        done
        cp -R "$SCRATCH/workbench/." "$WB/"
        for pair in "fonts:Fonts" "locale:Locale" "storage:Storage" "extras:Extras"; do
            mkdir -p "$WB/${pair##*:}"
            cp -R "$SCRATCH/${pair%%:*}/." "$WB/${pair##*:}/"
        done
        rm -rf "$SCRATCH"
        # xdftool unpacks 0644, which would leave every command in C: without
        # its E bit once the host mode bits become Amiga protection bits.
        chmod -R a+rx "$WB"
    else
        echo "==> Workbench 3.1 tree is current ($WB)"
    fi
fi

# `Run`, `Echo`, `FailAt` and `Stack` are the Shell's own built-ins in
# AmigaDOS 2.0 and later, not files, so only the disk commands are looked
# for here.  The 3.1 Workbench floppy carries 50 of them and Run is not
# among them; requiring C/Run made this exit 2 on a perfectly good tree.
# `Execute` is a disk command on 3.1 and a built-in on 3.2, so it is called
# without a C: prefix below and is not required here either.
for want in C/Mount C/List C/Wait C/Info C/Assign; do
    [ -e "$WB/$want" ] || { echo "!! the assembled SYS: has no $want" >&2; exit 2; }
done

# --------------------------------------------------------------- the drive --

rm -rf "$HD"; mkdir -p "$HD"
cp -R "$WB/." "$HD/"
mkdir -p "$HD/L" "$HD/Libs" "$HD/C" "$HD/S" \
         "$HD/Devs/NetInterfaces" "$HD/Devs/Internet" \
         "$HD/Storage/DOSDrivers" "$HD/Devs/Networks"

cp "$A2065" "$HD/Devs/a2065.device"
cp "$A2065" "$HD/Devs/Networks/a2065.device"
cp "$LIBBSD" "$HD/Libs/bsdsocket.library"
[ -f "$LIBUG" ] && cp "$LIBUG" "$HD/Libs/usergroup.library"
HAVE_NETSTAT=no; HAVE_NC=no
for t in $STACK_TOOLS; do
    if [ -f "$CMDDIR/$t" ]; then
        cp "$CMDDIR/$t" "$HD/C/$t"
        [ "$t" = netstat ] && HAVE_NETSTAT=yes
        [ "$t" = nc ] && HAVE_NC=yes
    fi
done
true

# SMBProbe, built here rather than shipped: it is a test instrument, and the
# only way to tell a device that answered with an error from one that put up
# an invisible requester and waited.
GCC="${AMIGA_GCC:-$HOME/amigaos/tools/m68k-amigaos-gcc/bin/m68k-amigaos-gcc}"
NDK="${AMIGA_NDK:-$HOME/amigaos/tools/m68k-amigaos-gcc/m68k-amigaos/ndk-include}"
[ -x "$GCC" ] || { echo "no m68k gcc at $GCC; set AMIGA_GCC" >&2; exit 2; }
"$GCC" -O2 -m68000 -Wall -Wextra -I"$NDK" \
       -o "$ROOT/build/SMBProbe-$TAG" "$ROOT/install/test/smbprobe.c" || exit 2
cp "$ROOT/build/SMBProbe-$TAG" "$HD/C/SMBProbe"

if [ "$CLIENT" = smbfs ]; then
    cp "$SMBFS/smbfs" "$HD/C/smbfs"
    chmod 755 "$HD/C/smbfs"
else
    cp "$FSBOX/Libs/filesysbox.library.$CPUSFX" "$HD/Libs/filesysbox.library"
    cp "$SMB2FS/L/smb2-handler.$CPUSFX"         "$HD/L/smb2-handler"
    chmod 755 "$HD/L/smb2-handler" "$HD/Libs/filesysbox.library"
fi
chmod 755 "$HD/Devs/a2065.device" "$HD/Devs/Networks/a2065.device"
chmod 755 "$HD/C/"* 2>/dev/null || true

cat > "$HD/Devs/NetInterfaces/eth0" <<EOF
DEVICE=a2065.device
UNIT=0
CONFIGURE=DHCP
EOF

cat > "$HD/Devs/Internet/name_resolution" <<EOF
hostname amiga-smb
EOF

# THE DOSDRIVER, verbatim from smb2fs's own README, and in
# SYS:Storage/DOSDrivers rather than DEVS:DOSDrivers: the stock
# Startup-Sequence mounts everything in DEVS:DOSDrivers on the way up, before
# the network exists, so a driver left there is already mounted by the time
# this script runs (Mount then says "already mounted", rc 20) and with
# ACTIVATE=1 the handler would start with no stack under it.  ACTIVATE decides which
# process does the connecting: without it Mount only builds the device node
# and the first access starts the handler; with it, Mount does.
if [ "$CLIENT" = smb2 ]; then
{
    echo "Handler   = L:smb2-handler"
    echo "StackSize = 65536"
    echo "Priority  = 5"
    echo "GlobVec   = -1"
    [ "$ACTIVATE" = 1 ] && echo "ACTIVATE  = 1"
    echo "Startup   = \"$URL${EXTRA:+ $EXTRA}\""
} > "$HD/Storage/DOSDrivers/SMB2"
fi

# ------------------------------------------------------------ the scripts --
#
# The detached half.  Every step writes a file before it runs and a file after
# it returns, each one its own open-and-close, so a step that never returns is
# a file that is not there rather than output stuck in a buffer nobody
# flushed.

if [ "$CLIENT" = smbfs ]; then
# smbfs is not mounted, it is run: it puts SMB2: up itself and stays resident
# for as long as the share is there, so it goes in the background and the
# steps after it get a moment before they look.  Its console output is the
# only place its errors appear, so that is a file and not NIL:.
cat > "$HD/S/SMB-Work" <<EOF
FailAt 9999
Echo >DH0:w1-mount-start.txt "starting smbfs"
Run >DH0:w2-mount-out.txt <NIL: C:smbfs DEVICE=SMB2 //$SMBHOST:$SMBPORT/$SMBSHARE${EXTRA:+ $EXTRA}
Echo >DH0:w3-mount-rc.txt "\$RC"
C:Wait 20
EOF
else
cat > "$HD/S/SMB-Work" <<'EOF'
FailAt 9999
Echo >DH0:w1-mount-start.txt "mounting"
C:Mount SMB2: >DH0:w2-mount-out.txt
Echo >DH0:w3-mount-rc.txt "$RC"
EOF
fi
cat >> "$HD/S/SMB-Work" <<'EOF'
C:SMBProbe LOCK DEVICE=SMB2: LOG=DH0:w4-probe.txt >DH0:w4-probe-console.txt
Echo >DH0:w5-probe-rc.txt "$RC"
C:List SMB2: >DH0:w6-list-out.txt
Echo >DH0:w7-list-rc.txt "$RC"
C:Info SMB2: >DH0:w8-info-out.txt
Echo >DH0:w9-info-rc.txt "$RC"
Echo >DH0:wA-end.txt "done"
EOF

NCLINE="Echo >>DH0:smbcheck.txt \"RESULT nc445 rc=skipped\""
NETSTATLINE="Echo >>DH0:smbcheck.txt \"(this stack ships no netstat)\""
if [ "$HAVE_NC" = yes ]; then
    NCLINE="C:nc -v -w 5 $SMBHOST $SMBPORT >>DH0:smbcheck.txt
Echo >>DH0:smbcheck.txt \"RESULT nc445 rc=\$RC\""
fi
if [ "$HAVE_NETSTAT" = yes ]; then
    NETSTATLINE="C:netstat -a >>DH0:smbcheck.txt"
fi

# The boot shell's half.  It brings the network up, says what it sees, starts
# the detached half, and then samples the connection table while that half is
# working.  It never waits on it.
cat > "$HD/S/SMB-Check" <<EOF
FailAt 9999
Stack 200000

Echo >DH0:smbcheck.txt "=== 1. the network"
C:AddNetInterface eth0 >>DH0:smbcheck.txt
Echo >>DH0:smbcheck.txt "RESULT addnet rc=\$RC"
C:ShowNetStatus >>DH0:smbcheck.txt

Echo >>DH0:smbcheck.txt "*N=== 2. the server, from this machine"
C:ping $SMBHOST -c 2 -t 10 >>DH0:smbcheck.txt
Echo >>DH0:smbcheck.txt "RESULT ping rc=\$RC"
$NCLINE
Echo >>DH0:smbcheck.txt "*N=== 3. connections before the mount"
$NETSTATLINE

Echo >>DH0:smbcheck.txt "*N=== 4. Mount, List and Info, detached"
Run >NIL: <NIL: Execute S:SMB-Work
Echo >>DH0:smbcheck.txt "RESULT run rc=\$RC"
EOF

# The samples, written from the loop rather than by hand so -w changes the
# window without editing the script.
_t=0
while [ "$_t" -lt "$LISTWAIT" ]; do
    _step=15
    [ $((_t + _step)) -gt "$LISTWAIT" ] && _step=$((LISTWAIT - _t))
    _t=$((_t + _step))
    {
        echo "C:Wait $_step"
        echo "Echo >>DH0:smbcheck.txt \"*N=== connections ${_t}s into the mount\""
        echo "$NETSTATLINE"
    } >> "$HD/S/SMB-Check"
done

cat >> "$HD/S/SMB-Check" <<'EOF'

Echo >>DH0:smbcheck.txt "*N=== 5. what is on the screen right now"
C:SMBProbe WINDOWS LOG=DH0:windows.txt >>DH0:smbcheck.txt
Echo >>DH0:smbcheck.txt "RESULT windows rc=$RC"
C:SMBProbe TASKS LOG=DH0:tasks.txt >>DH0:smbcheck.txt
Echo >>DH0:smbcheck.txt "RESULT tasks rc=$RC"

Echo >>DH0:smbcheck.txt "*N=== 6. what the volume looks like now"
C:Assign >>DH0:smbcheck.txt
Echo >>DH0:smbcheck.txt "*N=== done"
EOF
chmod 755 "$HD/S/SMB-Check" "$HD/S/SMB-Work"

# The stock Startup-Sequence with its tail replaced.  `EndCLI` would take the
# boot shell away before any of this ran.  3.2 spells the file
# `Startup-sequence`; AmigaDOS does not care and this host does.
SS=$(find "$HD/S" -maxdepth 1 -iname 'startup-sequence' | head -1)
[ -n "$SS" ] || { echo "!! no S/Startup-Sequence on the drive" >&2; exit 2; }
sed -e '/^EndCLI/d' -e '/^ *EndShell/d' "$SS" > "$SS.new"
cat >> "$SS.new" <<'EOF'

FailAt 9999
Execute S:SMB-Check >DH0:check-console.txt
Echo >DH0:.done "$RC"
EOF
mv "$SS.new" "$SS"
chmod 755 "$SS"

# -------------------------------------------------------- the peer's server --
#
# BY PID, FROM A FILE THE PEER WRITES, and never `pkill -f`.  `pkill -f
# smb2-testserver-<tag>` matches the command line of the very shell that is
# about to start the server, because that name is in it: the first version of
# this killed its own remote shell before python ran, left an empty log and a
# port nothing was listening on, and read as a server that would not start.
# tests/perf/peercap.sh:108-111 says the same thing about the same mistake.
#
# The `timeout` on the far side is the other half: killing the local ssh does
# not kill what it started there, so a server with no ceiling of its own
# outlives its run, holds the port, and the next run mounts it.

SRV_PID=""
REMOTE_PY=""
REMOTE_PID="/tmp/smb2-testserver-$TAG.pid"
REMOTE_LOG="/tmp/smb2-testserver-$TAG.log"
stop_server() {
    [ -n "$SRV_PID" ] && kill -TERM "$SRV_PID" 2>/dev/null
    if [ -n "$REMOTE_PY" ]; then
        # What the server thought, brought back before it is deleted: when a
        # mount fails it is the other half of the transcript, and libsmb2's
        # complaint is usually in it.
        scp -q "$PEERHOST:$REMOTE_LOG" "$ROOT/build/smb-$TAG-server.log" \
            2>/dev/null || true
        ssh -o ConnectTimeout=10 "$PEERHOST" \
            "[ -f $REMOTE_PID ] && kill \$(cat $REMOTE_PID) 2>/dev/null; \
             rm -f $REMOTE_PY $REMOTE_PID $REMOTE_LOG; exit 0" \
            >/dev/null 2>&1
    fi
    SRV_PID=""; REMOTE_PY=""
    return 0
}

if [ -n "$PEERHOST" ]; then
    REMOTE_PY="/tmp/smb2-testserver-$TAG.py"
    scp -q "$ROOT/tests/tools/smb2-testserver.py" "$PEERHOST:$REMOTE_PY" || {
        echo "cannot copy the server to $PEERHOST" >&2; exit 2; }
    # impacket is what it is written against, and a peer without it fails at
    # import time inside a detached process, which would read here as a share
    # that refused the mount.  Ask first.
    ssh -o ConnectTimeout=10 "$PEERHOST" \
        "PYTHONPATH=\${AMINETXDUO_PYTHONPATH:-\$HOME/py-impacket} \
         python3 -c 'import impacket'" >/dev/null 2>&1 || {
        echo "$PEERHOST cannot run tests/tools/smb2-testserver.py: no" \
             "impacket for its python3.  Install it, or point" \
             "AMINETXDUO_PYTHONPATH there at a tree that has it." >&2
        stop_server; exit 2; }
    trap stop_server EXIT INT TERM HUP
    ssh -o ConnectTimeout=10 "$PEERHOST" \
        "nohup env PYTHONPATH=\${AMINETXDUO_PYTHONPATH:-\$HOME/py-impacket} \
             timeout $((TIMEOUT + 120)) python3 $REMOTE_PY $PEERPORT \
             > $REMOTE_LOG 2>&1 & \
         echo \$! > $REMOTE_PID" >/dev/null 2>&1 || {
        echo "cannot start the server on $PEERHOST" >&2; exit 2; }
    srv_up() { ssh -o ConnectTimeout=10 "$PEERHOST" \
                   "exec 3<>/dev/tcp/127.0.0.1/$PEERPORT" >/dev/null 2>&1; }
    for _ in $(seq 1 20); do srv_up && break; sleep 1; done
    srv_up || {
        echo "the server never came up on $PEERHOST:$PEERPORT:" >&2
        ssh -o ConnectTimeout=10 "$PEERHOST" "cat $REMOTE_LOG" >&2 2>/dev/null
        exit 2; }
    echo "==> serving RETRO from $PEERHOST on $PEERADDR:$PEERPORT"
fi

# ------------------------------------------------------------- the capture --

CAPFILE="$ROOT/build/smb-$TAG.pcap"
CAP_PID=""
if [ "$CAPTURE" = 1 ]; then
    rm -f "$CAPFILE"
    tcpdump -i "$BACKEND" -s 0 -U -w "$CAPFILE" \
        "host $SMBHOST and (port $SMBPORT or port 139)" \
        > "$ROOT/build/smb-$TAG-tcpdump.log" 2>&1 &
    CAP_PID=$!
    sleep 2
    if kill -0 "$CAP_PID" 2>/dev/null; then
        echo "==> capturing to $CAPFILE (pid $CAP_PID)"
    else
        CAP_PID=""
        echo "==> tcpdump would not start on $BACKEND, running without a capture:"
        sed -n '1,4p' "$ROOT/build/smb-$TAG-tcpdump.log" >&2
    fi
fi

# ------------------------------------------------------------- the machine --

export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-dummy}"
export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-dummy}"
[ "$SDL_VIDEODRIVER" = dummy ] && unset DISPLAY WAYLAND_DISPLAY || true

CFG="$ROOT/build/smb-$TAG.uae"
SERIAL="$ROOT/build/smb-$TAG-serial.log"
PORT=$((12000 + $(printf '%s' "smb-$TAG" | cksum | cut -d' ' -f1) % 900))
: > "$SERIAL"

cat > "$CFG" <<EOF
config_description=AmiNetXDuo smb $TAG
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
EOF

EMU_PID=""; SERIAL_PID=""
cleanup() {
    stop_server
    [ -n "$EMU_PID" ] && { kill -TERM "$EMU_PID" 2>/dev/null || true; sleep 1
                           kill -KILL "$EMU_PID" 2>/dev/null || true; }
    [ -n "$SERIAL_PID" ] && kill -TERM "$SERIAL_PID" 2>/dev/null || true
    [ -n "$CAP_PID" ] && kill -TERM "$CAP_PID" 2>/dev/null || true
    EMU_PID=""; SERIAL_PID=""; CAP_PID=""
}
trap cleanup EXIT INT TERM HUP

echo "==> booting, ${TIMEOUT}s budget, a2065 bridged on $BACKEND as $MAC"
( trap '' PIPE; exec "$AMIBERRY" --log -f "$CFG" ) \
    > "$ROOT/build/smb-$TAG-amiberry.log" 2>&1 &
EMU_PID=$!
(
    for _ in $(seq 1 60); do
        kill -0 "$EMU_PID" 2>/dev/null || exit 0
        nc 127.0.0.1 "$PORT" >> "$SERIAL" 2>/dev/null && exit 0
        sleep 0.5
    done
) &
SERIAL_PID=$!

elapsed=0; STATUS=124
while [ "$elapsed" -lt "$TIMEOUT" ]; do
    if [ -f "$HD/.done" ]; then
        STATUS=$(tr -dc '0-9' < "$HD/.done" | head -c 4); STATUS=${STATUS:-0}
        break
    fi
    kill -0 "$EMU_PID" 2>/dev/null || { echo "!! amiberry exited after ${elapsed}s" >&2; break; }
    sleep 1; elapsed=$((elapsed + 1))
done
echo "    (finished after ${elapsed}s, boot status $STATUS)"
cleanup
trap - EXIT INT TERM HUP

# ------------------------------------------------------------- the result --

echo
echo "================= what the machine did ================="
cat "$HD/smbcheck.txt" 2>/dev/null || echo "(DH0:smbcheck.txt was never written)"

echo
echo "================= the detached mount ==================="
for f in w1-mount-start w2-mount-out w3-mount-rc w4-probe w4-probe-console \
         w5-probe-rc w6-list-out w7-list-rc w8-info-out w9-info-rc wA-end; do
    if [ -e "$HD/$f.txt" ]; then
        printf -- '---- %s ----\n' "$f"
        cat "$HD/$f.txt"
    else
        printf -- '---- %s ---- NEVER WRITTEN\n' "$f"
    fi
done

for extra in windows tasks; do
    if [ -s "$HD/$extra.txt" ]; then
        echo
        printf -- '---- %s ----\n' "$extra.txt"
        cat "$HD/$extra.txt"
    fi
done

if [ -s "$HD/check-console.txt" ]; then
    echo
    echo "---- anything the boot shell itself said ----"
    cat "$HD/check-console.txt"
fi

# A step that never returned wrote no rc file at all, and reading one that is
# not there must not end the run under `set -e` before the table is printed.
rcof() { [ -e "$HD/$1.txt" ] || return 0
         tr -dc '0-9-' < "$HD/$1.txt" | head -c 6; }

MOUNT_RC=$(rcof w3-mount-rc); PROBE_RC=$(rcof w5-probe-rc)
LIST_RC=$(rcof w7-list-rc); INFO_RC=$(rcof w9-info-rc)
STOPPED=none
for step in "mount:w3-mount-rc" "probe:w5-probe-rc" "list:w7-list-rc" \
            "info:w9-info-rc" "end:wA-end"; do
    [ -e "$HD/${step##*:}.txt" ] || { STOPPED="${step%%:*}"; break; }
done
ESTAB=no
grep -qE "[.:]$SMBPORT([^0-9]|$)" "$HD/smbcheck.txt" 2>/dev/null && ESTAB=yes
LISTED=no
[ -s "$HD/w6-list-out.txt" ] && LISTED=yes

echo
echo "boot_status=$STATUS"
echo "addnet_rc=$(sed -n 's/^RESULT addnet rc=//p' "$HD/smbcheck.txt" 2>/dev/null | head -1)"
echo "ping_rc=$(sed -n 's/^RESULT ping rc=//p' "$HD/smbcheck.txt" 2>/dev/null | head -1)"
echo "nc445_rc=$(sed -n 's/^RESULT nc445 rc=//p' "$HD/smbcheck.txt" 2>/dev/null | head -1)"
echo "mount_rc=${MOUNT_RC:-NEVER_RETURNED}"
echo "probe_rc=${PROBE_RC:-NEVER_RETURNED}"
echo "probe_lock=$(sed -n 's/^RESULT lock=//p' "$HD/w4-probe.txt" 2>/dev/null | head -1)"
echo "windows_open=$(sed -n 's/^RESULT windows=//p' "$HD/windows.txt" 2>/dev/null | head -1)"
echo "tasks_seen=$(sed -n 's/^RESULT tasks=//p' "$HD/tasks.txt" 2>/dev/null | head -1)"
echo "list_rc=${LIST_RC:-NEVER_RETURNED}"
echo "info_rc=${INFO_RC:-NEVER_RETURNED}"
echo "list_produced_output=$LISTED"
echo "smb_socket_seen=$ESTAB"
echo "stopped_at=$STOPPED"
echo "client=$CLIENT"
echo "smb_server=${PEERHOST:-external}"
echo "activate=$ACTIVATE"
echo "startup_extra=${EXTRA:-none}"
echo "os=3.$([ "$OSVER" = 32 ] && echo 2 || echo 1)"
echo "model=$MODEL"
echo "stack=$STACK"
echo "drive=$HD"
[ -s "$CAPFILE" ] && echo "capture=$CAPFILE"

if [ "$STOPPED" = none ] && [ "${MOUNT_RC:-1}" = 0 ] && [ "${LIST_RC:-1}" = 0 ] \
   && [ "$LISTED" = yes ]; then
    echo "verdict=mounted"
    [ "$KEEP" = 1 ] || true
    exit 0
fi
if [ "$STOPPED" != none ]; then
    echo "verdict=hung_at_$STOPPED"
else
    echo "verdict=failed"
fi
exit 1
