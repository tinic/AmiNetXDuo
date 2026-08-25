#!/usr/bin/env bash
#
# THE REGRESSION TEST FOR NetSetup.
#
#   tests/tools/run-netsetup.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#                               [-A [-N board] [-B backend]] [-r TRANSCRIPT]
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=60
BUILD="${AMINETXDUO_BUILD:-build/cm}"
RUNNER=slirp
BOARD="${AMINETXDUO_AMIBERRY_BOARD:-a2065}"
IFACE="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"
REPLAY=""

while getopts "m:t:b:AN:B:r:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        A) RUNNER=amiberry ;;
        N) RUNNER=amiberry; BOARD="$OPTARG" ;;
        B) RUNNER=amiberry; IFACE="$OPTARG" ;;
        r) REPLAY="$OPTARG" ;;
        *) sed -n '3,7p' "$0" >&2; exit 2 ;;
    esac
done

case "$BUILD" in /*) ;; *) BUILD="${BUILD#./}" ;; esac

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-netsetup}"
HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"
RUN_RC=0

if [ -n "$REPLAY" ]; then
    [ -d "$REPLAY" ] || { echo "no such test drive: $REPLAY" >&2; exit 2; }
    HD="$REPLAY"
    echo "==> REPLAY of $HD: nothing was run, this only checks the checks"
else

for f in "$TOOLS/ToolsSmoke" "$TOOLS/NetSetup" "$TOOLS/AddNetInterface" \
         "$TOOLS/netstat" "$TOOLS/ping" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
done

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for candidate in \
        "$ROOT/build/a2065.device" \
        "$HOME/amiga-assets/devs/a2065.device"
    do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "No a2065.device found. Set AMINETXDUO_A2065=<path>." >&2
    exit 2
}


STAGE="$ROOT/build/netsetup-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"

rm -f "$STAGE/devs/NetInterfaces/"*

RESOLVER="$STAGE/devs/Internet/name_resolution"
grep -q '^domain localdomain' "$RESOLVER" || {
    echo "tests/netstack/devs/Internet/name_resolution no longer has the" \
         "'domain localdomain' line this test recognises the old file by." >&2
    exit 2
}

if [ "$RUNNER" = amiberry ]; then
    . "$ROOT/tools/sana2-stage.sh"

    if [ -z "${AMINETXDUO_SANA2_DRIVER:-}" ] && [ "$BOARD" != a2065 ]; then
        _want=$(sana2_driver_for "$BOARD")
        _have=$(sana2_local_driver "$_want")
        [ -n "$_have" ] && [ -f "$_have" ] &&
            export AMINETXDUO_SANA2_DRIVER="$_have"
    fi

    sana2_stage "$BOARD" "$STAGE/devs"
    echo "==> $BOARD: $SANA2_DRIVER, opened as '$SANA2_DEVICE'"
fi

cp "$BSD" "$STAGE/libs/bsdsocket.library"
for t in NetSetup AddNetInterface netstat ping; do
    cp "$TOOLS/$t" "$STAGE/$t"
done

cat > "$STAGE/commands.txt" <<'EOF'
SYS:netstat -i
SYS:NetSetup DEVICE=a2065.device UNIT=0 ADDRESS=10.0.2.15 NETMASK=255.255.255.0 GATEWAY=10.0.2.2 DNS=10.0.2.4 NOONLINE
SYS:netstat -i
SYS:NetSetup DEVICE=a2065.device UNIT=0 ADDRESS=10.0.2.15 NETMASK=255.255.255.0 GATEWAY=10.0.2.2 DNS=10.0.2.3 FORCE ONLINE
SYS:netstat -i
SYS:ping 10.0.2.2 -c 2 -t 10
SYS:NetSetup eth5 DEVICE=a2065.device UNIT=0 ADDRESS=10.0.2.15 NETMASK=255.255.255.0 NOONLINE
SYS:NetSetup eth5 DEVICE=a2065.device UNIT=0 ADDRESS=192.168.99.9 NOONLINE
SYS:NetSetup eth6 DEVICE=a2065.device UNIT=0 ADDRESS=10.0.2.15 NETMASK=255.255.255.0 NOONLINE
SYS:NetSetup eth6 DEVICE=a2065.device UNIT=0 ADDRESS=192.168.99.9 NOONLINE FORCE
SYS:NetSetup eth9 DEVICE=a2065.device UNIT=0 ADDRESS=notanaddress NOONLINE
SYS:NetSetup thisnameiswaytoolong DEVICE=a2065.device UNIT=0 DHCP NOONLINE
SYS:NetSetup eth8 DEVICE=a2065.device UNIT=0 DHCP NOONLINE QUIET
SYS:NetSetup eth7 DEVICE=a2065.device UNIT=0 DHCP NOONLINE
SYS:NetSetup eth4 DEVICE=a2065.device UNIT=0 DHCP
EOF


set +e
if [ "$RUNNER" = "amiberry" ]; then
    echo "==> booting $MODEL under Amiberry, $BOARD on $IFACE"
    "$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
        -t "$TIMEOUT" \
        "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
        "$STAGE/NetSetup" "$STAGE/AddNetInterface" "$STAGE/netstat" \
        "$STAGE/ping"
else
    echo "==> booting $MODEL with the A2065 on SLIRP"
    "$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" -t "$TIMEOUT" \
        "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
        "$STAGE/NetSetup" "$STAGE/AddNetInterface" "$STAGE/netstat" \
        "$STAGE/ping"
fi
RUN_RC=$?
set -e

fi  # not a replay

REPORT="$HD/tools.txt"
IFDIR="$HD/devs/NetInterfaces"
INET="$HD/devs/Internet"

if [ ! -f "$REPORT" ]; then
    echo "FAIL: the guest wrote no $REPORT (run rc=$RUN_RC)" >&2
    [ "$RUN_RC" = 124 ] &&
        echo "       rc 124 is the ${TIMEOUT}s timeout: the machine never" \
             "got as far as writing one." >&2
    exit 1
fi

echo
echo "===================== what the commands printed ====================="
cat "$REPORT"
echo "====================================================================="
echo "the drive afterwards:"
for d in "$IFDIR" "$INET"; do
    [ -d "$d" ] || { echo "  $d: NOT THERE"; continue; }
    echo "  $d:"
    ls -1 "$d" | sed 's/^/    /'
done
echo "====================================================================="
echo

FAILED=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }
pass() { echo "  ok: $*"; }

block() {
    awk -v banner="$1" -v want="$2" '
        index($0, "===== " banner " =====") == 1 { n++; if (n == want) { on = 1; next } }
        on && /^----- rc / { print; exit }
        on { print }
    ' "$REPORT"
}

rc_of() { block "$1" "$2" | sed -n 's/^----- rc \([0-9-]*\),.*/\1/p'; }
show()  { block "$1" "$2" | sed 's/^/       /' >&2; }

