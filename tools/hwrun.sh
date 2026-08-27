#!/usr/bin/env bash
#
# RUN A COMMAND LIST ON A MACHINE THAT IS ALREADY SWITCHED ON.
#
#   tools/hwrun.sh [-A TARGET] [-p PORT] [-t SECONDS] [-w SECONDS]
#                  [-o OUTDIR] [-D DRAWER] [-H HOSTADDR] [-d]
#                  <commands.txt> <file> ...
#
# WHY THIS EXISTS
#
# tools/amiberry-run.sh drives a guest by building a drive and booting it.
# It cannot drive the real A1200: that machine is not an emulator, it holds
# the only 3c589 this project has, and no emulator models one.  Four backlog
# rows were open on the difference.
#
# The control surface is the one the machine already serves.  httpd -T gives
# a Shell over /shell, and our own `fetch` on the far end pulls the staged
# files out of an HTTP server started here.  Nothing is assumed about the
# machine beyond that: not the name of its boot drive, not the drawer httpd
# serves, not WebDAV being writable.  RAM: is where the run happens and RAM:
# exists on every Amiga.
#
# It writes <OUTDIR>/tools.txt in ToolsSmoke's own format, which is the file
# tests/tools/run-*.sh already assert against, so a physical arm reuses a
# harness's assertions rather than growing a second set.
#
# ABSENCE IS NOT A FAULT.  The machine is often off.  A target that does not
# resolve, does not answer ICMP or does not answer HTTP exits 3 with
# RESULT=skip, the convention tests/perf/run-ackscope.sh already uses: no
# measurement is a different thing from a failed one.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

TARGET="${AMINETXDUO_HW_TARGET:-amiga-1200.local}"
PORT="${AMINETXDUO_HW_PORT:-80}"
CMDTIME=120
RETURNWAIT=180
OUT=""
DRAWER="${AMINETXDUO_HW_DRAWER:-RAM:hwrun}"
HOSTADDR="${AMINETXDUO_HW_HOSTADDR:-}"
DETACH=no

usage() { sed -n '3,8p' "$0" >&2; }

while getopts "A:p:t:w:o:D:H:dh" opt; do
    case "$opt" in
        A) TARGET="$OPTARG" ;;
        p) PORT="$OPTARG" ;;
        t) CMDTIME="$OPTARG" ;;
        w) RETURNWAIT="$OPTARG" ;;
        o) OUT="$OPTARG" ;;
        D) DRAWER="$OPTARG" ;;
        H) HOSTADDR="$OPTARG" ;;
        d) DETACH=yes ;;
        h) usage; exit 0 ;;
        *) usage; exit 2 ;;
    esac
done
shift $((OPTIND - 1))

[ "$#" -ge 1 ] || { usage; exit 2; }

CMDFILE="$1"; shift
[ -f "$CMDFILE" ] || { echo "hwrun: no command list at $CMDFILE" >&2; exit 2; }

TAG="${AMINETXDUO_RUN_TAG:-hwrun}"
OUT="${OUT:-$ROOT/build/hwrun-$TAG}"
rm -rf "$OUT"; mkdir -p "$OUT"

# ------------------------------------------------------------- the target --
#
# `amiga-1200.local` is an mDNS name and DNS never answers for it, so a
# resolver that only knows DNS reports "Name or service not known" whether the
# machine is running or not.  That is not evidence about the machine, and it
# was read as evidence once.  Four ways are tried and the one that answered is
# printed, so a skip says which question was asked.

resolve_addr=""
resolve_how=""

