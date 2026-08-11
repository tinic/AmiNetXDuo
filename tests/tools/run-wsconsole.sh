#!/usr/bin/env bash
#
# THE CONSOLE, AS OPPOSED TO THE PIPE.
#
#   tests/tools/run-wsconsole.sh [-t SECONDS] [-p HOSTPORT] [-P GUESTPORT]
#                                [-b BUILDDIR] [-m MODEL] [-c CPU] [-S PORT]
#                                [-B BACKEND] [-K]
#
# BRIDGED BY DEFAULT, AND WHY THAT IS NOT A PREFERENCE
#
#   `-B slirp` puts the guest behind NAT and forwards one port, which is what
#   this used to do and is convenient.  It is also a different network: the
#   guest reaches the build host at 10.0.2.2 through a userspace TCP stack
#   that is not the one a real machine talks to, and a defect that needs a
#   bridge to appear cannot appear there.  So the default is an interface
#   name, the guest takes a lease of its own, and the drill talks to it over
#   the LAN exactly as a browser would.
#
#   TWO BRIDGED GUESTS ARE NOT TWO MACHINES UNLESS THEY SAY SO.  The MAC is
#   set explicitly and defaults to one this harness owns; a run that leaves it
#   alone would collide with whatever else is on the wire, both would flap,
#   and the machine that was already up would stop answering.
#
#   -K stages and boots and then HOLDS the guest, without drilling it.  That
#   is how tests/tools/wsconsole-page.mjs is run: the page test needs a
#   Chromium-family browser, which the machine with the emulator on it does
#   not necessarily have, so the guest is held here and the browser drives it
#   from wherever there is one, through an ssh -L to the forwarded port.
#
# WHAT IT PROVES, AND WHY IT IS NOT run-wsterm.sh
#
#   run-wsterm.sh proves a Shell answers through a WebSocket.  This proves the
#   thing on the far end of it is a CONSOLE: that it answers
#   ACTION_SCREEN_MODE, that RAW mode reaches the page as a word, that a
#   password typed at ssh's prompt is never drawn, and that a program asking
#   how big the window is gets told.
#
#   The two suites are separate because they need different furniture.  This
#   one needs a Workbench commands drawer (Ed, More), an ssh client, and an
#   sshd on the build host to point it at; run-wsterm.sh needs none of those
#   and must keep running on a machine that has none of them.
#
# THE ONE THING IT CANNOT PROVE ON ITS OWN
#
#   The echo lives in the PAGE.  A drill that speaks the protocol and does not
#   echo would pass this trivially, which is why the password assertion here
#   is on the SERVER's contract -- `mode raw` arrives before the prompt can be
#   answered, and nothing of what is typed comes back -- and the page's half
#   is proved by tests/tools/wsconsole-page.mjs, which drives the real built
#   shell.html in a real browser and reads the characters off the screen.
#
# WHAT IT NEEDS
#
#   a2065.device (AMINETXDUO_A2065, or build/a2065.device), a Kickstart, a
#   68020 Release build, and:
#
#     AMINETXDUO_WBC=<dir>   a Workbench C: drawer.  Ed and More come from a
#                            licensed Workbench and are not ours to ship; the
#                            lab store has the ADFs and amitools unpacks one.
#                            Utilities/More is copied in beside C: if present.
#     AMINETXDUO_WBLIBS=<d> a Workbench LIBS: drawer.  Ed needs rexxsyslib and
#                            asl from it and refuses to start without them.
#     AMINETXDUO_SSH=<file>  an AmigaOS ssh client, staged as C:ssh.
#
#   Absent either, the arms that need it are SKIPPED and said to be skipped --
#   never quietly passed.
#
# THE SSH SERVER, AND WHY BRIDGED NEEDS TO BE TOLD WHERE IT IS
#
#   Behind SLIRP the guest reaches this machine at 10.0.2.2, so
#   clients/dropbear/sshd-testserver.sh here is the whole arrangement and
#   nothing has to be configured.
#
#   Bridged it cannot: a guest on this host's own port cannot exchange a frame
#   with this host (the long note further down measures it both ways), so an
#   sshd here is not a server the guest can see.  It has to be a DIFFERENT
#   machine on the LAN, and this harness cannot guess which:
#
#     AMINETXDUO_WSCONSOLE_SSHD_HOST=<addr>   where the guest finds an sshd
#     AMINETXDUO_WSCONSOLE_SSHD_USER=<name>   who to log in as ($USER)
#     AMINETXDUO_WSCONSOLE_SSHD=<port>        which port (2224)
#
#   Unset and bridged, the ssh arms print sshd=skipped and say why.  They are
#   never quietly passed and never pointed at an address that cannot answer.
#
#     AMINETXDUO_WSCONSOLE_SSHKEY=<file>   the identity, staged as DH0:sshkey
#
#   It must be in DROPBEAR's format, which is not OpenSSH's: dbclient answers
#   an OpenSSH private key with "String too long" and stops.  Default is
#   build/sshd-test/id_amiga, which clients/dropbear/sshd-testserver.sh makes
#   with a natively built dropbearkey and authorises on the server it starts.
#   A key in the other format is converted here with dropbearconvert when one
#   can be built, and the arm is skipped with the reason when it cannot.
#
# A TIMEOUT IS A DEFECT
#
#   Same two ceilings as run-wsterm.sh, reported as what they cost.  A run
#   that burns one exits 2: infrastructure, not a result.
#
# WHAT IT PRINTS
#
#     boot_seconds=NN
#     forward=ok|failed
#     sshd=ok|skipped
#     checks=NN
#     failures=NN
#     result=pass|fail|infra
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