want_rc() { # banner nth expected description
    local got; got=$(rc_of "$1" "$2")
    if [ "$got" = "$3" ]; then pass "$4 (rc $got)"
    else fail "$4: expected rc $3, got '${got:-nothing}'"; show "$1" "$2"
    fi
}

says() { # banner nth extended-regexp description
    if block "$1" "$2" | grep -Eq "$3"; then pass "$4"
    else fail "$4"; show "$1" "$2"
    fi
}

silent_about() { # banner nth extended-regexp description
    if block "$1" "$2" | grep -Eq "$3"; then fail "$4"; show "$1" "$2"
    else pass "$4"
    fi
}


file_says() { # path extended-regexp description
    if [ ! -f "$1" ]; then
        fail "$3: there is no $1"
    elif grep -Eq "$2" "$1"; then
        pass "$3"
    else
        fail "$3"
        sed 's/^/       /' "$1" >&2
    fi
}

file_lacks() { # path extended-regexp description
    if [ ! -f "$1" ]; then
        fail "$3: there is no $1"
    elif grep -Eq "$2" "$1"; then
        fail "$3"
        sed 's/^/       /' "$1" >&2
    else
        pass "$3"
    fi
}

no_file() { # path description
    if [ -e "$1" ]; then fail "$2: $1 exists"
    else pass "$2"
    fi
}

ifaces() { block "SYS:netstat -i" "$1"; }

no_iface_yet() { # nth-netstat description
    if ifaces "$1" | grep -Eq '^eth0 '; then
        fail "$2"
        ifaces "$1" | sed 's/^/       /' >&2
    else
        pass "$2"
    fi
}