hw_resolve() {
    local name="$1" a=""

    case "$name" in
        [0-9]*.[0-9]*.[0-9]*.[0-9]*) resolve_addr="$name"
                                     resolve_how=literal; return 0 ;;
    esac

    a=$(getent ahostsv4 "$name" 2>/dev/null | awk 'NR==1 { print $1 }')
    [ -z "$a" ] || { resolve_addr="$a"; resolve_how=dns; return 0; }

    # The same host under the resolver's own search domain.  A lab whose DNS
    # carries the DHCP lease answers for `amiga-1200` when it will not answer
    # for `amiga-1200.local`.
    case "$name" in
        *.local)
            a=$(getent ahostsv4 "${name%.local}" 2>/dev/null |
                awk 'NR==1 { print $1 }')
            [ -z "$a" ] || { resolve_addr="$a"; resolve_how=dns-search
                             return 0; } ;;
    esac

    if command -v avahi-resolve >/dev/null 2>&1; then
        a=$(avahi-resolve -4 -n "$name" 2>/dev/null | awk '{ print $2 }')
        [ -z "$a" ] || { resolve_addr="$a"; resolve_how=mdns; return 0; }
    fi

    # A host that does have mDNS, when this one has not.  The lab's Linux
    # boxes are not all the same in this.
    if [ -n "${AMINETXDUO_HW_RESOLVER:-}" ]; then
        a=$(ssh -o ConnectTimeout=10 "$AMINETXDUO_HW_RESOLVER" \
            "avahi-resolve -4 -n '$name' 2>/dev/null | awk '{ print \$2 }'" \
            2>/dev/null)
        [ -z "$a" ] || { resolve_addr="$a"
                         resolve_how="mdns:$AMINETXDUO_HW_RESOLVER"; return 0; }
    fi

    return 1
}

echo "target_name=$TARGET"
if ! hw_resolve "$TARGET"; then
    echo "target_addr=unresolved"
    echo "resolved_by=none"
    echo "RESULT=skip"
    echo "hwrun: nothing on this network answers to '$TARGET'.  DNS, the" >&2
    echo "  resolver's search domain and mDNS were all asked.  The machine" >&2
    echo "  is off, or it has not taken a lease yet.  Nothing here waits." >&2
    exit 3
fi
ADDR="$resolve_addr"
echo "target_addr=$ADDR"
echo "resolved_by=$resolve_how"

# ------------------------------------------------------------ is it there --
#
# ICMP and HTTP are asked separately and both are reported.  The pair is the
# diagnosis: a machine that answers ping and not HTTP has lost httpd, and a
# machine that answers neither has lost the machine.  After a guru there is
# no ICMP, which is what makes the distinction worth printing.

hw_icmp() { ping -c 2 -W 2 "$ADDR" >/dev/null 2>&1 && echo yes || echo no; }

# python3 and not curl.  This runs wherever the target can be reached, and
# that is not always a machine with curl on it: playhouse4 has none, and the
# emulator host cannot reach its own bridged guest at all.  python3 is
# already required here for tests/tools/hwshell.py.
hw_http() {
    python3 -c '
import socket, sys
try:
    s = socket.create_connection((sys.argv[1], int(sys.argv[2])), 8)
    s.sendall(b"HEAD / HTTP/1.0\r\nHost: %s\r\n\r\n" % sys.argv[1].encode())
    sys.exit(0 if s.recv(16).startswith(b"HTTP/") else 1)
except Exception:
    sys.exit(1)
' "$ADDR" "$PORT" 2>/dev/null && echo yes || echo no
}

ICMP=$(hw_icmp)
HTTP=$(hw_http)
echo "hw_icmp=$ICMP"
echo "hw_http=$HTTP"

if [ "$HTTP" != yes ]; then
    echo "RESULT=skip"
    if [ "$ICMP" = yes ]; then
        echo "hwrun: $ADDR answers ICMP but nothing serves HTTP on port" >&2
        echo "  $PORT.  The machine is up and httpd is not running on it." >&2
    else
        echo "hwrun: $ADDR answers neither ICMP nor HTTP.  It resolves, so" >&2
        echo "  the name is known here; the machine is not on the wire." >&2
    fi
    exit 3
fi

# -------------------------------------------------------- what to send it --

STAGE="$OUT/send"
mkdir -p "$STAGE"
cp "$CMDFILE" "$STAGE/commands.txt"

SEND_NAMES=(commands.txt)
for f in "$@"; do
    [ -e "$f" ] || { echo "hwrun: no such file to send: $f" >&2; exit 2; }
    cp "$f" "$STAGE/$(basename "$f")"
    SEND_NAMES+=("$(basename "$f")")
