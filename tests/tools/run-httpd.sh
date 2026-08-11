#!/usr/bin/env bash
#
# Put `httpd` on the LAN under Amiberry, so that real WebDAV clients can be
# pointed at it.
#
#   tests/tools/run-httpd.sh [-t SECONDS] [-a ADDRESS] [-p PORT] [-b BUILDDIR]
#                            [-B BACKEND] [-m MODEL]
#
# WHY THIS IS NOT A PASS/FAIL TEST
#
#   Everything worth knowing about a WebDAV server is what a real client does
#   with it, and the clients are Finder, Explorer's redirector and gvfs on
#   three other machines.  None of them can be driven from inside this script,
#   so what it does is put the server on the wire at a known address and hold
#   it there for a window, print the address, and afterwards print the guest's
#   own TRACE log, every method and every header, in the order it arrived.
#   The transcript is the result; a green tick would be a weaker claim.
#
#   The one thing it does assert is that the server answered at all, with a
#   curl from this host, so a window that produced nothing can be told from a
#   window nobody connected to.
#
# BRIDGED, AND NOT SLIRP
#
#   -B ens18 puts the guest on the host's own LAN with its own MAC, which is
#   the only configuration another machine can reach.  SLIRP's guest is behind
#   NAT and cannot be connected TO, so it can prove nothing here.
#   tools/amiberry-run.sh refuses the run if it did not get the backend it
#   asked for, which matters because the emulator falls back to SLIRP silently
#   for any name it cannot match.
#
# A STATIC ADDRESS, AND NOT DHCP
#
#   The address has to be known before the guest boots, because it goes into
#   the instructions a person follows on three other machines.  Pick one the
#   DHCP server will not hand out; there is no reservation for the emulated
#   MAC.
#
# WHAT THE THREE CLIENTS ACTUALLY DID, 2026-08-02
#
#   All three mounted, listed, read files including one called
#   "My File [!].txt", and walked into a subdirectory.  None of them needed
#   anything this server does not have.  From the guest's own TRACE log:
#
#     macOS      WebDAVFS/3.0.0 (Darwin 25.5.0).  OPTIONS, then PROPFIND
#                Depth 0 on /, then Depth 1.  Mounts READ-ONLY off the Allow
#                header alone, it never attempted a write, so a `cp` to the
#                volume fails locally without a request leaving the machine.
#                Probes six Finder metadata names that do not exist
#                (/._., /.hidden, /.metadata_never_index, /.Spotlight-V100 and
#                two more); eleven of the run's 404s are these.
#     Windows    Microsoft-WebDAV-MiniRedir/10.0.22621.  Needs the WebClient
#                service, which is demand-start and comes up on the first
#                `net use`.  27 PROPFINDs for the same work the others do in
#                five, all Depth 0, it walks a path one segment at a time.
#                Sends `translate: f` on everything.  Maps as a drive letter
#                and reachable as \\<address>@<port>\DavWWWRoot.
#     Linux      gvfs/1.57.2.  The fewest requests of the three, and the only
#                one that reports the server's own words back to the user:
#                "HTTP Error: Method Not Allowed" for a write.
#
#   NOT ONE OF THEM SENT LOCK.  Nor PROPPATCH, nor Depth: infinity, 25
#   Depth 0 and 7 Depth 1 across the whole run.  `DAV: 1` with no locking is
#   enough to mount read-only on all three.
#
#   Windows and gvfs do attempt a write when asked (PUT, and MKCOL from
#   `mkdir`), take the 405, and report it.  macOS does not get that far.
#
# THE HOST RUNNING THE EMULATOR CANNOT BE A CLIENT.  A frame the host sends to
# the guest's MAC never comes back to that NIC's pcap, so playhouse3 gets "no
# route to host" from its own guest while every other machine on the LAN
# reaches it.  Run the Linux client from a different machine.
#
# The a2065.device driver is not ours to ship: AMINETXDUO_A2065=<path>, or
# drop a copy in build/a2065.device.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

WINDOW=300
ADDRESS=192.168.1.240
NETMASK=255.255.255.0
GATEWAY=192.168.1.1
PORT=8080
MODEL=A1200
BACKEND=ens18
BUILD="${AMINETXDUO_BUILD:-build/cm}"

# A board other than the a2065, for driving a card this harness was not
# written around.  Same three knobs run-fitzbench.sh already documents:
# -N names the emulated board, AMINETXDUO_EXTRA_DRIVER stages its driver and
# AMINETXDUO_IFDEVICE is what DEVS:NetInterfaces/eth0 opens.  Default is the
# a2065 and nothing changes for a caller that does not ask.
BOARD="${AMINETXDUO_HTTPD_BOARD:-a2065}"
IFDEVICE="${AMINETXDUO_IFDEVICE:-a2065.device}"