EXPECTED_BLOCKS=15
BLOCKS=$(grep -c '^===== SYS:' "$REPORT" || true)
if ! grep -q '^===== done, ' "$REPORT"; then
    LAST=$(grep '^===== SYS:' "$REPORT" | tail -1 | sed 's/^===== //; s/ =====$//')
    fail "THE RUN DID NOT FINISH: it stopped in '${LAST:-nothing ran}'," \
         "block $BLOCKS of $EXPECTED_BLOCKS (emulator rc=$RUN_RC," \
         "timeout ${TIMEOUT}s).  That command did not return."
elif [ "$BLOCKS" -eq "$EXPECTED_BLOCKS" ]; then
    pass "the machine booted once and ran all $EXPECTED_BLOCKS commands"
else
    fail "expected $EXPECTED_BLOCKS command blocks, found $BLOCKS:" \
         "the machine rebooted, or the list was truncated"
fi

WRITE1="SYS:NetSetup DEVICE=a2065.device UNIT=0 ADDRESS=10.0.2.15 NETMASK=255.255.255.0 GATEWAY=10.0.2.2 DNS=10.0.2.4 NOONLINE"
WRITE2="SYS:NetSetup DEVICE=a2065.device UNIT=0 ADDRESS=10.0.2.15 NETMASK=255.255.255.0 GATEWAY=10.0.2.2 DNS=10.0.2.3 FORCE ONLINE"

no_iface_yet 1 "the machine starts with no interface at all"

want_rc "$WRITE1" 1 0 "a fully specified call writes without asking anything"
says "$WRITE1" 1 "^Wrote DEVS:NetInterfaces/eth0$" \
     "the interface file is named eth0 when NAME is not given"
says "$WRITE1" 1 "^Wrote DEVS:Internet/routes$" \
     "GATEWAY writes the routes file"
says "$WRITE1" 1 "^Wrote DEVS:Internet/name_resolution$" \
     "DNS writes the resolver file"

silent_about "$WRITE1" 1 "The network is up" \
     "NOONLINE does not say the network came up"
no_iface_yet 2 "and netstat agrees nothing was started"

want_rc "$WRITE2" 1 0 "FORCE ONLINE rewrites the file and starts the network"
says "$WRITE2" 1 "^AddNetInterface eth0$" \
     "it starts it by running the real command"
says "$WRITE2" 1 "^The network is up.$" "and reports it came up"
if ifaces 3 | grep -Eq '^eth0 +[0-9]+ +10\.0\.2\.15 +up'; then
    pass "netstat -i shows eth0 up on the address NetSetup wrote"
else
    fail "eth0 is not up on 10.0.2.15 after ONLINE"
    ifaces 3 | sed 's/^/       /' >&2
fi
says "SYS:ping 10.0.2.2 -c 2 -t 10" 1 ", 0% packet loss" \
     "and the gateway NetSetup wrote answers a ping"
want_rc "SYS:ping 10.0.2.2 -c 2 -t 10" 1 0 "with ping succeeding"

E5A="SYS:NetSetup eth5 DEVICE=a2065.device UNIT=0 ADDRESS=10.0.2.15 NETMASK=255.255.255.0 NOONLINE"
E5B="SYS:NetSetup eth5 DEVICE=a2065.device UNIT=0 ADDRESS=192.168.99.9 NOONLINE"
want_rc "$E5A" 1 0 "eth5 is written"
says "$E5B" 1 "^NetSetup: DEVS:NetInterfaces/eth5 already exists$" \
     "a second call over the same name is refused by name"
want_rc "$E5B" 1 10 "and returns ERROR"
silent_about "$E5B" 1 "^Wrote " "and claims to have written nothing"

E6A="SYS:NetSetup eth6 DEVICE=a2065.device UNIT=0 ADDRESS=10.0.2.15 NETMASK=255.255.255.0 NOONLINE"
E6B="SYS:NetSetup eth6 DEVICE=a2065.device UNIT=0 ADDRESS=192.168.99.9 NOONLINE FORCE"
want_rc "$E6A" 1 0 "eth6 is written"
want_rc "$E6B" 1 0 "and FORCE writes over it"
says "$E6B" 1 "^Wrote DEVS:NetInterfaces/eth6$" "saying so"

BADIP="SYS:NetSetup eth9 DEVICE=a2065.device UNIT=0 ADDRESS=notanaddress NOONLINE"
says "$BADIP" 1 "^NetSetup: ADDRESS=notanaddress is not an address$" \
     "an ADDRESS that is not one is refused, quoting it"
