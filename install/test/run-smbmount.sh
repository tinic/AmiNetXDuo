#!/usr/bin/env bash
#
# MOUNT AN SMB SHARE THE WAY A USER DOES: a real Workbench, a real Shell, a
# DOSDriver, `Mount` and `List`, over our stack.
#
#   install/test/run-smbmount.sh [-m MODEL] [-o 31|32] [-u URL] [-P PEERHOST]
#                                [-B backend] [-M mac] [-T tag] [-t seconds]
#                                [-w seconds] [-A] [-C] [-k]
#
# NOTHING HANGS THE HARNESS: Mount, List and Info run DETACHED, each step
# writing its own marker with its own `Echo >file`, because a process that
# hangs never flushes a file it left open.  `netstat -a` is sampled four times
# meanwhile, separating "never connected" from "connected and stopped".
#
#   -A   ACTIVATE=1, so the handler starts at Mount time, not on first access
#   -P   puts tests/tools/smb2-testserver.py on a THIRD machine and mounts it;
#        the emulating host cannot be it, its frames do not reach its own pcap
#
# Exit 0 mounted and listed, 1 not, 2 an ingredient is missing.
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD="${AMINETXDUO_BUILD:-build/cm}"

MODEL=A1200
OSVER=31
TIMEOUT=420
LISTWAIT=90
# No default: this took a real NAS credential, in a public repository. -P
# stands a test server up and is what CI uses; -u names a share explicitly.
URL="${AMINETXDUO_SMB_URL:-}"
BACKEND="${AMINETXDUO_EMU_BACKEND:-ens18}"
# Empty here and derived from the tag below, AFTER getopts, because -T changes
# the tag and -M names an address outright.
MAC="${AMINETXDUO_EMU_MAC:-}"
TAG="${AMINETXDUO_RUN_TAG:-smbmount}"
ACTIVATE=0
CAPTURE=0
KEEP=0
STACK=ours
# AMINETXDUO_SMB_UAENET=1 swaps the emulated Zorro card for amiberry's own
# uaenet.device: no register-level card emulation, so the link is not capped at
# the a2065's 10 Mbit.  The emulator supplies the driver, so nothing is staged.
if [ "${AMINETXDUO_SMB_UAENET:-0}" = 1 ]; then
    UAENET_CFG="sana2=true"
    SMB_DEVICE="uaenet.device"
fi

# AMINETXDUO_SMB_BOARD=<card> boots an emulated card instead, driven by our own
# anxnet.device.  The cards are drained at hsync and uaenet at vsync
# (amiberry sana2.cpp:1849 vs ne2000.cpp:1287), which is the difference this
# option exists to measure.
if [ -n "${AMINETXDUO_SMB_BOARD:-}" ]; then
    UAENET_CFG=""
    # DEVICE= is written into DEVS:NetInterfaces/eth0 long before the driver is
    # staged, so the NAME has to be settled here.  Deciding it at staging time
    # left eth0 saying anxnet.device while the vendor driver was the one on
    # disk, and the interface refused with ENXIO.
    if [ "${AMINETXDUO_SMB_VENDOR:-0}" = 1 ]; then
        case "$AMINETXDUO_SMB_BOARD" in
            xsurf100z2|xsurf100z3) SMB_DEVICE=x-surf-100.device ;;
            xsurf)                 SMB_DEVICE=x-surf.device ;;
            ariadne2)              SMB_DEVICE=ariadne_ii.device ;;
            ariadne)               SMB_DEVICE=ariadne.device ;;
            hydra)                 SMB_DEVICE=hydra.device ;;
            eb920)                 SMB_DEVICE=eb920.device ;;
            a2065)                 SMB_DEVICE=a2065.device ;;
            ne2000_pcmcia)         SMB_DEVICE=cnet.device ;;
            *)                     SMB_DEVICE="$AMINETXDUO_SMB_BOARD.device" ;;
        esac
    else
        SMB_DEVICE="anxnet.device"
    fi
fi
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

