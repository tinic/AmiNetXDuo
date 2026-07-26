#!/usr/bin/env bash
#
# An SSH server for the emulated Amiga to connect to, on the build host.
#
#   clients/dropbear/sshd-testserver.sh {start|stop|status}
#
# FS-UAE's SLIRP puts the HOST at 10.0.2.2, so the shortest path to "does
# dbclient complete a key exchange" is an sshd on this machine.  This starts
# one that touches nothing outside build/sshd-test/:
#
#   * port 2222, so the system's own sshd (if any) is untouched
#   * its own host keys, made here -- ed25519, ECDSA P-256 and RSA, so that a
#     client built with any one algorithm family has something to verify
#   * its own authorized_keys, holding one key made here
#   * PasswordAuthentication no -- a non-root sshd cannot do it anyway, and
#     public-key is what a machine with no keyboard should be using
#   * StrictModes no, because the key lives under build/ and not in ~/.ssh
#
# NO ROOT.  OpenSSH's sshd refuses to change user without it, which is exactly
# why the login has to be the user running this script -- and it is, because
# the authorized key is theirs.  Anything else would need sudo and this does
# not.
#
# THE CLIENT KEY IS MADE HERE, NOT ON THE AMIGA, AND THAT IS THE POINT.
#
#   build/sshd-test/id_amiga is a Dropbear-format ed25519 private key, made by
#   a NATIVELY built dropbearkey from the same pinned source.  The Amiga could
#   run dropbearkey -- it builds -- and it should not: src/common/ami_random.c
#   credits itself about 21 bits and reports itself unseeded, so a long-term
#   key generated there would be a key somebody could enumerate.  A per-session
#   ephemeral key from that pool risks one session; a private key on disk risks
#   every session anyone ever makes with it.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
DIR="$ROOT/build/sshd-test"
PORT="${AMINETXDUO_SSH_PORT:-2222}"
SSHD="${AMINETXDUO_SSHD:-/usr/sbin/sshd}"

action="${1:-status}"

# dropbearkey, built natively from the pinned submodule.  Built here rather
# than depended upon, because the point is that the key is in Dropbear's own
# format and made by Dropbear's own code.
db_host_tools()
{
    local out="$ROOT/build/dropbear-host"

    [ -x "$out/dropbearkey" ] && return 0

    echo "==> building a native dropbearkey (for the test key only)"
    mkdir -p "$out"
    (
        cd "$out"
        # macOS has no setresgid().  Turning DROPBEAR_SVR_MULTIUSER off instead
        # would be wrong even for a key generator: it arms a runtime check in
        # common-session.c that a real multiuser host fails.
        printf '#define DROPBEAR_SVR_DROP_PRIVS 0\n#define DROPBEAR_SVR_LOCALSTREAMFWD 0\n#define DROPBEAR_SVR_REMOTESTREAMFWD 0\n' > localoptions.h
        "$ROOT/third_party/dropbear/configure" --disable-zlib --disable-harden \
            >configure.log 2>&1
        make PROGRAMS=dropbearkey -j4 >make.log 2>&1
    ) || { echo "!! native dropbearkey build failed; see $out/make.log" >&2; return 1; }
    return 0
}

