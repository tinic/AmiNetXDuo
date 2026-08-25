#!/usr/bin/env bash
#
# A STANDING /console GUEST, WITH ITS SERIAL PORT ON A FILE.
#
#   tests/tools/console-instance.sh start|stop|status|serial|log [-a ADDR]
#                                   [-b BUILDDIR] [-m MODEL] [-B BACKEND]
#                                   [-d DEPTH] [-H CONSOLEHTML] [-r RUNDIR]
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

BUILD="${AMINETXDUO_BUILD:-$ROOT/build/cm}"
MODEL="${AMINETXDUO_EMU_MODEL:-A1200}"
BACKEND="${AMINETXDUO_CONSOLE_BACKEND:-ens18}"
ADDRESS="${CONSOLE_ADDRESS:-192.168.1.233}"
NETMASK=255.255.255.0
GATEWAY=192.168.1.1
PORT="${CONSOLE_PORT:-8080}"
DEPTH="${CONSOLE_DEPTH:-4}"
RUN="${CONSOLE_RUN:-$ROOT/build/console-instance}"
PAGE="${AMINETXDUO_CONSOLE_PAGE:-$ROOT/src/tools/web/console.html}"
SHELLPAGE="${AMINETXDUO_SHELL_PAGE:-$ROOT/src/tools/web/shell.html}"

ACTION="${1:-status}"
shift || true

while getopts "a:p:b:m:B:d:H:r:S:" opt; do
    case "$opt" in
        a) ADDRESS="$OPTARG" ;;
        p) PORT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        B) BACKEND="$OPTARG" ;;
        d) DEPTH="$OPTARG" ;;
        H) PAGE="$OPTARG" ;;
        r) RUN="$OPTARG" ;;
        S) SHELLPAGE="$OPTARG" ;;
        *) sed -n '3,8p' "$0" >&2; exit 2 ;;
    esac
done

HD="$RUN/dh0"
CFG="$RUN/console.uae"
PIDFILE="$RUN/amiberry.pid"
RDPIDFILE="$RUN/reader.pid"
SERIAL="$RUN/serial.log"
EMULOG="$RUN/amiberry.log"
MACFILE="$RUN/mac"
PORTFILE="$RUN/serialport"

say() { printf '%s=%s\n' "$1" "$2"; }

# Every kill in here names a pid this script wrote.  A bare pkill on this box
# takes out other agents' guests, which has happened.
stop_pid() {
    local f="$1" n=0 pid
    [ -f "$f" ] || return 0
    pid=$(cat "$f")
    [ -n "$pid" ] || { rm -f "$f"; return 0; }
    kill -TERM "$pid" 2>/dev/null || true
    while [ "$n" -lt 10 ] && kill -0 "$pid" 2>/dev/null; do sleep 1; n=$((n + 1)); done
    kill -KILL "$pid" 2>/dev/null || true
    rm -f "$f"
    printf '%s' "$pid"
}

running() {
    [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null
}

case "$ACTION" in
status)
    if running; then say pid "$(cat "$PIDFILE")"; say state running
    else say state down; fi
    say url "http://$ADDRESS:$PORT/console"
    say http "$(curl -s -m 5 -o /dev/null -w '%{http_code}' "http://$ADDRESS:$PORT/" || true)"
    say serial "$SERIAL"
    say serial_bytes "$( [ -f "$SERIAL" ] && wc -c < "$SERIAL" || echo 0)"
    say httpd_log "$HD/httpd.log"
    say httpd_log_bytes "$( [ -f "$HD/httpd.log" ] && wc -c < "$HD/httpd.log" || echo 0)"
    [ -f "$MACFILE" ] && say mac "$(cat "$MACFILE")"
    exit 0
    ;;
serial)
    [ -f "$SERIAL" ] || { say error "no serial log at $SERIAL"; exit 2; }
    cat "$SERIAL"
    exit 0
    ;;
log)
    [ -f "$HD/httpd.log" ] || { say error "no httpd log at $HD/httpd.log"; exit 2; }
    cat "$HD/httpd.log"
    exit 0
    ;;
stop)
    p=$(stop_pid "$PIDFILE"); say stopped "${p:-none}"
    r=$(stop_pid "$RDPIDFILE"); say reader_stopped "${r:-none}"
    exit 0
    ;;
start) ;;
*) sed -n '3,8p' "$0" >&2; exit 2 ;;
esac