# One MAC per tag, from tools/emu-mac.sh, rather than the pinned address this
# had: a fixed one puts every run of every arm on the bridge under the same
# hardware address, beside the demo instance and beside other checkouts'
# guests, and a peer's neighbour cache then keeps whichever answered last.
# shellcheck source=../../tools/emu-mac.sh
. "$ROOT/tools/emu-mac.sh"
[ -n "$MAC" ] || MAC=$(emu_mac_for_tag "$TAG")

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
        STACK_TOOLS="AddNetInterface ShowNetStatus netstat ping nc host NetTrace iperf ShowNetServices"
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
    amitcpng)
        # AmiTCP_NG 4.1.5, staged by another checkout's harness.  The third
        # stack mja65 compared against, and the one this harness kept dropping
        # because -s only knew two names.  Its tree is lowercase `libs' and
        # `amitcp/C', and it spells the status command GetNetStatus, so the
        # names are mapped here rather than left to fail as a missing file.
        NGDIR="${AMINETXDUO_CMP_AMITCPNG:-$HOME/anxd-metro/build/amitcpng-stage}"
        need "$NGDIR/libs/bsdsocket.library" "set AMINETXDUO_CMP_AMITCPNG"
        LIBBSD="$NGDIR/libs/bsdsocket.library"
        LIBUG="$NGDIR/libs/usergroup.library"
        CMDDIR="$NGDIR/amitcp/C"
        STACK_TOOLS="AddNetInterface ping netstat"
        # AmiTCP_NG is not driven like the other two.  ~/anxd-metro's own
        # commands.txt shows the form: the tree is assigned to AmiTCP: and
        # AddNetInterface takes the PATH to the interface file rather than the
        # interface name.  Without both it never opens the device and the arm
        # runs out its whole boot budget with an empty serial log, which is
        # what happened on the first attempt.
        NG_TREE="$NGDIR/amitcp"
        NG_IFUP="C:Assign AmiTCP: SYS:amitcp
SYS:amitcp/C/AddNetInterface DEVS:NetInterfaces/eth0"
        ;;
    *) echo "-s takes ours, roadshow or amitcpng" >&2; exit 2 ;;
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
[ -n "${NG_TREE:-}" ] && cp -R "$NG_TREE" "$HD/amitcp"
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

# AMINETXDUO_SMB_ADDR pins the guest's address.  Required for any leg where
# the PEER has to initiate -- an iperf RECEIVE, where the peer must know where
# to connect before the guest has booted.  DHCP everywhere else.
if [ -n "${AMINETXDUO_SMB_ADDR:-}" ]; then
    IFCONF="CONFIGURE=STATIC
ADDRESS=$AMINETXDUO_SMB_ADDR
NETMASK=${AMINETXDUO_SMB_MASK:-255.255.255.0}
GATEWAY=${AMINETXDUO_SMB_GW:-192.168.1.1}"
else
    IFCONF="CONFIGURE=DHCP"
fi

cat > "$HD/Devs/NetInterfaces/eth0" <<EOF
DEVICE=${SMB_DEVICE:-a2065.device}
${AMINETXDUO_SMB_MTU:+MTU=$AMINETXDUO_SMB_MTU}
UNIT=0
MDNS=${AMINETXDUO_SMB_MDNS:-NO}
$IFCONF
EOF

cat > "$HD/Devs/Internet/name_resolution" <<EOF
hostname ${AMINETXDUO_SMB_HOST:-amiga-smb}
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
# THE CONTROL'S SOURCE FILE, WHICH DID NOT EXIST UNTIL 2026-09-13.
#
# S/SMB-Work has copied DH0:localsrc.dat since the control was added, and
# nothing ever created it: Copy failed instantly, the two C:Date stamps came
# out equal, and the harness reported a 0 s local copy -- a control that
# measured nothing and could not say so.  Same size as the SMB payload and the
# same destination volume, so the difference between the two stamps is the SMB
# path and nothing else.
#
# 32 MB matches bigfile.dat on the peer's share.  DH0 is a DIRECTORY MAPPING,
# so this write goes through amiberry's filesystem emulation exactly as
# copytmp.dat does.
if [ ! -s "$HD/localsrc.dat" ]; then
    dd if=/dev/zero of="$HD/localsrc.dat" bs=1M count=32 status=none
fi