WINDOW=420
HOSTPORT=18081
GUESTPORT=8080
MODEL=A1200
CPU=""
BUILD="${AMINETXDUO_BUILD:-build/cm}"
GUEST_IP=10.0.2.15
SSHD_PORT="${AMINETXDUO_WSCONSOLE_SSHD:-2224}"
SSHD_HOST="${AMINETXDUO_WSCONSOLE_SSHD_HOST:-}"
SSHD_USER="${AMINETXDUO_WSCONSOLE_SSHD_USER:-$(id -un)}"
KEEP=no
BACKEND="${AMINETXDUO_WSCONSOLE_BACKEND:-ens18}"

# An address and a name of this RUN's own, derived from the tag.
#
# tools/demo.sh can have one fixed address because there is one demo.  A test
# harness cannot: two agents running this script at the same time is the
# normal case, and a fixed default would give both guests the same address.
# Two a2065s with one address on one wire is the failure demo.sh already
# records -- leases fight, ARP caches flap, and the machine that was already
# up stops answering while httpd goes on binding 0.0.0.0 and reporting itself
# happily, which reads as stuck rather than as unplugged.
#
# So the last byte comes from the run tag.  The A2065 keeps only the last
# three bytes -- amiberry-run.sh says so and a2065.cpp is why -- so that is
# the byte that has to differ.  0x01 is every default-configured a2065 and
# 0x77 is the demo; both are stepped over rather than hashed into.
_tag="${AMINETXDUO_RUN_TAG:-wsconsole}"
_byte=$(printf '%s' "$_tag" | cksum | awk '{printf "%d", ($1 % 200) + 16}')
[ "$_byte" -eq 119 ] && _byte=120          # 0x77, tools/demo.sh
export AMINETXDUO_AMIBERRY_MAC="${AMINETXDUO_AMIBERRY_MAC:-$(printf '02:41:4d:49:00:%02x' "$_byte")}"
HOSTNAME_="${AMINETXDUO_WSCONSOLE_NAME:-anxd-$_tag}"

while getopts "t:p:P:b:m:c:S:B:K" opt; do
    case "$opt" in
        t) WINDOW="$OPTARG" ;;
        p) HOSTPORT="$OPTARG" ;;
        P) GUESTPORT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        c) CPU="$OPTARG" ;;
        S) SSHD_PORT="$OPTARG" ;;
        B) BACKEND="$OPTARG" ;;
        K) KEEP=yes ;;
        *) echo "usage: $0 [-t seconds] [-p hostport] [-P guestport]" \
                "[-b builddir] [-m model] [-c cpu] [-S sshdport]" \
                "[-B backend] [-K]" >&2; exit 2 ;;
    esac
done

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
PAGE="$ROOT/src/tools/web/shell.html"
# Both forms, so the browser this run drives is fetching the gzipped one --
# which is what a real browser will get and the only place that is proved.
PAGEGZ="$PAGE.gz"

for f in "$TOOLS/httpd" "$BSD" "$PAGE" "$PAGEGZ"; do
    [ -f "$f" ] || { echo "result=infra"; echo "missing $f, build the tree first" >&2; exit 2; }
