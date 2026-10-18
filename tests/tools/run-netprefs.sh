#!/usr/bin/env bash
#
# NetPrefs, table-driven, on a real Workbench 3.1: one boot per group.
#
#   tests/tools/run-netprefs.sh [-b BUILDDIR] [-g GROUP]... [-t SECONDS]
#                               [-o OUTDIR] [-k]
#
# Each row drives the window with WinClick (click, TEXT, DUMP), then reads
# back what it changed: the interface file, S:Network-Startup,
# DEVS:Internet/name_resolution, the gadgets after the window is opened
# again, and for the live group ShowNetStatus.  Groups: cold (no stack),
# live (a2065 on slirp), boot (two boots: configure, then the startup the
# configuration wrote).  Prints row_<id>=PASS|FAIL, row_<id>_fail=<check>
# for every check that failed, RESULT.  Exit 0 all rows passed, 1 a row
# failed, 3 a boot reached no verdict, 2 usage or environment.  -k keeps
# the drive.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT" || exit 2

BUILD="${AMINETXDUO_BUILD:-$ROOT/build/default}"
MODEL="${AMINETXDUO_EMU_MODEL:-A1200}"
TIMEOUT=600
OUTDIR="$ROOT/build/netprefs-out"
KEEP=0
GROUPS_RUN=()

while getopts "b:t:o:g:k" opt; do
    case "$opt" in
        b) BUILD="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        o) OUTDIR="$OPTARG" ;;
        g) GROUPS_RUN+=("$OPTARG") ;;
        k) KEEP=1 ;;
        *) sed -n '3,17p' "$0" >&2; exit 2 ;;
    esac