cat >> "$HD/S/SMB-Work" <<'EOF'
C:SMBProbe LOCK DEVICE=SMB2: LOG=DH0:w4-probe.txt >DH0:w4-probe-console.txt
Echo >DH0:w5-probe-rc.txt "$RC"
C:List SMB2: >DH0:w6-list-out.txt
Echo >DH0:w7-list-rc.txt "$RC"
C:Info SMB2: >DH0:w8-info-out.txt
Echo >DH0:w9-info-rc.txt "$RC"
C:Date >DH0:wE-local-start.txt
C:Copy DH0:localsrc.dat TO DH0:localdst.dat >DH0:wE2-local-out.txt
Echo >DH0:wE3-local-rc.txt "$RC"
C:Date >DH0:wF-local-end.txt
C:Delete DH0:localdst.dat >NIL:
@RX@Run >DH0:wX2-rx-out.txt <NIL: C:iperf -s -p 5002
@RX@C:Wait 2
@WIRE@C:Date >DH0:wW1-wire-start.txt
@WIRE@C:iperf @TCPHOST@ -p 5001 -n 32768 >DH0:wW2-wire-out.txt
@WIRE@Echo >DH0:wW3-wire-rc.txt "$RC"
@WIRE@C:Date >DH0:wW4-wire-end.txt
@LOOP@Run >NIL: <NIL: C:iperf -s -p 5999
@LOOP@C:Wait 3
@LOOP@C:Date >DH0:wL1-loop-start.txt
@LOOP@C:iperf 127.0.0.1 -p 5999 -n 32768 >DH0:wL2-loop-out.txt
@LOOP@Echo >DH0:wL3-loop-rc.txt "$RC"
@LOOP@C:Date >DH0:wL4-loop-end.txt
@TCPCTRL@C:Date >DH0:wN1-nil-start.txt
@TCPCTRL@C:nc @TCPHOST@ @TCPPORT@ >NIL:
@TCPCTRL@Echo >DH0:wN2-nil-rc.txt "$RC"
@TCPCTRL@C:Date >DH0:wN3-nil-end.txt
C:Date >DH0:wR1-ram-start.txt
C:Copy SMB2:midfile.dat TO RAM:midram.dat >DH0:wR2-ram-out.txt
Echo >DH0:wR3-ram-rc.txt "$RC"
C:Date >DH0:wR4-ram-end.txt
C:Delete RAM:midram.dat >NIL:
C:Date >DH0:wR5-dh0-start.txt
C:Copy SMB2:midfile.dat TO DH0:middh0.dat >DH0:wR6-dh0-out.txt
Echo >DH0:wR7-dh0-rc.txt "$RC"
C:Date >DH0:wR8-dh0-end.txt
C:Delete DH0:middh0.dat >NIL:
C:Date >DH0:wB-read-start.txt
C:Copy SMB2:hello.txt TO RAM: >DH0:wB0-small-out.txt
Echo >DH0:wB1-small-rc.txt "$RC"
C:Copy SMB2:bigfile.dat TO DH0:copytmp.dat >DH0:wB2-copy-out.txt
Echo >DH0:wC-read-rc.txt "$RC"
C:Date >DH0:wD-read-end.txt
C:Delete DH0:copytmp.dat >NIL:
Echo >DH0:wA-end.txt "done"
EOF

# The control's lines are marked so they can be filled in or removed whole;
# the heredoc above is quoted, so nothing in it expands on its own.
if [ "${AMINETXDUO_SMB_RX:-0}" = 1 ]; then
    sed -i -e "s|@RX@||" "$HD/S/SMB-Work"
else
    sed -i -e "/@RX@/d" "$HD/S/SMB-Work"
fi
if [ "${AMINETXDUO_SMB_WIRE:-0}" = 1 ]; then
    sed -i -e "s|@WIRE@||" -e "s|@TCPHOST@|$SMBHOST|" "$HD/S/SMB-Work"
else
    sed -i -e "/@WIRE@/d" "$HD/S/SMB-Work"
fi
if [ "${AMINETXDUO_SMB_LOOP:-0}" = 1 ]; then
    sed -i -e "s|@LOOP@||" "$HD/S/SMB-Work"
else
    sed -i -e "/@LOOP@/d" "$HD/S/SMB-Work"
fi
if [ "${AMINETXDUO_SMB_TCPCTRL:-0}" = 1 ]; then
    sed -i -e "s|@TCPCTRL@||" \
           -e "s|@TCPHOST@|$SMBHOST|" \
           -e "s|@TCPPORT2@|$((PEERPORT + 101))|" \
           -e "s|@TCPPORT@|$((PEERPORT + 100))|" "$HD/S/SMB-Work"