if running; then
    say error "our own guest is already up, pid $(cat "$PIDFILE")"
    say RESULT INFRA
    exit 2
fi

if [ "$(curl -s -m 4 -o /dev/null -w '%{http_code}' "http://$ADDRESS:$PORT/" || true)" = "200" ]; then
    say error "something already answers on http://$ADDRESS:$PORT/"
    say RESULT INFRA
    exit 2
fi

HTTPD="$BUILD/src/tools/httpd"
BSD="$BUILD/src/bsdsocket/bsdsocket.library"
for f in "$HTTPD" "$BSD"; do
    [ -f "$f" ] || { say error "no $f"; say hint "cmake --build $BUILD --parallel"; say RESULT INFRA; exit 2; }
done

[ -f "$PAGE" ] || { say error "no console page at $PAGE"; say RESULT INFRA; exit 2; }

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for c in "$ROOT/build/a2065.device" "$HOME/amiga-assets/devs/a2065.device"; do
        [ -f "$c" ] && { A2065="$c"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || { say error "no a2065.device"; say RESULT INFRA; exit 2; }

model_var=$(printf '%s' "$MODEL" | tr '[:lower:]' '[:upper:]' | tr -c 'A-Z0-9' '_')
model_var=${model_var%_}
eval "KICKSTART=\${AMINETXDUO_KICKSTART_$model_var:-}"
KICKSTART="${KICKSTART:-${AMINETXDUO_KICKSTART:-}}"
[ -n "$KICKSTART" ] && [ -f "$KICKSTART" ] || { say error "no Kickstart for $MODEL"; say RESULT INFRA; exit 2; }

AMIBERRY="${AMIBERRY:-$(command -v amiberry || true)}"
[ -n "$AMIBERRY" ] || for c in "$HOME/amiberry/build/amiberry" "$HOME/amiberry/amiberry"; do
    [ -x "$c" ] && { AMIBERRY="$c"; break; }
done
[ -n "$AMIBERRY" ] || { say error "amiberry not found"; say RESULT INFRA; exit 2; }

# shellcheck source=tests/tools/wb31-sys.sh
. "$ROOT/tests/tools/wb31-sys.sh"
wb31_assemble "$ROOT/build/wb31-sys" >/dev/null || { say RESULT INFRA; exit 2; }
WB="$WB31_SYS"

mkdir -p "$RUN"
rm -rf "$HD"
mkdir -p "$HD/Public" "$HD/Console"
cp -R "$WB/." "$HD/"
cp "$HTTPD" "$HD/C/httpd"; chmod 755 "$HD/C/httpd"
mkdir -p "$HD/Libs" "$HD/Devs/Networks" "$HD/Devs/NetInterfaces"
cp "$BSD" "$HD/Libs/bsdsocket.library"
cp "$A2065" "$HD/Devs/Networks/a2065.device"
cp "$PAGE" "$HD/Console/console.html"
[ -f "$SHELLPAGE" ] && cp "$SHELLPAGE" "$HD/Console/shell.html"

[ -f "$PAGE.gz" ] && cp "$PAGE.gz" "$HD/Console/console.html.gz"
[ -f "$SHELLPAGE.gz" ] && cp "$SHELLPAGE.gz" "$HD/Console/shell.html.gz"

cat > "$HD/Devs/NetInterfaces/eth0" <<EOF
DEVICE=a2065.device
UNIT=0
CONFIGURE=STATIC
ADDRESS=$ADDRESS
NETMASK=$NETMASK
GATEWAY=$GATEWAY
EOF

echo "Hello from an Amiga." > "$HD/Public/readme.txt"
wb31_screenmode_prefs "$HD" "$DEPTH"

mkdir -p "$HD/Modes"
python3 - "$HD/Modes" <<'MODES'
import os, struct, sys
PAL, HIRES, LACE, SUPER = 0x00021000, 0x00008000, 0x00000004, 0x00008020
MODES = {
    "d2":     (PAL | HIRES,        640, 256, 2),
    "d4":     (PAL | HIRES,        640, 256, 4),
    "d8":     (PAL | HIRES,        640, 256, 8),
    "lace":   (PAL | HIRES | LACE, 640, 512, 4),
    "shires": (PAL | SUPER,       1280, 256, 2),
}
def chunk(tag, p):
    return tag + struct.pack(">L", len(p)) + p + (b"\0" if len(p) & 1 else b"")
for name, (did, w, h, d) in MODES.items():
    body = (b"PREF"
            + chunk(b"PRHD", struct.pack(">BB4B", 0, 0, 0, 0, 0, 0))
            + chunk(b"SCRM", struct.pack(">4L L HHHH", 0, 0, 0, 0, did, w, h, d, 0)))
    with open(os.path.join(sys.argv[1], name + ".prefs"), "wb") as fh:
        fh.write(b"FORM" + struct.pack(">L", len(body)) + body)
MODES

cat > "$HD/Modes/sweep" <<'SWEEP'
C:Wait 30
C:Copy DH0:Modes/d8.prefs ENV:Sys/screenmode.prefs
C:Wait 16
C:Copy DH0:Modes/lace.prefs ENV:Sys/screenmode.prefs
C:Wait 16
C:Copy DH0:Modes/shires.prefs ENV:Sys/screenmode.prefs
C:Wait 16
C:Copy DH0:Modes/d2.prefs ENV:Sys/screenmode.prefs
C:Wait 16
C:Copy DH0:Modes/d4.prefs ENV:Sys/screenmode.prefs
SWEEP

sed -e '/^EndCLI/d' "$WB/S/Startup-Sequence" > "$HD/S/Startup-Sequence"
cat >> "$HD/S/Startup-Sequence" <<EOF

FailAt 9999
C:Wait 6
Run >NIL: <NIL: C:Execute S:httpd-run
EOF
chmod 755 "$HD/S/Startup-Sequence"

TERMARGS=""
[ -f "$HD/Console/shell.html" ] && TERMARGS="-T PAGE DH0:Console/shell.html"
cat > "$HD/S/httpd-run" <<EOF
C:httpd DH0:Public $PORT -C CONSOLEPAGE DH0:Console/console.html $TERMARGS -v >DH0:httpd.log
EOF
chmod 755 "$HD/S/httpd-run"

# A fresh MAC per start, so the router's cache can never answer for a guest
# that did not come up.  Kept in a file because `status` is a separate run.
MAC=$(printf '02:41:4d:49:%02x:%02x' $(( ($$ >> 8) & 0xff )) $(( $$ & 0xff )))
printf '%s\n' "$MAC" > "$MACFILE"

# A free localhost port for the serial socket, so two instances on one host do
# not share one.
SERIAL_PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')
printf '%s\n' "$SERIAL_PORT" > "$PORTFILE"

export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-dummy}"
export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-dummy}"
[ "${SDL_VIDEODRIVER}" = "dummy" ] && unset DISPLAY WAYLAND_DISPLAY || true