want_rc "$BADIP" 1 10 "and returns ERROR"

BADNAME="SYS:NetSetup thisnameiswaytoolong DEVICE=a2065.device UNIT=0 DHCP NOONLINE"
says "$BADNAME" 1 '^NetSetup: "thisnameiswaytoolong" is not a usable interface name$' \
     "a name no file can have is refused, quoting it"
want_rc "$BADNAME" 1 10 "and returns ERROR"

QCALL="SYS:NetSetup eth8 DEVICE=a2065.device UNIT=0 DHCP NOONLINE QUIET"
LCALL="SYS:NetSetup eth7 DEVICE=a2065.device UNIT=0 DHCP NOONLINE"
want_rc "$QCALL" 1 0 "QUIET still writes"
says "$QCALL" 1 "^Wrote DEVS:NetInterfaces/eth8$" "and still says what it wrote"
silent_about "$QCALL" 1 "NetSetup: set up a network interface" \
     "but not the banner"
says "$LCALL" 1 "NetSetup: set up a network interface" \
     "and the same call without QUIET does print it"

DEFCALL="SYS:NetSetup eth4 DEVICE=a2065.device UNIT=0 DHCP"
want_rc "$DEFCALL" 1 0 "a call naming neither ONLINE nor NOONLINE writes"
says "$DEFCALL" 1 "AddNetInterface eth4" \
     "and tells the user how to start it"
silent_about "$LCALL" 1 "Start the network with" \
     "where NOONLINE does not offer even that"

file_says "$IFDIR/eth0" "^DEVICE += a2065\.device$" \
          "eth0 names the driver it was given"
file_says "$IFDIR/eth0" "^UNIT += 0$"                "and the unit"
file_says "$IFDIR/eth0" "^CONFIGURE += STATIC$"      "and a static configuration"
file_says "$IFDIR/eth0" "^ADDRESS += 10\.0\.2\.15$"  "and the address"
file_says "$IFDIR/eth0" "^NETMASK += 255\.255\.255\.0$" "and the netmask"
file_says "$IFDIR/eth0" "^STATE += UP$"              "and STATE = UP"

file_says "$INET/routes" "^DEFAULT += 10\.0\.2\.2$" \
          "DEVS:Internet/routes holds the GATEWAY as the default route"

file_says "$INET/name_resolution" "^NAMESERVER +10\.0\.2\.3$" \
          "the resolver file holds the DNS of the last call that wrote it"
file_says "$INET/name_resolution.old" "^NAMESERVER +10\.0\.2\.4$" \
          "and the one it replaced is beside it as .old, contents intact"
file_lacks "$INET/name_resolution" "^domain localdomain$" \
          "the staged file it replaced is gone from the live one"

if find "$IFDIR" -name '*.old' | grep -q .; then
    fail "DEVS:NetInterfaces holds a .old file, which the stack would read" \
         "as another interface"
    find "$IFDIR" -name '*.old' | sed 's/^/       /' >&2
else
    pass "no .old was left in DEVS:NetInterfaces, where it would be a phantom"
fi

file_says  "$IFDIR/eth5" "^ADDRESS += 10\.0\.2\.15$" \
           "eth5 still holds the address it was written with"
file_lacks "$IFDIR/eth5" "192\.168\.99\.9" \
           "and nothing of the call that was refused"
file_says  "$IFDIR/eth6" "^ADDRESS += 192\.168\.99\.9$" \
           "eth6 holds the address FORCE wrote over it"

no_file "$IFDIR/eth9" "the refused ADDRESS wrote no file"
no_file "$IFDIR/thisnameiswaytoolong" "nor did the refused name"

file_says "$IFDIR/eth8" "^CONFIGURE += DHCP$" "DHCP writes CONFIGURE = DHCP"
file_lacks "$IFDIR/eth8" "^ADDRESS " "and no address of its own"

echo
if [ "$FAILED" -eq 0 ]; then
    [ -n "$REPLAY" ] && { echo "checks pass against $HD (nothing was run)"; exit 0; }
    echo "PASS: NetSetup writes what it says it writes, and the stack can use it"
    exit 0
fi
echo "the transcript above is the whole run" >&2
exit 1