else
    sed -i -e "/@TCPCTRL@/d" "$HD/S/SMB-Work"
fi

NCLINE="Echo >>DH0:smbcheck.txt \"RESULT nc445 rc=skipped\""
NETSTATLINE="Echo >>DH0:smbcheck.txt \"(this stack ships no netstat)\""
if [ "$HAVE_NC" = yes ]; then
    NCLINE="C:nc -v -w 5 $SMBHOST $SMBPORT >>DH0:smbcheck.txt
Echo >>DH0:smbcheck.txt \"RESULT nc445 rc=\$RC\""
fi
if [ "$HAVE_NETSTAT" = yes ]; then
    NETSTATLINE="C:netstat -a -s >>DH0:smbcheck.txt"
    SVCLINE="C:ShowNetServices TYPE _ipps._tcp SECONDS 4 >>DH0:smbcheck.txt"
fi

# The boot shell's half.  It brings the network up, says what it sees, starts
# the detached half, and then samples the connection table while that half is
# working.  It never waits on it.
cat > "$HD/S/SMB-Check" <<EOF
FailAt 9999
Stack 200000

Echo >DH0:smbcheck.txt "=== 1. the network"
${NG_IFUP:-C:AddNetInterface eth0} >>DH0:smbcheck.txt
Echo >>DH0:smbcheck.txt "RESULT addnet rc=\$RC"
C:ShowNetStatus >>DH0:smbcheck.txt

Echo >>DH0:smbcheck.txt "*N=== 2. the server, from this machine"
C:ping $SMBHOST -c 2 -t 10 >>DH0:smbcheck.txt
Echo >>DH0:smbcheck.txt "RESULT ping rc=\$RC"
$NCLINE
Echo >>DH0:smbcheck.txt "*N=== 3. connections before the mount"
$NETSTATLINE
${SVCLINE:-Echo >NIL: ""}

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
# tests/perf/peercap.sh:119-121 says the same thing about the same mistake.
#
# The `timeout` on the far side is the other half: killing the local ssh does
# not kill what it started there, so a server with no ceiling of its own
# outlives its run, holds the port, and the next run mounts it.