while getopts "t:a:p:b:B:m:g:N:" opt; do
    case "$opt" in
        N) BOARD="$OPTARG" ;;
        t) WINDOW="$OPTARG" ;;
        a) ADDRESS="$OPTARG" ;;
        p) PORT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        B) BACKEND="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        g) GATEWAY="$OPTARG" ;;
        *) echo "usage: $0 [-t seconds] [-a address] [-p port] [-b builddir] [-B backend] [-m model]" >&2; exit 2 ;;
    esac
done

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"

for f in "$TOOLS/httpd" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
done

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for candidate in "$ROOT/build/a2065.device" "$HOME/amiga-assets/devs/a2065.device"; do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "No a2065.device found.  Set AMINETXDUO_A2065=<path>." >&2
    exit 2
}

# ------------------------------------------------------------- staging ---

STAGE="$ROOT/build/httpd-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs" "$STAGE/Public/Docs" "$STAGE/Public/Empty Drawer"

cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/Networks/a2065.device" 2>/dev/null || {
    mkdir -p "$STAGE/devs/Networks"
    cp "$A2065" "$STAGE/devs/Networks/a2065.device"
}
cp "$BSD" "$STAGE/libs/bsdsocket.library"

# The extra driver goes beside the a2065's, which stays: harmless, and some
# paths still reference it.
# Both places.  run-fitzbench.sh stages a driver at DEVS: root and this
# harness puts the a2065 under DEVS:Networks; which one a given driver is
# found in is not worth discovering from a stack that will only say the
# network would not start.
[ -z "${AMINETXDUO_EXTRA_DRIVER:-}" ] || {
    cp "$AMINETXDUO_EXTRA_DRIVER" "$STAGE/devs/Networks/$(basename "$AMINETXDUO_EXTRA_DRIVER")"
    cp "$AMINETXDUO_EXTRA_DRIVER" "$STAGE/devs/$(basename "$AMINETXDUO_EXTRA_DRIVER")"
}

cat > "$STAGE/devs/NetInterfaces/eth0" <<EOF
DEVICE=$IFDEVICE
UNIT=0
CONFIGURE=STATIC
ADDRESS=$ADDRESS
NETMASK=$NETMASK
GATEWAY=$GATEWAY
EOF

# The content a client will see.  Deliberately awkward: a space and a bracket
# in a name is what percent-encoding is for, and a WebDAV client that gets the
# escaping wrong shows the drawer as empty rather than failing, which is the
# kind of thing only a real client finds.
echo "Hello from an Amiga." > "$STAGE/Public/readme.txt"
echo "<html><body><h1>Amiga</h1></body></html>" > "$STAGE/Public/index.html"
echo "one two three" > "$STAGE/Public/My File [!].txt"
echo "in a drawer" > "$STAGE/Public/Docs/notes.txt"
: > "$STAGE/Public/empty.dat"
# Big enough that a client reads it in more than one go, and that a Range
# request is worth making.
dd if=/dev/urandom of="$STAGE/Public/blob.bin" bs=1024 count=512 status=none

echo "==> serving DH0:Public on http://$ADDRESS:$PORT/ for ${WINDOW}s"

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-httpd}"
HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"

# ------------------------------------------------------------------ run ---
#
# httpd is the only executable: opening bsdsocket.library is what starts the
# network, so the interface comes up underneath the server rather than needing
# an AddNetInterface before it.  The server never exits, so the run ends on
# the harness timeout and 124 is the expected status.
set +e
"$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$BACKEND" -m "$MODEL" -t "$WINDOW" \
    -a "DH0:Public $PORT TRACE" \
    "$TOOLS/httpd" "$STAGE/devs" "$STAGE/libs" "$STAGE/Public" &
RUNNER=$!
set -e

# ---------------------------------------------------------- the assertion --
#
# One request from this host, once the guest is up, so that a window in which
# nothing happened can be told from one in which the server was never there.
# The guest takes a while to boot and bring the interface up; poll rather than
# sleeping a guessed amount.
ANSWERED=no
for _ in $(seq 1 "$((WINDOW / 2))"); do
    sleep 2
    kill -0 "$RUNNER" 2>/dev/null || break
    if curl -s -m 4 -o /dev/null -w '%{http_code}' \
            "http://$ADDRESS:$PORT/" 2>/dev/null | grep -q '^200$'; then
        ANSWERED=yes
        break
    fi
done

if [ "$ANSWERED" = yes ]; then
    echo "==> the guest answered a GET from this host"
    echo "==> point a client at http://$ADDRESS:$PORT/ now"
else
    echo "!! the guest never answered from this host" >&2
fi

wait "$RUNNER" 2>/dev/null || true

echo
echo "===================== what the clients sent ====================="
if [ -f "$HD/stdout.txt" ]; then
    cat "$HD/stdout.txt"
else
    echo "(the guest wrote no stdout.txt)"
fi
echo "================================================================="

[ "$ANSWERED" = yes ] || exit 1