done

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for candidate in "$ROOT/build/a2065.device" "$HOME/amiga-assets/devs/a2065.device"; do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "result=infra"
    echo "No a2065.device found.  Set AMINETXDUO_A2065=<path>." >&2
    exit 2
}

# ------------------------------------------------------------- staging ---
#
# run-wsterm.sh's shape, plus a C: drawer and the libraries an ssh client
# needs.  Never hand-assembled: the drawer layout, the NetInterfaces file and
# the devs tree are copied from the harness that already works.

STAGE="$ROOT/build/wsconsole-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs" "$STAGE/Public/Docs" "$STAGE/c"

cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
mkdir -p "$STAGE/devs/Networks"
cp "$A2065" "$STAGE/devs/Networks/a2065.device"
cp "$BSD" "$STAGE/libs/bsdsocket.library"
cp "$PAGE"   "$STAGE/shell.html"
cp "$PAGEGZ" "$STAGE/shell.html.gz"

cat > "$STAGE/devs/NetInterfaces/eth0" <<EOF
DEVICE=a2065.device
UNIT=0
CONFIGURE=DHCP
MDNS=YES
EOF

# A name to answer to, so a guest on the LAN is identifiable as this harness's
# rather than as one more lease.
mkdir -p "$STAGE/devs/Internet"
echo "hostname $HOSTNAME_" >> "$STAGE/devs/Internet/name_resolution"

echo "Hello from an Amiga." > "$STAGE/Public/readme.txt"
echo "<html><body><h1>Amiga</h1></body></html>" > "$STAGE/Public/index.html"
echo "in a drawer" > "$STAGE/Public/Docs/notes.txt"

# A file for Ed to edit and More to page.  Long enough that More has to stop
# for a keypress, which is the whole of what More is being asked to do here.
{
    echo "The quick brown fox jumps over the lazy dog."
    i=1
    while [ "$i" -le 120 ]; do
        printf 'line %03d of the paging test\n' "$i"
        i=$((i + 1))
    done
} > "$STAGE/Public/pagefile.txt"

WBC="${AMINETXDUO_WBC:-}"
HAVE_ED=no
HAVE_MORE=no
if [ -n "$WBC" ] && [ -d "$WBC" ]; then
    cp -R "$WBC/." "$STAGE/c/"
    chmod -R u+rw "$STAGE/c"
    [ -f "$STAGE/c/Ed" ]   && HAVE_ED=yes
    [ -f "$STAGE/c/More" ] && HAVE_MORE=yes
fi