REMOTE_PY=""
REMOTE_PID="/tmp/smb2-testserver-$TAG.pid"
REMOTE_LOG="/tmp/smb2-testserver-$TAG.log"
stop_server() {
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
    REMOTE_PY=""
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

    # THE SAME iperf THE GUEST RUNS OVER LOOPBACK, OVER THE WIRE INSTEAD.
    # Loopback measures the stack; this measures the stack plus the SANA-II
    # path, uaenet.device and the emulator's network trap.  One tool, one
    # boot, one difference.
    if [ "${AMINETXDUO_SMB_WIRE:-0}" = 1 ]; then
        scp -q "$ROOT/tests/tools/iperfpeer.py" "$PEERHOST:/tmp/iperfpeer.py" ||
            { echo "cannot copy iperfpeer to $PEERHOST" >&2; exit 2; }
        ssh -o ConnectTimeout=10 "$PEERHOST" \
            "nohup timeout $((TIMEOUT + 120)) python3 /tmp/iperfpeer.py serve tcp \
                 --port 5001 --seconds $((TIMEOUT)) \
                 > /tmp/iperfpeer-$TAG.log 2>&1 &" >/dev/null 2>&1 || {
            echo "cannot start iperfpeer on $PEERHOST" >&2; exit 2; }
        sleep 2
        echo "==> iperf peer serving tcp on $PEERADDR:5001"
    fi

    # The RECEIVE direction, which is the one every slow case is in.  The peer
    # initiates, so this needs AMINETXDUO_SMB_ADDR: it retries until the
    # guest's `iperf -s' is listening.  The peer's own count is the oracle.
    if [ "${AMINETXDUO_SMB_RX:-0}" = 1 ]; then
        scp -q "$ROOT/tests/tools/iperfpeer.py" "$PEERHOST:/tmp/iperfpeer.py" \
            >/dev/null 2>&1 || true
        echo "==> iperf RECEIVE armed: waiting for the guest's address"
    fi

    # THE CONTROL THAT SEPARATES THE PROTOCOL FROM THE STACK.
    #
    # Same guest, same driver, same 32 MB, same destination volume -- but read
    # over a plain TCP stream instead of SMB2, so the only difference between
    # this number and the SMB one is smb2-handler/filesysbox and the SMB2
    # request/response cadence.  Without it a slow SMB copy cannot be told
    # apart from a slow stack, which is the question that has been open.
    #
    # A one-shot listener: it writes 32 MB and closes, and the close is what
    # ends the guest's read.
    if [ "${AMINETXDUO_SMB_TCPCTRL:-0}" = 1 ]; then
        TCPPORT=$((PEERPORT + 100))
        ssh -o ConnectTimeout=10 "$PEERHOST" \
            "nohup timeout $((TIMEOUT + 120)) sh -c \
                 'dd if=/dev/zero bs=1M count=32 2>/dev/null | \
                  nc -l -p $TCPPORT -q 1' > /tmp/tcpctrl-$TAG.log 2>&1 &" \
            >/dev/null 2>&1 || {
            echo "cannot start the TCP control on $PEERHOST" >&2; exit 2; }
        # A SECOND one-shot listener, so the same 32 MB can be taken twice:
        # once discarded and once written.  The pair is what separates our
        # socket path from the DOS write path -- nc is the only arm where the
        # WHOLE path is ours and the sender never waits, so it cannot hide a
        # request/response stall the way an SMB read does.
        ssh -o ConnectTimeout=10 "$PEERHOST" \
            "nohup timeout $((TIMEOUT + 120)) sh -c \
                 'dd if=/dev/zero bs=1M count=32 2>/dev/null | \
                  nc -l -p $((TCPPORT + 1)) -q 1' >> /tmp/tcpctrl-$TAG.log 2>&1 &" \
            >/dev/null 2>&1 || {
            echo "cannot start the second TCP control on $PEERHOST" >&2; exit 2; }
        sleep 2
        echo "==> TCP control: 32 MB from $PEERADDR:$TCPPORT (to NIL:) and" \
             "$((TCPPORT + 1)) (to DH0:)"
    fi
fi

# The TCP control runs on its own ports; without them the capture records
# the SMB conversation only and the control arm cannot be analysed at all.
CAPEXTRA=""
if [ "${AMINETXDUO_SMB_TCPCTRL:-0}" = 1 ]; then
    CAPEXTRA=" or port $((PEERPORT + 100)) or port $((PEERPORT + 101))"
fi

# ------------------------------------------------------------- the capture --

CAPFILE="$ROOT/build/smb-$TAG.pcap"
CAP_PID=""
if [ "$CAPTURE" = 1 ]; then
    rm -f "$CAPFILE"
    tcpdump -i "$BACKEND" -s 0 -U -w "$CAPFILE" \
        "(host $SMBHOST and (port $SMBPORT or port 139${CAPEXTRA:-})) or port 5353 or igmp" \
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
# ALLOCATED, not hashed: `smb-$TAG` hashed into the same 900 slots that
# tools/amiberry-run.sh used, so an SMB run and an unrelated arm in another
# checkout could land on one port and read each other's guest.
# tools/emu-rig-lock.sh has the mechanism.
# shellcheck source=../../tools/emu-rig-lock.sh
. "$ROOT/tools/emu-rig-lock.sh"
rig_claim_port "run-smbmount $TAG" || exit 2
PORT="$RIG_PORT"
: > "$SERIAL"

if [ -n "${AMINETXDUO_SMB_BOARD:-}" ]; then
    . "$ROOT/tools/emu-board.sh"
    BOARD_CFG=$(emu_board_lines "$AMINETXDUO_SMB_BOARD" "$MAC" "$BACKEND") ||
        { echo "!! unknown board $AMINETXDUO_SMB_BOARD" >&2; exit 2; }
    # AMINETXDUO_SMB_VENDOR=1 stages the BOARD VENDOR'S driver instead of ours.
    # Comparing two stacks on our own driver is not a comparison of the stacks:
    # anxnet.device is known to do worse under Roadshow, which charges Roadshow
    # for our driver.  The vendor driver is the common ground.
    if [ "${AMINETXDUO_SMB_VENDOR:-0}" = 1 ]; then
        . "$ROOT/tools/sana2-stage.sh"
        VDRV=$(sana2_driver_for "$AMINETXDUO_SMB_BOARD")
        VSRC="${AMINETXDUO_SANA2_DIR:-$HOME/amiga-assets/devs}/$VDRV"
        [ -f "$VSRC" ] || { echo "!! no vendor driver at $VSRC" >&2; exit 2; }
        cp "$VSRC" "$HD/Devs/Networks/$VDRV"
        chmod 755 "$HD/Devs/Networks/$VDRV"
    else
        ANX="${AMINETXDUO_ANXNET:-$BUILD/src/netdev/anxnet.device}"
        [ -f "$ANX" ] || { echo "!! no anxnet.device at $ANX" >&2; exit 2; }
        cp "$ANX" "$HD/Devs/Networks/anxnet.device"
        chmod 755 "$HD/Devs/Networks/anxnet.device"
    fi
    # CARD takes OUR driver's board name, not the emulator's config key, and
    # the two differ.  Getting it wrong fails the whole interface file, so the
    # mapping lives here rather than in a caller's env var.
    case "${AMINETXDUO_SMB_CARD:-$AMINETXDUO_SMB_BOARD}" in
        xsurf100z2|xsurf100z3) NDCARD=XSURF100 ;;
        xsurf)                 NDCARD=XSURF ;;
        ariadne2)              NDCARD=ARIADNE2 ;;
        ariadne)               NDCARD=ARIADNE ;;
        hydra)                 NDCARD=HYDRA ;;
        eb920)                 NDCARD=LANROVER ;;
        a2065)                 NDCARD=A2065 ;;
        ne2000_pcmcia)         NDCARD=PCMCIA ;;
        *)                     NDCARD=$(printf '%s' \
                                   "${AMINETXDUO_SMB_CARD:-$AMINETXDUO_SMB_BOARD}" \
                                   | tr 'a-z' 'A-Z') ;;
    esac
    [ "${AMINETXDUO_SMB_VENDOR:-0}" = 1 ] ||
        printf 'CARD=%s\n' "$NDCARD" >> "$HD/Devs/NetInterfaces/eth0"