done
[ ${#GROUPS_RUN[@]} -gt 0 ] || GROUPS_RUN=(cold live boot)

say() { printf '%s=%s\n' "$1" "$2"; }

T="$BUILD/src/tools"
# Another NetPrefs binary, for the same rows against an earlier build or
# another configuration; AMINETXDUO_NETPREFS_IPV6=0 for one built without
# IPv6 (minimal, micro).
NETPREFS="${AMINETXDUO_NETPREFS:-$T/NetPrefs}"
IPV6="${AMINETXDUO_NETPREFS_IPV6:-1}"
NEED=("$NETPREFS" "$BUILD/tests/tools/WinClick" "$T/AddNetInterface"
      "$T/RemoveNetInterface" "$T/ShowNetStatus" "$T/Online" "$T/Offline"
      "$BUILD/src/bsdsocket/bsdsocket.library")
for f in "${NEED[@]}"; do
    [ -f "$f" ] || {
        say error "missing $f"
        say hint "cmake --build $BUILD --target tool_netprefs test_winclick tool_addnetinterface tool_removenetinterface tool_shownetstatus tool_online tool_offline bsdsocket_library"
        exit 2
    }
done
[ -f "${AMINETXDUO_A2065:-}" ] || { say error "no a2065.device; set AMINETXDUO_A2065"; exit 2; }

model_var=$(printf '%s' "$MODEL" | tr '[:lower:]' '[:upper:]' | tr -c 'A-Z0-9' '_')
model_var=${model_var%_}
eval "KICKSTART=\${AMINETXDUO_KICKSTART_$model_var:-}"
KICKSTART="${KICKSTART:-${AMINETXDUO_KICKSTART:-}}"
[ -n "$KICKSTART" ] && [ -f "$KICKSTART" ] || { say error "no Kickstart for $MODEL"; exit 2; }

AMIBERRY="${AMIBERRY:-$(command -v amiberry || true)}"
[ -n "$AMIBERRY" ] || for c in "$HOME/amiberry/build/amiberry" "$HOME/amiberry/amiberry"; do
    [ -x "$c" ] && { AMIBERRY="$c"; break; }
done
[ -n "$AMIBERRY" ] || { say error "amiberry not found; set AMIBERRY=<path>"; exit 2; }

HD="$ROOT/build/netprefs-dh0"
# shellcheck source=tools/emu-rig-lock.sh
. "$ROOT/tools/emu-rig-lock.sh"
rig_claim_drive "$HD" "netprefs in $ROOT" none || exit $?
# shellcheck source=tests/tools/wb31-sys.sh
. "$ROOT/tests/tools/wb31-sys.sh"
wb31_assemble "$ROOT/build/wb31-sys" >/dev/null || exit 2
WB="$WB31_SYS"

# ------------------------------------------------------------- the drive --

stage() {
    rm -rf "$HD"
    mkdir -p "$HD"
    cp -R "$WB/." "$HD/"
    local t
    cp "$NETPREFS" "$HD/C/NetPrefs"
    for t in AddNetInterface RemoveNetInterface ShowNetStatus Online Offline; do
        cp "$T/$t" "$HD/C/$t"
    done
    cp "$BUILD/tests/tools/WinClick" "$HD/C/WinClick"
    # Screens for a person to look at, when wbgrab is built.
    [ -f "$T/wbgrab" ] && cp "$T/wbgrab" "$HD/C/wbgrab"
    # A program holding the library open, as httpd does on a real machine.
    [ -f "$T/ping" ] && cp "$T/ping" "$HD/C/ping"
    chmod 755 "$HD/C/"*
    cp "$BUILD/src/bsdsocket/bsdsocket.library" "$HD/Libs/bsdsocket.library"
    mkdir -p "$HD/Devs/NetInterfaces" "$HD/Devs/Networks" "$HD/Devs/Internet" "$HD/np"
    cp "$AMINETXDUO_A2065" "$HD/Devs/Networks/a2065.device"
    printf '%s\n' "DEVICE=a2065.device" "UNIT=0" "CONFIGURE=DHCP" "MTU=1500" \
        > "$HD/Devs/NetInterfaces/ETH0"

    # Every page of the window, the panel cycle's own value naming each.
    cat > "$HD/S/npall" <<'EOF'
.KEY out/A
.BRA {
.KET }
C:WinClick "AmiNetXDuo Network" DUMP >>{out}
C:WinClick "AmiNetXDuo Network" 2 >NIL:
C:Wait 1
C:WinClick "AmiNetXDuo Network" DUMP >>{out}
C:WinClick "AmiNetXDuo Network" 2 >NIL:
C:Wait 1
C:WinClick "AmiNetXDuo Network" DUMP >>{out}
C:WinClick "AmiNetXDuo Network" 2 >NIL:
C:Wait 1
C:WinClick "AmiNetXDuo Network" DUMP >>{out}
C:WinClick "AmiNetXDuo Network" 2 >NIL:
C:Wait 1
C:WinClick "AmiNetXDuo Network" DUMP >>{out}
C:WinClick "AmiNetXDuo Network" 2 >NIL:
C:Wait 1
EOF
    # A file, or missing=1.
    cat > "$HD/S/npfile" <<'EOF'
.KEY file/A,out/A
.BRA {
.KET }
If EXISTS {file}
  Type {file} >{out}
Else
  Echo "missing=1" >{out}
EndIf
EOF
    chmod 644 "$HD/S/npall" "$HD/S/npfile"
}

startup_with() {
    local ss
    ss=$(find "$WB/S" -maxdepth 1 -iname 'startup-sequence' | head -1)
    rm -f "$HD/S/$(basename "$ss")"
    sed -e '/^EndCLI/d' "$ss" > "$HD/S/Startup-Sequence"
    printf '\n%s\n' "$1" >> "$HD/S/Startup-Sequence"
    chmod 755 "$HD/S/Startup-Sequence"
}

# ------------------------------------------------- guest script builders --
# The script is built line by line; PAGE tracks the panel cycle so that
# `page N' can click it round to N.

SCRIPT=""
PAGE=0
ROW=""
emit() { SCRIPT+="$*"$'\n'; }
W='"AmiNetXDuo Network"'
row()    { ROW="$1"; emit "Echo \"row=$1\" >>DH0:np/trace.txt"; }
open_np(){ emit "Run >NIL: ${1:-C:NetPrefs}"; emit "C:Wait 5"; PAGE=0; }
close_np(){ emit "C:WinClick $W 36 >NIL:"; emit "C:Wait 2"; }
click()  { emit "C:WinClick $W $* >>DH0:np/trace.txt"; emit "C:Wait 1"; }
text()   { emit "C:WinClick $W $1 TEXT \"$2\" >>DH0:np/trace.txt"; }
page()   {
    local n=$(( ($1 - PAGE + 5) % 5 ))
    [ "$n" = 0 ] || click 2 TIMES "$n"
    PAGE=$1
}
save()   { click 32; emit "C:Wait ${1:-2}"; }
apply()  { click 33; emit "C:Wait ${1:-20}"; }
dumpall(){ emit "Execute S:npall DH0:np/$ROW-$1.txt"; }
grab()   { [ -f "$T/wbgrab" ] && emit "C:wbgrab TO DH0:np/$ROW-$1.pfs >NIL:"; }
dump()   { emit "C:WinClick $W DUMP >DH0:np/$ROW-$1.txt"; }
file()   { emit "Execute S:npfile $1 DH0:np/$ROW-$2.txt"; }
cap()    { emit "$1 >DH0:np/$ROW-$2.txt"; }
# A requester: present=1 or 0 in <tag>, then dismissed by its last button.
req()    {
    emit "C:WinClick \"${2:-AmiNetXDuo NetPrefs}\" DUMP >DH0:np/$ROW-$1.txt"
    emit "C:WinClick \"${2:-AmiNetXDuo NetPrefs}\" ${3:-0} >NIL:"
    emit "C:Wait 1"
}

# ------------------------------------------------------ host assertions --

FAILED=0
NOVERDICT=0
ROWFAIL=()
f_of() { printf '%s/np/%s-%s.txt' "$HD" "$1" "$2"; }
fail() { ROWFAIL+=("$1:$2"); }
# has ROW TAG REGEX / hasnt ROW TAG REGEX: a line of that capture.
has()   { tr -d '\r' < "$(f_of "$1" "$2")" 2>/dev/null | grep -Eiq -- "$3" || fail "$1" "$2~/$3/"; }
hasnt() { ! tr -d '\r' < "$(f_of "$1" "$2")" 2>/dev/null | grep -Eiq -- "$3" || fail "$1" "$2!~/$3/"; }
count() {
    local n
    n=$(tr -d '\r' < "$(f_of "$1" "$2")" 2>/dev/null | grep -Eic -- "$3")
    [ "$n" = "$4" ] || fail "$1" "$2#/$3/=$n!=$4"
}
# gad ROW TAG PAGE KEY VALUE: in a dumpall capture, gadget KEY on PAGE.
gad() {
    local got
    got=$(tr -d '\r' < "$(f_of "$1" "$2")" 2>/dev/null | awk -v p="$3" -v k="$4" -F= '
        /^g2\.cycle=/ { page = $2 }
        page == p && $1 == k { sub(/^[^=]*=/, ""); print; exit }')
    [ "$got" = "$5" ] || fail "$1" "$2:p$3.$4=$got!=$5"
}
# req ROW TAG 1|0: a requester was (not) up.
reqd() {
    if [ "$3" = 1 ]; then has "$1" "$2" '^window='; else has "$1" "$2" '^error=no window'; fi
}

report() {
    local r="$1" e any=0
    for e in "${ROWFAIL[@]}"; do
        [ "${e%%:*}" = "$r" ] || continue
        say "row_${r}_fail" "${e#*:}"
        any=1
    done
    say "row_$r" "$([ $any = 0 ] && echo PASS || echo FAIL)"
    [ $any = 0 ] || FAILED=1
}

# ------------------------------------------------------------- the groups --

ROWS_cold=()
build_cold() {
    # shellcheck disable=SC2034  # read through the name ROWS_<group>
    ROWS_cold=(wb load progdir static todhcp spaceform boot drawer wildcard
               new newdup remove device tuning general ipv6 dns dnsbad dnsone dnsdhcp
               badstatic)
    # From Workbench: the icon left out at 62,146 (48x26), double-clicked.
    row wb
    emit "C:WinClick \"Workbench\" X 86 Y 156 TIMES 2 >>DH0:np/trace.txt"
    emit "C:Wait 8"
    dump open
    save
    dump status
    close_np

    row load
    open_np
    dumpall dump
    close_np

    # Resident: no program directory, as from a dock or a launcher.
    row progdir
    emit "Resident NetPrefs C:NetPrefs PURE"
    open_np NetPrefs
    save 4
    req sysreq "System Request" 0
    dump after
    apply 8
    req sysreq2 "System Request" 0
    req sysreq3 "System Request" 0
    req failed
    close_np
    emit "Resident NetPrefs REMOVE"

    row static
    open_np
    page 1
    click 11 TIMES 3
    text 12 192.168.5.10
    text 13 255.255.255.0
    text 14 192.168.5.1
    save
    dump status
    file DEVS:NetInterfaces/ETH0 file
    file DEVS:Internet/name_resolution res
    close_np
    open_np
    dumpall reopen
    close_np

    row todhcp
    open_np
    page 1
    click 11
    save
    dump status
    file DEVS:NetInterfaces/ETH0 file
    close_np

    # KEYWORD value, as the guide allows and AmiTCP files are written.
    row spaceform
    open_np
    click 1 AT 14
    page 1
    click 11
    save
    file DEVS:NetInterfaces/ETH1 file
    close_np

    row boot
    open_np
    click 21
    save
    file S:Network-Startup on
    click 21
    save
    file S:Network-Startup off
    click 21
    save
    file S:Network-Startup again
    close_np

    # A drawer installation: AmiNetXDuo:C holds the commands.
    row drawer
    emit "MakeDir DH0:anxd DH0:anxd/C"
    emit "Copy C:AddNetInterface DH0:anxd/C/ QUIET"
    emit "Assign AmiNetXDuo: DH0:anxd"
    open_np
    click 1 AT 14
    click 21
    save
    file S:Network-Startup file
    close_np
    emit "Assign AmiNetXDuo: REMOVE"

    row wildcard
    emit "Copy S:Network-Startup S:Network-Startup.np QUIET"
    emit "Echo \"C:AddNetInterface DEVS:NetInterfaces/~(#?.info) QUIET\" >S:Network-Startup"
    open_np
    dumpall dump
    click 21
    save
    req req1
    req req2
    file S:Network-Startup file
    close_np
    emit "Copy S:Network-Startup.np S:Network-Startup QUIET"

    row new
    open_np
    click 3
    text 4 WLAN0
    page 3
    text 6 prism2.device
    save
    dump status
    file DEVS:NetInterfaces/WLAN0 file
    close_np

    # New with a name that differs from a definition only in case.
    row newdup
    open_np
    click 3
    text 4 eth0
    page 3
    text 6 other.device
    save
    req exists
    file DEVS:NetInterfaces/ETH0 file
    close_np

    row remove
    open_np
    click 1 AT 22
    click 35
    req confirm "AmiNetXDuo NetPrefs" 1
    dump status
    cap "List DEVS:NetInterfaces/ LFORMAT=%N" list
    close_np

    row device
    open_np
    page 3
    text 8 1
    text 9 XSURF100
    text 10 02:11:22:33:44:55
    click 23
    click 24
    click 25
    save
    file DEVS:NetInterfaces/ETH0 file
    close_np
    open_np
    dumpall reopen
    close_np

    row tuning
    open_np
    page 4
    text 26 50
    save
    req bad
    text 26 1400
    text 27 8192
    text 28 16
    text 29 4
    text 30 8
    save
    file DEVS:NetInterfaces/ETH0 file
    close_np
    open_np
    dumpall reopen
    close_np

    row general
    open_np
    text 22 200
    save
    req bad
    text 22 -5
    text 5 myamiga
    click 19
    click 20
    save
    file DEVS:NetInterfaces/ETH0 file
    close_np
    open_np
    dumpall reopen
    close_np

    row ipv6
    open_np
    page 2
    click 15
    text 16 2001:db8::5/64
    text 18 fe80::1
    save
    file DEVS:NetInterfaces/ETH0 file
    close_np

    row dns
    open_np
    page 1
    click 11 TIMES 3
    text 12 192.168.5.10
    text 13 255.255.255.0
    text 37 10.0.2.3
    text 38 1.1.1.1
    save
    grab ipv4
    dump status
    file DEVS:Internet/name_resolution res
    close_np
    open_np
    dumpall reopen
    close_np

    row dnsbad
    open_np
    page 1
    text 37 dns.example
    save
    req bad
    file DEVS:Internet/name_resolution res
    close_np

    row dnsone
    open_np
    page 1
    text 38 ""
    save
    file DEVS:Internet/name_resolution res
    close_np

    # Back to DHCP: the static name servers are offered for removal.
    row dnsdhcp
    open_np
    page 1
    click 11
    save
    req offer "AmiNetXDuo NetPrefs" 1
    file DEVS:Internet/name_resolution res
    file DEVS:NetInterfaces/ETH0 file
    close_np

    row badstatic
    open_np
    page 1
    click 11 TIMES 3
    text 12 ""
    save
    req bad
    file DEVS:NetInterfaces/ETH0 file
    close_np
}

stage_cold() {
    cp "$NETPREFS" "$HD/NetPrefs"
    cp "$ROOT/install/NetPrefs.info" "$HD/NetPrefs.info"
    python3 - "$ROOT/install/tools/layout-icons.py" "$HD/NetPrefs.info" <<'PY'
import importlib.util, pathlib, sys
spec = importlib.util.spec_from_file_location("layout", sys.argv[1])
layout = importlib.util.module_from_spec(spec)
spec.loader.exec_module(layout)
layout.edit(pathlib.Path(sys.argv[2]), (62, 146))
PY
    printf ':NetPrefs\n' > "$HD/.backdrop"
    printf '%s\n' "; resolver" ";nameserver 192.168.1.1" "domain example.org" \
        "hostname np" > "$HD/Devs/Internet/name_resolution"
    printf '%s\n' "DEVICE a2065.device" "UNIT 0" "CONFIGURE STATIC" \
        "ADDRESS 10.1.1.5" "NETMASK 255.0.0.0" "GATEWAY 10.1.1.1" "# keep me" \
        > "$HD/Devs/NetInterfaces/ETH1"
}

check_cold() {
    gad wb open 0 g31.text "Loaded."
    gad wb status 0 g31.text "Saved."

    gad load dump 0 g4.str ETH0
    gad load dump 1 g11.cycle 1
    gad load dump 1 g12.disabled 1
    gad load dump 4 g26.num 1500
    gad load dump 0 g31.text "Loaded."

    reqd progdir sysreq 0
    reqd progdir sysreq2 0
    reqd progdir failed 1
    gad progdir after 0 g31.text "Saved."

    gad static status 1 g31.text "Saved."
    has static file '^CONFIGURE *= *STATIC'
    has static file '^ADDRESS *= *192\.168\.5\.10$'
    has static file '^NETMASK *= *255\.255\.255\.0$'
    has static file '^GATEWAY *= *192\.168\.5\.1$'
    has static file '^MTU *= *1500$'
    gad static reopen 1 g11.cycle 0
    gad static reopen 1 g12.str 192.168.5.10
    gad static reopen 1 g14.str 192.168.5.1
    gad static reopen 1 g12.disabled 0

    has todhcp file '^CONFIGURE *= *DHCP'
    hasnt todhcp file '^(ADDRESS|NETMASK|GATEWAY)'

    has spaceform file '^CONFIGURE *= *DHCP'
    count spaceform file '^CONFIGURE *=' 1
    hasnt spaceform file '^(ADDRESS|NETMASK|GATEWAY)[ =]'
    has spaceform file '^# keep me'

    has boot on '^C:AddNetInterface DEVS:NetInterfaces/ETH0 QUIET'
    has boot off '^; *C:AddNetInterface DEVS:NetInterfaces/ETH0 QUIET'
    count boot again 'AddNetInterface DEVS:NetInterfaces/ETH0' 1
    hasnt boot again '^;.*AddNetInterface DEVS:NetInterfaces/ETH0'

    has drawer file '^AmiNetXDuo:C/AddNetInterface DEVS:NetInterfaces/ETH1 QUIET'

    gad wildcard dump 0 g21.checked 1
    gad wildcard dump 0 g31.text "Starts at boot (wildcard)."
    reqd wildcard req1 1
    reqd wildcard req2 1
    count wildcard file '.' 1

    gad new status 3 g31.text "Saved."
    has new file '^DEVICE *= *prism2\.device'
    has new file '^CONFIGURE *= *DHCP'

    reqd newdup exists 1
    has newdup file '^DEVICE *= *a2065\.device'
    hasnt newdup file 'other\.device'

    gad remove status 0 g31.text "Removed (.disabled backup kept)."
    reqd remove confirm 1
    has remove list '^WLAN0\.disabled\.info$'
    hasnt remove list '^WLAN0$'

    has device file '^UNIT *= *1$'
    has device file '^CARD *= *XSURF100$'
    has device file '^HARDWAREADDRESS *= *02:11:22:33:44:55$'
    has device file '^DOWNGOESOFFLINE *= *YES'
    has device file '^REQUIRESINITDELAY *= *YES'
    has device file '^FILTER *= *EVERYTHING'
    gad device reopen 3 g8.num 1
    gad device reopen 3 g9.str XSURF100
    gad device reopen 3 g23.checked 1
    gad device reopen 3 g25.checked 1

    reqd tuning bad 1
    has tuning file '^MTU *= *1400$'
    has tuning file '^RXBUFFER *= *8192$'
    has tuning file '^IPREQUESTS *= *16$'
    has tuning file '^ARPREQUESTS *= *4$'
    has tuning file '^WRITEREQUESTS *= *8$'
    gad tuning reopen 4 g27.num 8192

    reqd general bad 1
    has general file '^PRIORITY *= *-5$'
    has general file '^ID *= *myamiga$'
    has general file '^MDNS *= *YES'
    has general file '^STATE *= *DOWN'
    gad general reopen 0 g22.num -5
    gad general reopen 0 g20.checked 0

    if [ "$IPV6" = 1 ]; then
        has ipv6 file '^CONFIGURE6 *= *STATIC'
        has ipv6 file '^ADDRESS6 *= *2001:db8::5/64'
        has ipv6 file '^GATEWAY6 *= *fe80::1'
    else
        hasnt ipv6 file '^(CONFIGURE6|ADDRESS6|GATEWAY6)'
        gad load dump 2 g15.disabled 1
    fi

    count static res '^nameserver' 0
    gad static reopen 1 g37.str ""
    gad static reopen 1 g37.disabled 0

    gad dns status 1 g31.text "Saved."
    has dns res '^nameserver 10\.0\.2\.3$'
    has dns res '^nameserver 1\.1\.1\.1$'
    count dns res '^nameserver' 2
    has dns res '^;nameserver 192\.168\.1\.1$'
    has dns res '^hostname np$'
    has dns res '^domain example\.org$'
    gad dns reopen 1 g37.str 10.0.2.3
    gad dns reopen 1 g38.str 1.1.1.1
    gad dns reopen 1 g37.disabled 0

    reqd dnsbad bad 1
    count dnsbad res '^nameserver' 2

    has dnsone res '^nameserver 10\.0\.2\.3$'
    count dnsone res '^nameserver' 1

    reqd dnsdhcp offer 1
    count dnsdhcp res '^nameserver' 0
    has dnsdhcp res '^hostname np$'
    has dnsdhcp file '^CONFIGURE *= *DHCP'

    reqd badstatic bad 1
    has badstatic file '^CONFIGURE *= *DHCP'
}

# shellcheck disable=SC2034
ROWS_live=(shell start offline static gwchange todhcp)
build_live() {
    # The commands NetPrefs runs, from the Shell first.
    row shell
    cap "C:AddNetInterface ETH0" addif
    emit "C:Wait 5"
    cap "C:ShowNetStatus" sns
    cap "C:RemoveNetInterface ETH0" rmif
    emit "C:Wait 3"

    row start
    open_np
    apply 25
    req sysreq "System Request"
    req failed
    dump status
    cap "C:ShowNetStatus" sns
    cap "C:ShowNetStatus ROUTES" routes
    close_np

    row offline
    open_np
    click 34
    emit "C:Wait 3"
    dump off
    cap "C:ShowNetStatus INTERFACES" snsoff
    click 34
    emit "C:Wait 8"
    dump on
    cap "C:ShowNetStatus INTERFACES" snson
    close_np

    # With another program holding the stack, Save & Start does not
    # restart it.
    row static
    emit "Run >NIL: C:ping -c 400 127.0.0.1"
    open_np
    page 1
    click 11 TIMES 3
    text 12 10.0.2.20
    text 13 255.255.255.0
    text 14 10.0.2.2
    text 37 10.0.2.9
    apply 15
    req failed
    dump status
    cap "C:ShowNetStatus" sns
    cap "C:ShowNetStatus ROUTES" routes
    close_np

    row gwchange
    open_np
    page 1
    text 14 10.0.2.4
    apply 15
    req failed
    cap "C:ShowNetStatus ROUTES" routes
    close_np

    row todhcp
    open_np
    page 1
    click 11
    apply 1
    req offer "AmiNetXDuo NetPrefs" 1
    emit "C:Wait 20"
    dump status
    cap "C:ShowNetStatus" sns
    cap "C:ShowNetStatus ROUTES" routes
    close_np
}
check_live() {
    has shell sns '^Network stack: +running'

    reqd start sysreq 0
    reqd start failed 0
    gad start status 0 g31.text "Saved and restarted."
    has start sns '^Network stack: +running'
    has start sns '^ +address +10\.0\.2\.15 '
    has start routes '^default +10\.0\.2\.2 .*ETH0'

    gad offline off 0 g0.text Offline
    gad offline on 0 g0.text Online

    reqd static failed 0
    # ping holds the stack, which reads name_resolution when it starts.
    gad static status 1 g31.text "Saved and restarted; DNS at the next start."
    has static sns '^ +address +10\.0\.2\.20 '
    has static routes '^default +10\.0\.2\.2 .*ETH0'

    reqd gwchange failed 0
    has gwchange routes '^default +10\.0\.2\.4 .*ETH0'
    count gwchange routes '^default' 1

    reqd todhcp offer 1
    has todhcp sns '^ +address +10\.0\.2\.15 '
    has todhcp routes '^default +10\.0\.2\.2 .*ETH0'
}

# Configured in one boot, started by what that wrote in the next.
# shellcheck disable=SC2034
ROWS_boot=(reboot)
build_boot() {
    row reboot
    open_np
    click 21
    page 1
    click 11 TIMES 3
    text 12 10.0.2.20
    text 13 255.255.255.0
    text 14 10.0.2.2
    text 37 10.0.2.3
    save
    file S:Network-Startup startup
    close_np
}
build_boot2() {
    ROW=reboot
    emit "Execute S:Network-Startup"
    emit "C:Wait 5"
    cap "C:ShowNetStatus" sns
    cap "C:ShowNetStatus ROUTES" routes
}
check_boot() {
    has reboot startup '^C:AddNetInterface DEVS:NetInterfaces/ETH0 QUIET'
    has reboot sns '^ +address +10\.0\.2\.20 '
    has reboot sns '^Default route: +10\.0\.2\.2'
    has reboot routes '^default +10\.0\.2\.2 .*ETH0'
    has reboot sns '^Name servers: +10\.0\.2\.3'
    hasnt reboot sns 'from DEVS:Internet'
}

# --------------------------------------------------------------- the run --

export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-dummy}"
export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-dummy}"
[ "${SDL_VIDEODRIVER}" = "dummy" ] && unset DISPLAY WAYLAND_DISPLAY || true

EMU_PID=""
cleanup() { [ -n "$EMU_PID" ] && kill -TERM "$EMU_PID" 2>/dev/null; EMU_PID=""; }
trap cleanup EXIT INT TERM HUP

BOOT_STATUS=0
boot() {
    local name="$1" net="$2" cfg="$ROOT/build/netprefs-$1.uae" elapsed=0
    rm -f "$HD/.done"
    cat > "$cfg" <<EOF
config_description=AmiNetXDuo netprefs $name
use_gui=no
headless=true
quickstart=$MODEL,0
kickstart_rom_file=$KICKSTART
fastmem_size=8
floppy0type=-1
nr_floppies=0
uaehf0=dir,rw,DH0:DH0:$HD,0
EOF
    [ "$net" = 1 ] && printf 'a2065_rom_file=:ENABLED\na2065_rom_options=mac=02:%02x:%02x:%02x:%02x:%02x,slirp\n' \
        $((RANDOM % 256)) $((RANDOM % 256)) $((RANDOM % 256)) \
        $((RANDOM % 256)) $((RANDOM % 256)) >> "$cfg"
    ( trap '' PIPE; exec "$AMIBERRY" --log -f "$cfg" ) \
        >"$ROOT/build/amiberry-netprefs-$name.log" 2>&1 &
    EMU_PID=$!
    BOOT_STATUS=124
    while [ "$elapsed" -lt "$TIMEOUT" ]; do
        if [ -f "$HD/.done" ]; then BOOT_STATUS=0; break; fi
        kill -0 "$EMU_PID" 2>/dev/null || { BOOT_STATUS=125; break; }
        sleep 1
        elapsed=$((elapsed + 1))
    done
    say "${name}_boot_seconds" "$elapsed"
    kill -TERM "$EMU_PID" 2>/dev/null
    wait "$EMU_PID" 2>/dev/null
    EMU_PID=""
}

run_group() {
    local g="$1" net=0
    SCRIPT=""
    emit "FailAt 9999"
    emit "C:Wait 6"
    stage
    "stage_$g" 2>/dev/null || true
    "build_$g"
    emit "Echo >DH0:.done done"
    startup_with "$SCRIPT"
    [ "$g" = cold ] || net=1
    boot "$g" "$net"
    if [ "$BOOT_STATUS" = 0 ] && declare -F "build_${g}2" >/dev/null; then
        SCRIPT=""
        emit "FailAt 9999"
        emit "C:Wait 6"
        "build_${g}2"
        emit "Echo >DH0:.done done"
        startup_with "$SCRIPT"
        boot "${g}2" "$net"
    fi
    if [ "$BOOT_STATUS" != 0 ]; then
        say "${g}_verdict" "none_$BOOT_STATUS"
        NOVERDICT=1
        return
    fi
    ROWFAIL=()
    "check_$g"
    local r
    local -n rows="ROWS_$g"
    for r in "${rows[@]}"; do report "$r"; done
}

mkdir -p "$OUTDIR"
for g in "${GROUPS_RUN[@]}"; do
    declare -F "build_$g" >/dev/null || { say error "unknown group $g"; exit 2; }
    run_group "$g"
    rm -rf "${OUTDIR:?}/$g"
    cp -R "$HD/np" "$OUTDIR/$g"
done

[ "$KEEP" = 1 ] || rm -rf "$HD"
say outdir "$OUTDIR"
if [ "$NOVERDICT" = 1 ]; then say RESULT NOVERDICT; exit 3; fi
say RESULT "$([ "$FAILED" = 0 ] && echo PASS || echo FAIL)"
exit "$FAILED"
