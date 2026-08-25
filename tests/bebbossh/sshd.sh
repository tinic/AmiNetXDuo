#!/usr/bin/env bash
#
# An SSH server on the build host for BebboSSH to connect to.
#
#   tests/bebbossh/sshd.sh {start|stop|status|knownhosts}
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
DIR="$ROOT/build/bebbossh-test"
PORT="${AMINETXDUO_BEBBOSSH_PORT:-2223}"
SSHD="${AMINETXDUO_SSHD:-}"

if [ -z "$SSHD" ]; then
    for cand in /usr/sbin/sshd /usr/bin/sshd /usr/local/sbin/sshd; do
        [ -x "$cand" ] && { SSHD="$cand"; break; }
    done
fi

SFTP="${AMINETXDUO_SFTP_SERVER:-}"
if [ -z "$SFTP" ]; then
    for cand in /usr/libexec/sftp-server \
                /usr/lib/openssh/sftp-server \
                /usr/libexec/openssh/sftp-server \
                /usr/lib/ssh/sftp-server; do
        [ -x "$cand" ] && { SFTP="$cand"; break; }
    done
fi

action="${1:-status}"

host_fingerprint() {
    ssh-keygen -lf "$DIR/hostkey_ed25519.pub" \
        | awk '{print $2}' | sed 's/^SHA256://; s/$/=/'
}

case "$action" in
start)
    [ -n "$SSHD" ] && [ -x "$SSHD" ] || {
        echo "no sshd found; set AMINETXDUO_SSHD=<path>" >&2; exit 2; }
    [ -n "$SFTP" ] || {
        echo "no sftp-server found; set AMINETXDUO_SFTP_SERVER=<path>" >&2
        echo "bebboscp is an SFTP client and cannot transfer without one." >&2
        exit 2; }

    mkdir -p "$DIR/xfer"

    if [ ! -f "$DIR/hostkey_ed25519" ]; then
        echo "==> host key (ed25519, the only kind BebboSSH verifies)"
        ssh-keygen -t ed25519 -f "$DIR/hostkey_ed25519" -N "" -q -C bebbossh-test
    fi

    if [ ! -f "$DIR/id_ed25519" ]; then
        echo "==> client key (OpenSSH format, made on the HOST, see above)"
        ssh-keygen -t ed25519 -f "$DIR/id_ed25519" -N "" -q -C amiga
        cp "$DIR/id_ed25519.pub" "$DIR/authorized_keys"
        chmod 600 "$DIR/authorized_keys" "$DIR/id_ed25519"
    fi

    cat > "$DIR/sshd_config" <<EOF
Port $PORT
ListenAddress 0.0.0.0
HostKey $DIR/hostkey_ed25519
PidFile $DIR/sshd.pid
AuthorizedKeysFile $DIR/authorized_keys
PasswordAuthentication no
PubkeyAuthentication yes
KbdInteractiveAuthentication no
UsePAM no
StrictModes no
LogLevel VERBOSE
Subsystem sftp $SFTP
#
# LoginGraceTime is why this file exists rather than "just use the system
# sshd".  OpenSSH allows 120 seconds from TCP connect to completed
# authentication; BebboSSH's own ReadMe budgets about a minute for the
# connection on an unaccelerated machine and measures 0.9 s per X25519 key
# pair on an A3000.  A stock server WILL hang up on a 14 MHz 68020, which is
# a property of the machine, not a bug in either end, and the same shape as the
# Cloudflare TLS timeouts in docs/RESEARCH.md 11.8.  Raised here so a run
# measures the Amiga rather than the server's patience.
LoginGraceTime 900
EOF

    if [ -f "$DIR/sshd.pid" ] && kill -0 "$(cat "$DIR/sshd.pid")" 2>/dev/null; then
        echo "==> already running, pid $(cat "$DIR/sshd.pid")"
    else
        "$SSHD" -f "$DIR/sshd_config" -E "$DIR/sshd.log" -o LogLevel=DEBUG2
        sleep 1
    fi

    echo "==> sshd on port $PORT, pid $(cat "$DIR/sshd.pid" 2>/dev/null || echo '?')"
    echo "    user       : $(id -un)"
    echo "    client key : $DIR/id_ed25519"
    echo "    sftp       : $SFTP"
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
        exit 1
    fi
    ;;

knownhosts)
    [ -f "$DIR/hostkey_ed25519.pub" ] || { echo "no host key yet; run start" >&2; exit 2; }
    host="${2:-10.0.2.2}"
    if [ "$PORT" = "22" ]; then
        printf '%s ssh-ed25519 %s\n' "$host" "$(host_fingerprint)"
    else
        printf '%s:%s ssh-ed25519 %s\n' "$host" "$PORT" "$(host_fingerprint)"
    fi
    ;;

*)
    echo "usage: $0 {start|stop|status|knownhosts [host]}" >&2
    exit 2
    ;;
esac