fi

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
${BOARD_CFG:-${UAENET_CFG:-a2065_rom_file=:ENABLED
a2065_rom_options=mac=$MAC,$BACKEND}}
EOF

# THE CPU, AND WHY THIS BLOCK HAS TO EXIST.  This harness writes its own
# emulator config instead of going through tools/amiberry-run.sh, so it did not
# read AMINETXDUO_AMIBERRY_EXTRA and a caller passing cpu_model/cachesize on the
# command line had it SILENTLY DISCARDED.  Every SMB measurement taken before
# 2026-09-13 therefore ran on a stock 68020 A1200 at 14 MHz with no JIT, where
# smb2-handler through filesysbox is the bottleneck and the stack underneath it
# barely matters -- which is not the question this harness is asked.
if [ -n "${AMINETXDUO_AMIBERRY_EXTRA:-}" ]; then
    printf '%s\n' "${AMINETXDUO_AMIBERRY_EXTRA}" | tr ';' '\n' >> "$CFG"
    echo "==> extra config: ${AMINETXDUO_AMIBERRY_EXTRA}"
fi

# A CONFIG THAT CAN ONLY PRODUCE THE NAT IS KNOWABLE NOW, NOT AFTER A BOOT.
#
# uaenet.device takes no backend, so `sana2=true' with no emulated card is
# user-mode NAT every time -- see tools/emu-bridge.sh for what that costs.  The
# post-boot address check still runs; this is here so the two minutes are not
# spent first.
if [ -n "${UAENET_CFG:-}" ] && [ -z "${AMINETXDUO_SMB_BOARD:-}" ] \
   && [ "${AMINETXDUO_ALLOW_SLIRP:-0}" != 1 ]; then
    echo "!! AMINETXDUO_SMB_UAENET=1 gives amiberry's user-mode NAT, not a" >&2
    echo "   bridge: uaenet.device has no backend option. Throughput measured" >&2
    echo "   this way is the NAT's (18 s vs 3 s on a 32 MB SMB2 read)." >&2
    echo "   Use AMINETXDUO_SMB_BOARD=<card> for a bridged run, or" >&2
    echo "   AMINETXDUO_ALLOW_SLIRP=1 if the NAT is what you want." >&2
    exit 3