# Workbench's LIBS:, if there is one.  Ed opens rexxsyslib.library and
# asl.library and stops with "Unable to open needed library" without them --
# which reads exactly like a console that did not answer, and cost a run
# before it was staged.  gadtools and icon are in ROM on 3.1 and are not the
# problem; the disk ones are.
WBLIBS="${AMINETXDUO_WBLIBS:-}"
if [ -n "$WBLIBS" ] && [ -d "$WBLIBS" ]; then
    # -n so ours are never overwritten; bsdsocket.library is the point of the
    # run.  All of them rather than a chosen few: tools/demo.sh records why,
    # which is that picking them out one at a time is how an evening goes.
    cp -n "$WBLIBS"/*.library "$STAGE/libs/" 2>/dev/null || true
    chmod -R u+rw "$STAGE/libs"
fi

# vim, if the asset store has it: the most demanding thing the console runs.
# Staged exactly as tools/demo.sh stages it, including ENV:VIM -- with that
# unset vim derives its path from the binary name, AmigaDOS reads "vim" as a
# volume, and the machine asks you to insert volume vim: in any drive.
EXTRA_DRAWERS=()
VIMBIN="${AMINETXDUO_WSCONSOLE_VIM:-$HOME/amiga-assets/apps/vim-9.1/vim}"
HAVE_VIM=no
if [ -f "$VIMBIN" ]; then
    cp -f "$VIMBIN" "$STAGE/c/vim"
    chmod u+rwx "$STAGE/c/vim"
    mkdir -p "$STAGE/s/vim" "$STAGE/env"
    printf 'S:vim' > "$STAGE/env/VIM"
    EXTRA_DRAWERS=("$STAGE/s" "$STAGE/env")
    HAVE_VIM=yes
fi

# An ssh identity, so the drill can actually LOG IN.
#
# Without one the ssh arm can only reach the password prompt and assert that
# the password is not echoed -- which is worth asserting and is NOT the
# feature.  A password prompt that does not echo is a program still in its
# prompt-and-read phase; the interactive session that comes after it is a
# different path through the console and has to be driven to be tested.
#
# The format is Dropbear's, not OpenSSH's.  dbclient reads an OpenSSH private
# key as a length-prefixed blob, finds a four-byte length where the base64 is,
# and stops with "String too long" -- which reads like a corrupt file and is
# not.  So the format is checked here rather than discovered on the guest, and
# converted when it can be.
SSHKEY="${AMINETXDUO_WSCONSOLE_SSHKEY:-$ROOT/build/sshd-test/id_amiga}"
HAVE_SSHKEY=no
SSHKEY_WHY=""

db_convert()
{
    # dropbearconvert, built natively from the pinned submodule, the same
    # recipe and the same build directory clients/dropbear/sshd-testserver.sh
    # uses for dropbearkey.
    local out="$ROOT/build/dropbear-host"

    if [ ! -x "$out/dropbearconvert" ]; then
        mkdir -p "$out"
        (
            cd "$out"
            [ -f localoptions.h ] || printf '#define DROPBEAR_SVR_DROP_PRIVS 0\n' > localoptions.h
            [ -f Makefile ] || "$ROOT/third_party/dropbear/configure" \
                --disable-zlib --disable-harden >configure.log 2>&1
            make PROGRAMS=dropbearconvert -j4 >convert.log 2>&1
        ) >/dev/null 2>&1 || return 1
    fi
    [ -x "$out/dropbearconvert" ] || return 1
    "$out/dropbearconvert" openssh dropbear "$1" "$2" >/dev/null 2>&1
}

if [ ! -f "$SSHKEY" ]; then
    SSHKEY_WHY="no identity at $SSHKEY (clients/dropbear/sshd-testserver.sh start makes one)"
elif head -c 11 "$SSHKEY" | grep -q -- "-----BEGIN"; then
    if db_convert "$SSHKEY" "$STAGE/sshkey"; then
        chmod u+rw "$STAGE/sshkey"
        HAVE_SSHKEY=yes
        echo "==> converted $SSHKEY from OpenSSH format with dropbearconvert"
    else
        SSHKEY_WHY="$SSHKEY is an OpenSSH key and dropbearconvert could not be built"
    fi
else
    cp -f "$SSHKEY" "$STAGE/sshkey"
    chmod u+rw "$STAGE/sshkey"
    HAVE_SSHKEY=yes
fi
[ -z "$SSHKEY_WHY" ] || echo "!! no ssh identity staged: $SSHKEY_WHY" >&2

SSHBIN="${AMINETXDUO_SSH:-}"
HAVE_SSH=no
if [ -n "$SSHBIN" ] && [ -f "$SSHBIN" ]; then
    cp -f "$SSHBIN" "$STAGE/c/ssh"
    chmod u+rwx "$STAGE/c/ssh"
    HAVE_SSH=yes
    # An ssh client built on newlib wants the IEEE double libraries, which a
    # real Workbench has in LIBS: and a staged drive does not.  Without them it
    # loads and dies with "mathieeedoubbas.library failed to load", which reads
    # like a broken binary.
    for m in mathieeedoubbas mathieeedoubtrans; do
        for src in "$HOME/amiga-assets/nglibs/$m.library" \
                   "$HOME/amiga-assets/libs/$m.library"; do
            [ -f "$src" ] && { cp -f "$src" "$STAGE/libs/"; break; }
        done
    done
fi

# ------------------------------------------------------------------ run ---

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-wsconsole}"
HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"

if [ "$BACKEND" = slirp ]; then
    export AMINETXDUO_AMIBERRY_EXTRA="slirp_redir=tcp:${HOSTPORT}:${GUESTPORT}:${GUEST_IP}"
    echo "==> httpd on the guest at :${GUESTPORT}, forwarded to 127.0.0.1:${HOSTPORT}"
else
    echo "==> httpd on the guest at :${GUESTPORT}, bridged on ${BACKEND}" \
         "as ${HOSTNAME_} (${AMINETXDUO_AMIBERRY_MAC})"
fi

CPUARG=()
[ -z "$CPU" ] || CPUARG=(-c "$CPU")

set +e
"$ROOT/tools/amiberry-run.sh" -N a2065 -B "$BACKEND" -m "$MODEL" "${CPUARG[@]}" \
    -t "$WINDOW" \
    -a "DH0:Public $GUESTPORT -T PAGE=DH0:shell.html TRACE" \
    "$TOOLS/httpd" "$STAGE/devs" "$STAGE/libs" "$STAGE/Public" \
    "$STAGE/shell.html" "$STAGE/shell.html.gz" "$STAGE/c" \
    "${EXTRA_DRAWERS[@]}" \
    ${HAVE_SSHKEY:+$([ "$HAVE_SSHKEY" = yes ] && echo "$STAGE/sshkey")} \
    > "$ROOT/build/wsconsole-emu.log" 2>&1 &
RUNNER=$!
set -e

cleanup() {
    if kill -0 "$RUNNER" 2>/dev/null; then
        kill "$RUNNER" 2>/dev/null || true
        wait "$RUNNER" 2>/dev/null || true
    fi
}
trap cleanup EXIT

# ------------------------------------------------------- is it there yet ---

BOOT_MAX=${AMINETXDUO_WSCONSOLE_BOOT:-180}
BOOT_AT=0
FORWARD=failed
STARTED=$(date +%s)

EMULOG="$ROOT/build/amiberry-$AMINETXDUO_RUN_TAG.log"
EMULOG_MAX=${AMINETXDUO_WSCONSOLE_LOGMAX:-33554432}

trim_emulog() {
    [ -f "$EMULOG" ] || return 0
    local size
    size=$(wc -c < "$EMULOG" 2>/dev/null || echo 0)
    [ "$size" -gt "$EMULOG_MAX" ] || return 0
    tail -c 4194304 "$EMULOG" > "$EMULOG.tail" 2>/dev/null || return 0
    mv "$EMULOG.tail" "$EMULOG"
}

# Where to talk to it.  Behind NAT that is the forwarded port on this machine;
# bridged it is a lease, and the lease is READ OFF THE WIRE rather than
# guessed -- the guest announces itself by ARP as soon as it has one, and the
# MAC to watch for is the one the emulator logged, not the one we asked for.
if [ "$BACKEND" = slirp ]; then
    TARGET_ADDR=127.0.0.1
    TARGET_PORT="$HOSTPORT"
else
    TARGET_ADDR=""
    TARGET_PORT="$GUESTPORT"

    WIREMAC=""
    for _ in $(seq 1 60); do
        sleep 2
        kill -0 "$RUNNER" 2>/dev/null || break
        # Case-insensitively, and with nothing assumed about what precedes it.
        # tools/demo.sh looks for a quoted board name before the address; this
        # emulator build logs the LANCE line unquoted and in upper case, so
        # that pattern silently matches nothing and the run dies as "no lease"
        # with a guest that booted perfectly.  Cost one run.
        WIREMAC=$(grep -i "7990:" "$EMULOG" 2>/dev/null |
                  grep -oiE "([0-9a-f]{2}:){5}[0-9a-f]{2}" | tail -1 || true)
        [ -n "$WIREMAC" ] && break
    done

    # ANY IPv4 packet FROM that MAC, and take its source.
    #
    # tools/demo.sh waits for an "ARP, Reply", and this guest never sends one:
    # it emits DHCP, duplicate-address ARP *Requests*, mDNS and IPv6 discovery,
    # and answers ARP only when something on the LAN asks -- which nothing here
    # does, because this machine cannot reach it at all (see the note below).
    # Waiting for a reply is waiting for something that is not coming; the
    # mDNS announcement carries the address and arrives unprompted.
    if [ -n "$WIREMAC" ]; then
        echo "wire_mac=$WIREMAC"
        for _ in $(seq 1 20); do
            TARGET_ADDR=$(timeout 15 tcpdump -i "$BACKEND" -n -c 4 \
                              "ether src $WIREMAC and ip" 2>/dev/null |
                          awk '{print $2}' |
                          grep -oE "^([0-9]{1,3}\.){3}[0-9]{1,3}" |
                          grep -v "^0\.0\.0\.0$" | head -1 || true)
            [ -n "$TARGET_ADDR" ] && break
            kill -0 "$RUNNER" 2>/dev/null || break
        done
    fi

    if [ -z "$TARGET_ADDR" ] && [ "$KEEP" != yes ]; then
        echo "forward=failed"
        echo "boot_seconds=0"
        echo "checks=0"
        echo "failures=0"
        echo "result=infra"
        echo "!! no lease seen for the guest on $BACKEND" >&2
        exit 2
    fi

    if [ -z "$TARGET_ADDR" ]; then
        # Held, and the address was not caught.  The guest is running; killing
        # it because a sniff missed would throw away the thing being held.
        TARGET_ADDR="(unknown -- watch: tcpdump -i $BACKEND -n ether src $WIREMAC)"
    fi
    echo "guest_address=$TARGET_ADDR"
fi

# Bridged and held: there is nothing to poll FROM here, because this is the
# machine emulating it and a guest bridged onto a host's own port cannot talk
# to that host (the long note further down says why).  Holding is the whole
# job in that case; whoever drives it from elsewhere finds out whether it
# answers, and finds out sooner than a poll here would.
if [ "$KEEP" = yes ] && [ "$BACKEND" != slirp ]; then
    FORWARD=unpolled
    BOOT_AT=$(( $(date +%s) - STARTED ))
else
    for _ in $(seq 1 "$BOOT_MAX"); do
        sleep 1
        trim_emulog
        kill -0 "$RUNNER" 2>/dev/null || break
        code=$(curl -s -m 3 -o /dev/null -w '%{http_code}' \
               "http://${TARGET_ADDR}:${TARGET_PORT}/" 2>/dev/null || true)
        if [ "$code" = "200" ]; then
            FORWARD=ok
            BOOT_AT=$(( $(date +%s) - STARTED ))
            break
        fi
    done
fi

echo "forward=$FORWARD"
echo "boot_seconds=$BOOT_AT"

if [ "$FORWARD" != ok ] && [ "$FORWARD" != unpolled ]; then
    echo "checks=0"
    echo "failures=0"
    echo "result=infra"
    echo "!! nothing answered on ${TARGET_ADDR}:${TARGET_PORT} within ${BOOT_MAX}s." >&2
    if [ "$BACKEND" != slirp ]; then
        cat >&2 <<'WHY'
!!
!! BRIDGED, AND THIS IS THE MACHINE RUNNING THE EMULATOR.
!!
!! A guest bridged onto a host's own physical port cannot exchange traffic
!! with that host.  The guest's frame leaves the port, and the switch does not
!! send it back out the port it arrived on -- so ARP never resolves in either
!! direction.  Measured both ways: the host cannot ping the guest, and `ssh`
!! from the guest to the host is silent for as long as you care to wait.
!!
!! It is not a fault in the guest and not a fault in the server.  Drive a
!! bridged guest from ANOTHER machine on the LAN:
!!
!!     on this machine:   tests/tools/run-wsconsole.sh -B <iface> -K
!!     on another:        tests/tools/wsterm-console.py <guest address> <port>
!!
!! -K boots it and holds it, and prints the address to aim at.
WHY
    fi
    sed -n '1,40p' "$HD/stdout.txt" 2>/dev/null >&2 || true
    exit 2
fi

# Held rather than drilled: see -K at the top.  The guest stays up until the
# emulator's own window runs out or somebody stops this script.
if [ "$KEEP" = yes ]; then
    echo "ready=1"
    echo "url=http://${TARGET_ADDR}:${TARGET_PORT}/shell"
    echo "sshd_port=${SSHD_PORT}"
    echo "sshd_host=${SSHD_HOST:-(unset)}"
    # The drill has to run somewhere that can reach the guest, which is any
    # machine on the LAN except this one.  Printed with its environment
    # already filled in, because the ssh arms are silently skipped without it
    # and a skipped arm read as a passing one for a whole release.
    echo "==> drive it from another machine on the LAN:"
    echo "      AMINETXDUO_WSCONSOLE_SSH=$([ "$HAVE_SSH" = yes ] && echo yes || echo no) \\"
    echo "      AMINETXDUO_WSCONSOLE_SSHKEY=$HAVE_SSHKEY \\"
    echo "      AMINETXDUO_WSCONSOLE_SSHD_PORT=$SSHD_PORT \\"
    echo "      AMINETXDUO_WSCONSOLE_SSHD_USER=$SSHD_USER \\"
    echo "      AMINETXDUO_WSCONSOLE_HOST=${SSHD_HOST:-<addr of an sshd>} \\"
    echo "      tests/tools/wsterm-console.py $TARGET_ADDR $TARGET_PORT"
    echo "==> holding the guest; Ctrl-C to stop it"
    while kill -0 "$RUNNER" 2>/dev/null; do
        sleep 5
        trim_emulog
    done
    exit 0
fi

# ---------------------------------------------------------------- the drill --
#
# WHERE THE GUEST FINDS AN sshd, AND THE ONE ADDRESS THAT IS NEVER IT
#
# Behind SLIRP the guest reaches this machine at 10.0.2.2 and this machine is
# the server; the port is probed on 127.0.0.1, which is the same listener.
#
# Bridged, this machine is the ONE machine on the LAN the guest cannot reach,
# so pointing the arm at this host's own address is pointing it at silence --
# and it was, which is how an ssh arm stayed green while ssh was broken.  The
# address has to be given, it is probed from here before the arm runs, and the
# arm is skipped and said to be skipped when it is not.

SSHD=skipped
SSHD_WHY=""
if [ "$BACKEND" = slirp ]; then
    GUEST_SSHD_HOST="${SSHD_HOST:-10.0.2.2}"
    PROBE_HOST=127.0.0.1
else
    GUEST_SSHD_HOST="$SSHD_HOST"
    PROBE_HOST="$SSHD_HOST"
fi

if [ "$HAVE_SSH" != yes ]; then
    SSHD_WHY="no ssh client staged (AMINETXDUO_SSH)"
elif [ -z "$GUEST_SSHD_HOST" ]; then
    SSHD_WHY="bridged, and AMINETXDUO_WSCONSOLE_SSHD_HOST names no server the guest can reach"
elif ! (exec 3<>/dev/tcp/"$PROBE_HOST"/"$SSHD_PORT") 2>/dev/null; then
    SSHD_WHY="nothing listening on ${PROBE_HOST}:${SSHD_PORT}"
    SSHD=absent
else
    SSHD=ok
fi
echo "sshd=$SSHD"
[ -z "$SSHD_WHY" ] || echo "==> ssh arms skipped: $SSHD_WHY"

DRILL_AT=$(date +%s)
set +e
AMINETXDUO_WSCONSOLE_ED="$HAVE_ED" \
AMINETXDUO_WSCONSOLE_MORE="$HAVE_MORE" \
AMINETXDUO_WSCONSOLE_SSH="$([ "$SSHD" = ok ] && echo yes || echo no)" \
AMINETXDUO_WSCONSOLE_SSHD_PORT="$SSHD_PORT" \
AMINETXDUO_WSCONSOLE_SSHD_USER="$SSHD_USER" \
AMINETXDUO_WSCONSOLE_SSHKEY="$HAVE_SSHKEY" \
AMINETXDUO_WSCONSOLE_VIM="$HAVE_VIM" \
AMINETXDUO_WSCONSOLE_HOST="$GUEST_SSHD_HOST" \
python3 -u "$ROOT/tests/tools/wsterm-console.py" "$TARGET_ADDR" "$TARGET_PORT" \
    > "$ROOT/build/wsconsole-drill.txt" 2>&1 &
DRILL=$!
while kill -0 "$DRILL" 2>/dev/null; do
    sleep 5
    trim_emulog
done
wait "$DRILL"
DRILL_RC=$?
set -e
DRILL_SECS=$(( $(date +%s) - DRILL_AT ))

cat "$ROOT/build/wsconsole-drill.txt"

CHECKS=$(sed -n 's/^\([0-9]\{1,\}\) checks.*/\1/p' "$ROOT/build/wsconsole-drill.txt" | tail -1)
FAILS=$(sed -n 's/^[0-9]\{1,\} checks, \([0-9]\{1,\}\) failure.*/\1/p' "$ROOT/build/wsconsole-drill.txt" | tail -1)

echo
echo "===================== the guest's own log ======================="
if [ -f "$HD/stdout.txt" ]; then
    cat "$HD/stdout.txt"
else
    echo "(the guest wrote no stdout.txt)"
fi
echo "================================================================"
echo

cleanup
trap - EXIT

if [ -f "$EMULOG" ]; then
    tail -400 "$EMULOG" > "$EMULOG.tail" && mv "$EMULOG.tail" "$EMULOG"
fi

echo "drill_seconds=$DRILL_SECS"
echo "checks=${CHECKS:-0}"
echo "failures=${FAILS:-0}"

if [ -z "${CHECKS:-}" ]; then
    echo "result=infra"
    echo "!! the drill printed no tally; it did not reach the end" >&2
    exit 2
fi

if [ "$DRILL_RC" -ne 0 ] || [ "${FAILS:-1}" -ne 0 ]; then
    echo "result=fail"
    exit 1
fi

echo "result=pass"
exit 0