done
SENT=${#SEND_NAMES[@]}
echo "files_sent=$SENT"

# Where the guest reaches this machine.  Not guessable from in here when the
# host has several addresses, so the one on the route to the target is taken
# and -H overrides it.
if [ -z "$HOSTADDR" ]; then
    HOSTADDR=$(ip -4 -o route get "$ADDR" 2>/dev/null |
               sed -n 's/.* src \([0-9.]*\).*/\1/p' | head -1)
fi
[ -n "$HOSTADDR" ] || {
    echo "hwrun: cannot work out this machine's address on the route to" >&2
    echo "  $ADDR.  Give it with -H." >&2
    exit 2; }
echo "host_addr=$HOSTADDR"

# A port of our own, out of the way of the harness blocks documented in
# tests/tools/run-payverify.sh.
HTTPPORT=$((40000 + ($(printf '%s' "$TAG" | cksum | cut -d' ' -f1) % 900)))
SERVER_PID=""
stop_server() {
    [ -n "$SERVER_PID" ] || return 0
    kill "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=""
}
trap stop_server EXIT INT TERM HUP

( cd "$STAGE" && exec python3 -m http.server "$HTTPPORT" \
    --bind "$HOSTADDR" ) > "$OUT/server.log" 2>&1 &
SERVER_PID=$!
sleep 1
kill -0 "$SERVER_PID" 2>/dev/null || {
    echo "hwrun: the file server did not start on $HOSTADDR:$HTTPPORT" >&2
    cat "$OUT/server.log" >&2
    exit 2; }
echo "host_fileport=$HTTPPORT"

# ------------------------------------------------------------- the run --
#
# `fetch` is ours and it is in C: on any machine running our httpd, so the
# guest pulls rather than being pushed to.  That is what keeps this free of
# the WebDAV drawer's AmigaDOS name, which nothing over HTTP reveals.

BASEURL="http://$HOSTADDR:$HTTPPORT"

{
    echo "MakeDir $DRAWER"
    echo "Delete $DRAWER/tools.txt QUIET"
    for f in "${SEND_NAMES[@]}"; do
        # `fetch`, not `SYS:fetch`.  The emulator arms stage every tool at
        # the root of the drive they build, so SYS: finds them there; an
        # installed machine puts them in C: and SYS:fetch is then "Unknown
        # command" on a machine that has fetch.
        echo "fetch $BASEURL/$f TO $DRAWER/$f QUIET"
    done
    echo "List $DRAWER"
} > "$OUT/stage-commands.txt"

echo "==> staging $SENT file(s) into $DRAWER on $ADDR"
STAGE_RC=0
python3 "$ROOT/tests/tools/hwshell.py" "$ADDR" "$PORT" -t "$CMDTIME" \
    < "$OUT/stage-commands.txt" > "$OUT/stage.txt" 2>"$OUT/stage.err" ||
    STAGE_RC=$?
sed 's/^/    /' "$OUT/stage.txt"
[ ! -s "$OUT/stage.err" ] || sed 's/^/    /' "$OUT/stage.err" >&2
echo "stage_rc=$STAGE_RC"

if [ "$STAGE_RC" = 3 ]; then
    echo "RESULT=skip"
    exit 3
fi
[ "$STAGE_RC" = 0 ] || { echo "RESULT=fail"; exit 1; }

# Every file has to be there.  `fetch` reports its own failures, and a run
# that starts with three of four staged files fails somewhere far from here.
MISSING=""
for f in "${SEND_NAMES[@]}"; do
    grep -qi "^$f " "$OUT/stage.txt" || MISSING="$MISSING $f"
done
if [ -n "$MISSING" ]; then
    echo "staged_missing=$MISSING"
    echo "RESULT=fail"
    echo "hwrun: $DRAWER on $ADDR does not hold:$MISSING" >&2
    echo "  The List above is what it does hold." >&2
    exit 1
fi
echo "staged_missing=none"

RUNLINE="$DRAWER/ToolsSmoke $DRAWER"
if [ "$DETACH" = yes ]; then
    echo "run_mode=detached"
    echo "==> starting ToolsSmoke detached; nothing here holds the Shell" \
         "while it runs"
    python3 "$ROOT/tests/tools/hwshell.py" "$ADDR" "$PORT" -t 30 \
        -c "Run >NIL: <NIL: $RUNLINE" \
        > "$OUT/launch.txt" 2>"$OUT/launch.err" || true
    sed 's/^/    /' "$OUT/launch.txt"
else
    echo "run_mode=attached"
    echo "==> running ToolsSmoke, up to $CMDTIME s"
    RUN_RC=0
    python3 "$ROOT/tests/tools/hwshell.py" "$ADDR" "$PORT" -t "$CMDTIME" \
        -c "$RUNLINE" > "$OUT/run.txt" 2>"$OUT/run.err" || RUN_RC=$?
    sed 's/^/    /' "$OUT/run.txt"
    [ ! -s "$OUT/run.err" ] || sed 's/^/    /' "$OUT/run.err" >&2
    echo "run_rc=$RUN_RC"
fi

# ------------------------------------------------- waiting for it to come back --

WAITED=0
RET_ICMP=no
RET_HTTP=no
if [ "$DETACH" = yes ]; then
    echo "==> waiting up to $RETURNWAIT s for $ADDR to serve HTTP again"
    while [ "$WAITED" -lt "$RETURNWAIT" ]; do
        sleep 5
        WAITED=$((WAITED + 5))
        RET_HTTP=$(hw_http)
        [ "$RET_HTTP" = yes ] && break
    done
    RET_ICMP=$(hw_icmp)
    echo "return_waited_s=$WAITED"
    echo "return_icmp=$RET_ICMP"
    echo "return_http=$RET_HTTP"

    if [ "$RET_HTTP" != yes ]; then
        echo "hw_returned=no"
        echo "RESULT=fail"
        if [ "$RET_ICMP" = yes ]; then
            echo "hwrun: $ADDR is alive and serves no HTTP $WAITED s after" >&2
            echo "  the command list ran.  The machine did not stop; the" >&2
            echo "  network or httpd did not come back." >&2
        else
            echo "hwrun: $ADDR answers NEITHER ICMP NOR HTTP $WAITED s" >&2
            echo "  after the command list ran.  It was answering both" >&2
            echo "  before it.  A machine that stops answering ICMP has" >&2
            echo "  stopped, which is what a guru looks like from here." >&2
        fi
        exit 1
    fi
    echo "hw_returned=yes"
else
    echo "hw_returned=yes"
fi

# ---------------------------------------------------------- the report --
#
# Read out through the Shell rather than fetched over WebDAV: the drawer
# httpd serves is not RAM:hwrun and its AmigaDOS name is not discoverable
# from here.  `Type` puts the file on the socket, which is all that is
# wanted.

REPORT="$OUT/tools.txt"

read_report() {
    python3 "$ROOT/tests/tools/hwshell.py" "$ADDR" "$PORT" -t "$CMDTIME" \
        -c "Type $DRAWER/tools.txt" > "$OUT/report-raw.txt" \
        2>"$OUT/report.err" || true
    # hwshell prints its own key=value lines and a `----- <command>` header;
    # the report is what follows the header.
    awk 'f { print } /^----- Type / { f = 1 }' "$OUT/report-raw.txt" |
        sed '/^shell_commands=/d' > "$REPORT"
}

# HTTP COMING BACK IS NOT THE RUN FINISHING.  In detached mode the network is
# up again long before the command list is done, and the first read returned
# 571 bytes of a report whose next line was still being written -- which every
# assertion downstream would have taken for the whole answer.  ToolsSmoke ends
# its report with a line of its own; that line is the end of the run.
DONE_RE='^===== done, [0-9]+ command\(s\) would not run ====='
COMPLETE=no
WAITED_R=0
while : ; do
    read_report
    if grep -Eq "$DONE_RE" "$REPORT"; then
        COMPLETE=yes
        break
    fi
    [ "$WAITED_R" -lt "$CMDTIME" ] || break
    sleep 5
    WAITED_R=$((WAITED_R + 5))
done
echo "report_waited_s=$WAITED_R"
echo "report_complete=$COMPLETE"

if [ "$COMPLETE" != yes ]; then
    echo "report_bytes=$(wc -c < "$REPORT" | tr -d ' ')"
    echo "RESULT=fail"
    echo "hwrun: $DRAWER/tools.txt on $ADDR never got its closing line in" >&2
    echo "  $CMDTIME s.  What is there is a run still going or a run that" >&2
    echo "  stopped in the middle, and neither is an answer." >&2
    exit 1
fi

if [ ! -s "$REPORT" ]; then
    echo "report_bytes=0"
    echo "RESULT=fail"
    echo "hwrun: $DRAWER/tools.txt on $ADDR is empty or absent.  What the" >&2
    echo "  Shell said:" >&2
    sed 's/^/    /' "$OUT/report-raw.txt" >&2
    exit 1
fi

echo "report_bytes=$(wc -c < "$REPORT" | tr -d ' ')"
echo "report_path=$REPORT"
echo "RESULT=pass"
exit 0