fi

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

echo "==> booting, ${TIMEOUT}s budget, ${SMB_DEVICE:-a2065.device} on $BACKEND as $MAC"
#
# --log IS NOT FREE, AND ON A THROUGHPUT ARM IT IS THE MEASUREMENT.
#
# amiberry's drawing.cpp:8307 prints "Denise queue without lock!" from
# waitqueue(), once per call, unthrottled, whenever thread_debug_lock is false
# -- which it is for the whole run of a headless guest.  write_log() takes a
# GLOBAL semaphore and does a vsnprintf + timestamp + SDL_Log write per line,
# on the emulation thread.  Every SMB arm measured before 2026-09-13 paid it:
# smb-q1024 1,346,127 of 1,346,649 lines, smb-smbC-ours 14,962,727 of
# 14,963,424 -- 99.9% in each.  A peer session measured 2.4 GB in one run and a
# guest starved so hard the SANA-II interface never came up at all.
#
# Nothing here reads this file: the result comes off the serial port and out of
# DH0:smbcheck.txt.  So it is off unless asked for, and when it IS asked for
# the flood is dropped at the pipe -- that still pays the formatting, but not
# the write.  A number taken with AMINETXDUO_SMB_EMULOG=1 is not a number.
ALOG="$ROOT/build/smb-$TAG-amiberry.log"
if [ "${AMINETXDUO_SMB_EMULOG:-0}" = 1 ]; then
    echo "==> --log is ON: this arm is instrumented, not measured"
    ( trap '' PIPE; exec "$AMIBERRY" --log -f "$CFG" ) \
        > >(grep -v --line-buffered 'Denise queue without lock' > "$ALOG") 2>&1 &
    EMU_PID=$!   # a pipeline's $! is grep, not the emulator; this is not one
else
    ( trap '' PIPE; exec "$AMIBERRY" -f "$CFG" ) > "$ALOG" 2>&1 &
    EMU_PID=$!
fi
(
    for _ in $(seq 1 60); do
        kill -0 "$EMU_PID" 2>/dev/null || exit 0
        nc 127.0.0.1 "$PORT" >> "$SERIAL" 2>/dev/null && exit 0
        sleep 0.5
    done
) &
SERIAL_PID=$!

# A PEER-INITIATED LEG CANNOT KNOW THE GUEST'S ADDRESS IN ADVANCE.
#
# A static address was tried and is not reachable on this bridge -- the guest
# comes up with it and pings nothing.  DHCP is what works, so the address is
# read back out of the serial log the moment the stack prints it, and the peer
# is told then.  Backgrounded: the boot wait below must not be delayed by it.
if [ "${AMINETXDUO_SMB_RX:-0}" = 1 ] && [ -n "$PEERHOST" ]; then
(
    GUESTIP=""
    for _ in $(seq 1 240); do
        GUESTIP=$(sed -n 's/.*online, address \([0-9.]*\).*/\1/p' "$SERIAL" \
                  2>/dev/null | head -1)
        [ -n "$GUESTIP" ] && break
        sleep 1
    done
    [ -n "$GUESTIP" ] || { echo "!! never saw the guest's address" >&2; exit 0; }
    echo "==> iperf RECEIVE: peer sending 32 MB to $GUESTIP:5002"
    ssh -o ConnectTimeout=10 "$PEERHOST" \
        "for i in \$(seq 1 90); do \
             python3 /tmp/iperfpeer.py send tcp $GUESTIP --port 5002 \
                 --seconds 60 && break; sleep 2; done" \
        > "$ROOT/build/smb-$TAG-iperfrx.log" 2>&1
) &
fi

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

# WAS THIS RUN ON THE BRIDGE, OR ON AMIBERRY'S NAT?  Every number below
# is void if it was the NAT, and nothing else in the run says so.
# tools/emu-bridge.sh carries the evidence and the escape hatch.
. "$ROOT/tools/emu-bridge.sh"
emu_bridge_assert "$SERIAL" "$HD/smbcheck.txt" || BRIDGE_BAD=1
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

if [ "${BRIDGE_BAD:-0}" = 1 ]; then
    echo "verdict=not_bridged"
    echo "!! the numbers above are the NAT's, not a measurement" >&2
    exit 3
fi
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
