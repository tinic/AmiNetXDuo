#!/usr/bin/env bash
#
# THE CONSOLE, AS OPPOSED TO THE PIPE.
#
#   tests/tools/run-wsconsole.sh [-t SECONDS] [-p HOSTPORT] [-P GUESTPORT]
#                                [-b BUILDDIR] [-m MODEL] [-c CPU] [-S PORT]
#                                [-B BACKEND] [-K]
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

mkdir -p "$STAGE/devs/Internet"
echo "hostname $HOSTNAME_" >> "$STAGE/devs/Internet/name_resolution"

echo "Hello from an Amiga." > "$STAGE/Public/readme.txt"
echo "<html><body><h1>Amiga</h1></body></html>" > "$STAGE/Public/index.html"
echo "in a drawer" > "$STAGE/Public/Docs/notes.txt"

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

WBLIBS="${AMINETXDUO_WBLIBS:-}"
if [ -n "$WBLIBS" ] && [ -d "$WBLIBS" ]; then
    cp -n "$WBLIBS"/*.library "$STAGE/libs/" 2>/dev/null || true
    chmod -R u+rw "$STAGE/libs"
fi

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

SSHKEY="${AMINETXDUO_WSCONSOLE_SSHKEY:-$ROOT/build/sshd-test/id_amiga}"
HAVE_SSHKEY=no
SSHKEY_WHY=""

db_convert()
{
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
        WIREMAC=$(grep -i "7990:" "$EMULOG" 2>/dev/null |
                  grep -oiE "([0-9a-f]{2}:){5}[0-9a-f]{2}" | tail -1 || true)
        [ -n "$WIREMAC" ] && break
    done

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
        echo "arms=0"
        echo "arms_ran=0"
        echo "arms_skipped=0"
        echo "checks=0"
        echo "failures=0"
        echo "result=infra"
        echo "!! no lease seen for the guest on $BACKEND" >&2
        exit 2
    fi

    if [ -z "$TARGET_ADDR" ]; then
        TARGET_ADDR="(unknown -- watch: tcpdump -i $BACKEND -n ether src $WIREMAC)"
    fi
    echo "guest_address=$TARGET_ADDR"
fi

if [ "$KEEP" = yes ] && [ "$BACKEND" != slirp ]; then
    FORWARD=unpolled
    BOOT_AT=$(( $(date +%s) - STARTED ))
else
    for _ in $(seq 1 "$BOOT_MAX"); do
        sleep 1
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
    echo "arms=0"
    echo "arms_ran=0"
    echo "arms_skipped=0"
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

if [ "$KEEP" = yes ]; then
    echo "ready=1"
    echo "url=http://${TARGET_ADDR}:${TARGET_PORT}/shell"
    echo "sshd_port=${SSHD_PORT}"
    echo "sshd_host=${SSHD_HOST:-(unset)}"
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
        done
    exit 0
fi

# ---------------------------------------------------------------- the drill --

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
done
wait "$DRILL"
DRILL_RC=$?
set -e
DRILL_SECS=$(( $(date +%s) - DRILL_AT ))

cat "$ROOT/build/wsconsole-drill.txt"

CHECKS=$(sed -n 's/^\([0-9]\{1,\}\) checks.*/\1/p' "$ROOT/build/wsconsole-drill.txt" | tail -1)
FAILS=$(sed -n 's/^[0-9]\{1,\} checks, \([0-9]\{1,\}\) failure.*/\1/p' "$ROOT/build/wsconsole-drill.txt" | tail -1)
ARMS=$(sed -n 's/^\([0-9]\{1,\}\) arms,.*/\1/p' "$ROOT/build/wsconsole-drill.txt" | tail -1)
ARMS_RAN=$(sed -n 's/^[0-9]\{1,\} arms, \([0-9]\{1,\}\) ran.*/\1/p' "$ROOT/build/wsconsole-drill.txt" | tail -1)
ARMS_SKIPPED=$(sed -n 's/^[0-9]\{1,\} arms, [0-9]\{1,\} ran, \([0-9]\{1,\}\) skipped.*/\1/p' "$ROOT/build/wsconsole-drill.txt" | tail -1)

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

echo "drill_seconds=$DRILL_SECS"
echo "arms=${ARMS:-0}"
echo "arms_ran=${ARMS_RAN:-0}"
echo "arms_skipped=${ARMS_SKIPPED:-0}"
echo "checks=${CHECKS:-0}"
echo "failures=${FAILS:-0}"

if [ -z "${CHECKS:-}" ] || [ -z "${ARMS:-}" ]; then
    echo "result=infra"
    echo "!! the drill printed no tally; it did not reach the end" >&2
    exit 2
fi

if [ "$((ARMS_RAN + ARMS_SKIPPED))" -ne "$ARMS" ]; then
    echo "result=fail"
    echo "!! $ARMS arms, $ARMS_RAN ran and $ARMS_SKIPPED skipped: the drill" >&2
    echo "!! did not account for all of them, so it stopped calling its own" >&2
    echo "!! cases somewhere and the check count says nothing." >&2
    exit 1
fi

if [ "$DRILL_RC" -ne 0 ] || [ "${FAILS:-1}" -ne 0 ]; then
    echo "result=fail"
    exit 1
fi

if [ "${ARMS_SKIPPED:-0}" -ne 0 ]; then
    echo "result=skipped"
    echo "!! $ARMS_SKIPPED of $ARMS arms did not run; everything that DID run" >&2
    echo "!! passed.  The SKIPPED lines above say what each one needed." >&2
    exit 77
fi

echo "result=pass"
exit 0