case "$action" in
start)
    mkdir -p "$DIR"

    if [ ! -f "$DIR/hostkey_ed25519" ]; then
        echo "==> host keys"
        ssh-keygen -t ed25519 -f "$DIR/hostkey_ed25519" -N "" -q
        ssh-keygen -t rsa -b 2048 -f "$DIR/hostkey_rsa" -N "" -q
    fi
    # The ECDSA host key exists for one reason: clients/dropbear/
    # localoptions-p256.h builds a client with no 25519 of any kind, and it
    # needs a host key it can verify with P-256.  A stock OpenSSH install has
    # one (ssh-keygen -A makes all three), so this is not a special server.
    if [ ! -f "$DIR/hostkey_ecdsa" ]; then
        ssh-keygen -t ecdsa -b 256 -f "$DIR/hostkey_ecdsa" -N "" -q
    fi

    db_host_tools || exit 1

    if [ ! -f "$DIR/id_amiga" ]; then
        echo "==> client key (Dropbear format, made on the HOST -- see above)"
        "$ROOT/build/dropbear-host/dropbearkey" -t ed25519 -f "$DIR/id_amiga" \
            | sed -n 's/^Fingerprint/  fingerprint/p'
        "$ROOT/build/dropbear-host/dropbearkey" -y -f "$DIR/id_amiga" \
            | grep '^ssh-ed25519' > "$DIR/authorized_keys"
        chmod 600 "$DIR/authorized_keys" "$DIR/id_amiga"
    fi

    # A second client key, ECDSA P-256, for the same reason: the P-256 build
    # cannot present an ed25519 one.  Both are authorised, so one server serves
    # both builds and the two runs differ only in the client binary.
    if [ ! -f "$DIR/id_amiga_ecdsa" ]; then
        echo "==> client key, ECDSA P-256 (for the no-25519 build)"
        "$ROOT/build/dropbear-host/dropbearkey" -t ecdsa -s 256 \
            -f "$DIR/id_amiga_ecdsa" | sed -n 's/^Fingerprint/  fingerprint/p'
        "$ROOT/build/dropbear-host/dropbearkey" -y -f "$DIR/id_amiga_ecdsa" \
            | grep '^ecdsa-sha2-nistp256' >> "$DIR/authorized_keys"
        chmod 600 "$DIR/authorized_keys" "$DIR/id_amiga_ecdsa"
    fi

    cat > "$DIR/sshd_config" <<EOF
Port $PORT
ListenAddress 0.0.0.0
HostKey $DIR/hostkey_ed25519
HostKey $DIR/hostkey_ecdsa
HostKey $DIR/hostkey_rsa
PidFile $DIR/sshd.pid
AuthorizedKeysFile $DIR/authorized_keys
PasswordAuthentication no
PubkeyAuthentication yes
KbdInteractiveAuthentication no
UsePAM no
StrictModes no
LogLevel VERBOSE
#
# LoginGraceTime is the reason this file exists rather than "just use your
# system sshd".  OpenSSH's default is 120 seconds from TCP connect to a
# completed authentication, and a 14 MHz 68020 doing curve25519 twice and an
# ed25519 verify does not always fit inside it.  A stock server WILL hang up
# on this client -- which is a finding, not a bug in the harness, and the same
# shape as the Cloudflare TLS timeouts in docs/RESEARCH.md §11.8.  Raised here
# so that a run measures the Amiga rather than the server's patience.
LoginGraceTime 600
EOF

    if [ -f "$DIR/sshd.pid" ] && kill -0 "$(cat "$DIR/sshd.pid")" 2>/dev/null; then
        echo "==> already running, pid $(cat "$DIR/sshd.pid")"
    else
        "$SSHD" -f "$DIR/sshd_config" -E "$DIR/sshd.log" -o LogLevel=DEBUG2
        sleep 1
    fi

    echo "==> sshd on port $PORT, pid $(cat "$DIR/sshd.pid" 2>/dev/null || echo '?')"
    echo "    user       : $(id -un)"
    echo "    client key : $DIR/id_amiga"
    echo "    log        : $DIR/sshd.log"
    ;;

stop)
    if [ -f "$DIR/sshd.pid" ]; then
        kill "$(cat "$DIR/sshd.pid")" 2>/dev/null || true
        rm -f "$DIR/sshd.pid"
        echo "==> stopped"
    else
        echo "==> not running"
    fi
    ;;

status)
    if [ -f "$DIR/sshd.pid" ] && kill -0 "$(cat "$DIR/sshd.pid")" 2>/dev/null; then
        echo "running, pid $(cat "$DIR/sshd.pid"), port $PORT"
    else
        echo "not running"
    fi
    ;;

*)
    echo "usage: $0 {start|stop|status}" >&2
    exit 2
    ;;
esac