cat > "$CFG" <<EOF
config_description=AmiNetXDuo standing console
use_gui=no
headless=true
quickstart=$MODEL,0
kickstart_rom_file=$KICKSTART
fastmem_size=8
floppy0type=-1
nr_floppies=0
uaehf0=dir,rw,DH0:DH0:$HD,0
a2065_rom_file=:ENABLED
a2065_rom_options=mac=$MAC,$BACKEND
serial_port=tcp://127.0.0.1:$SERIAL_PORT/wait
uaeserial=false
EOF

# NOT --log: 3.3 GB an hour, and it filled this machine once.  The emulator's
# own stdout is kept because it is a few lines, and truncated on every start so
# it can never be the thing that fills a disk either.
: > "$EMULOG"
( trap '' PIPE; exec "$AMIBERRY" -f "$CFG" ) >>"$EMULOG" 2>&1 &
printf '%s\n' "$!" > "$PIDFILE"

# The reader, retried: /wait holds the emulator until this connects, and the
# emulator has to have opened the listener first.  Appends, so a restart of the
# reader never loses what the last one had.
touch "$SERIAL"
(
    n=0
    while [ "$n" -lt 60 ]; do
        if kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
            nc 127.0.0.1 "$SERIAL_PORT" >> "$SERIAL" 2>/dev/null && exit 0
        else
            exit 0
        fi
        sleep 1
        n=$((n + 1))
    done
) &
printf '%s\n' "$!" > "$RDPIDFILE"

say pid "$(cat "$PIDFILE")"
say mac "$MAC"
say address "$ADDRESS:$PORT"
say url "http://$ADDRESS:$PORT/console"
say shell_url "http://$ADDRESS:$PORT/shell"
say serial "$SERIAL"
say serial_socket "127.0.0.1:$SERIAL_PORT"
say httpd_log "$HD/httpd.log"
say state starting
