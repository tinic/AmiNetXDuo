#!/usr/bin/env bash
#
# ONE PLACE THAT ARBITRATES THE RIG, so two runs on one host cannot take the
# same thing.
#
#   . tools/emu-rig-lock.sh
#   rig_claim_port  "$TAG"            # -> $RIG_PORT, reserved until this exits
#   rig_claim_name  bridged-pcmcia    # -> exclusive, or a refusal that names
#                                     #    who holds it
#   rig_claim_address 192.168.1 200 239   # -> $RIG_ADDRESS, free on the LAN
#
# THE KNOB IS THE DIRECTORY: $AMINETXDUO_RIG_LOCKDIR, or
# ${TMPDIR:-/tmp}/aminetxduo-rig-<uid>.  Host-wide and NOT under build/ on
# purpose -- the runs that corrupt each other are in different clones.
#
# A claim is flock(2) on <lockdir>/port-<n>.lock AND a bind(2) probe on
# 127.0.0.1:<n>; neither alone is enough, and SO_REUSEADDR is NOT set, so a
# port in TIME_WAIT is refused deliberately.
#
# SPDX-License-Identifier: MIT

# Bash 4.1 for `exec {fd}>`, 4.2 for `declare -g`.  Both are 2011 or older.
# -g because a script that sources this from inside a function would otherwise
# get a local array, and every claim it made would be released at the end of
# that function rather than at the end of the run.
declare -gA RIG_HELD_FDS 2> /dev/null || true

# The directory, created if it is not there.  0700: the lock files carry the
# holder's pid and command, which is nobody else's business.
rig_lockdir() {
    local d="${AMINETXDUO_RIG_LOCKDIR:-${TMPDIR:-/tmp}/aminetxduo-rig-$(id -u)}"
    mkdir -p "$d" 2>/dev/null || true
    chmod 700 "$d" 2>/dev/null || true
    printf '%s\n' "$d"
}

# Is a TCP port genuinely free on the loopback?  0 free, 1 taken.
#
# python3 does a real bind, which is the exact question.  Without python3 the
# fallback asks ss(8) whether anything is LISTENing there, which is weaker --
# it does not see a TIME_WAIT and it does not see a bind on a different
# address -- and a host with neither gets an honest "cannot tell", which the
# caller treats as free because the flock is then the only protection there is.
rig_port_free() { # port
    if command -v python3 > /dev/null 2>&1; then
        python3 - "$1" <<'PY' 2> /dev/null
import socket, sys
s = socket.socket()
try:
    s.bind(("127.0.0.1", int(sys.argv[1])))
except OSError:
    sys.exit(1)
finally:
    s.close()
PY
        return $?
    fi
    if command -v ss > /dev/null 2>&1; then
        [ -z "$(ss -ltnH "sport = :$1" 2>/dev/null)" ]
        return $?
    fi
    return 0
}

# Claim a free loopback port and HOLD it until this shell exits.
#
#   rig_claim_port <who> [base] [span]
#
# Sets RIG_PORT, RIG_PORT_FILE and RIG_PORT_FD.  Returns 1 and says why when
# the range is full, which on a rig means something is not cleaning up.
#
# The scan starts at a per-process offset rather than at the bottom of the
# range, so two harnesses that start together do not both walk from 12000 and
# fight over the same first candidates.  It is a scheduling hint and nothing
# depends on it: correctness is the lock.
rig_claim_port() { # who [base] [span]
    local who="${1:-anon}" base="${2:-12000}" span="${3:-900}"
    local dir p i off fd tried=0
    dir=$(rig_lockdir)

    off=$(( ($$ * 7919 + $(date +%s)) % span ))
    for (( i = 0; i < span; i++ )); do
        p=$(( base + (off + i) % span ))
        # Append, never truncate: a candidate we do not win must not clobber
        # the record of the harness that owns it.
        #
        # AND `2> /dev/null` MUST NOT GO ON THE exec.  Redirections on `exec`
        # apply to the shell PERMANENTLY, so `exec {fd}>>f 2>/dev/null` sends
        # the caller's stderr to the bit bucket for the rest of the run --
        # every refusal this file prints then vanishes, which is precisely the
        # silence it exists to remove.  The `:` below is where the quiet
        # creatability test belongs: a redirection on a builtin that is not
        # `exec` is undone when the builtin returns.
        : >> "$dir/port-$p.lock" 2> /dev/null || continue
        exec {fd}>>"$dir/port-$p.lock" || continue
        if flock -n "$fd" 2> /dev/null; then
            tried=$((tried + 1))
            if rig_port_free "$p"; then
                # We hold the lock, so this truncating write is ours to make.
                printf 'port=%s pid=%s who=%s since=%s\n' \
                       "$p" "$$" "$who" "$(date +%FT%T)" > "$dir/port-$p.lock"
                RIG_PORT="$p"
                RIG_PORT_FD="$fd"
                RIG_PORT_FILE="$dir/port-$p.lock"
                return 0
            fi
        fi
        exec {fd}>&-
    done

    echo "no free port in $base..$((base + span - 1)) on this host." >&2
    echo "  $tried of $span were free to lock and none of them was free to" >&2
    echo "  bind, so something is holding them without holding the lock." >&2
    echo "  ss -ltn 'sport >= :$base and sport < :$((base + span))'" >&2
    echo "  lock directory: $dir" >&2
    return 1
}

rig_release_port() {
    [ -n "${RIG_PORT_FD:-}" ] || return 0
    eval "exec ${RIG_PORT_FD}>&-" 2> /dev/null || true
    RIG_PORT_FD=""
}

# Claim a NAMED exclusive resource, non-blocking.
#
#   rig_claim_name <name> [who]
#
# Returns 0 holding it, 1 having printed who has it.  The lock file's contents
# are the previous holder's record, which is the difference between "somebody
# else is running" and a sentence a caller can act on.
rig_claim_name() { # name [who]
    local name="$1" who="${2:-$$}" dir fd
    dir=$(rig_lockdir)

    : >> "$dir/$name.lock" 2> /dev/null || {
        echo "cannot create $dir/$name.lock" >&2; return 1; }
    exec {fd}>>"$dir/$name.lock" || return 1
    if ! flock -n "$fd" 2> /dev/null; then
        echo "another run holds '$name' on this host:" >&2
        sed 's/^/    /' "$dir/$name.lock" >&2 2> /dev/null || true
        exec {fd}>&-
        return 1
    fi
    printf 'name=%s pid=%s who=%s since=%s\n' \
           "$name" "$$" "$who" "$(date +%FT%T)" > "$dir/$name.lock"
    RIG_HELD_FDS["$name"]="$fd"
    return 0
}

rig_release_name() { # name
    local fd="${RIG_HELD_FDS[$1]:-}"
    [ -n "$fd" ] || return 0
    eval "exec ${fd}>&-" 2> /dev/null || true
    unset "RIG_HELD_FDS[$1]"
}

# Exit 1 from ping means "no reply"; anything else means the prober itself
# could not run, and an unusable prober must never read as an empty LAN.
rig_prober_usable() {
    ping -c 1 -W 1 127.0.0.1 > /dev/null 2>&1 && return 0
    echo "ping cannot probe: no address can be claimed." >&2
    echo "  Unprivileged ICMP is off and /usr/bin/ping has no cap_net_raw." >&2
    echo "  As root: sysctl -w net.ipv4.ping_group_range='0 2147483647'" >&2
    echo "        or setcap cap_net_raw+ep /usr/bin/ping" >&2
    return 1
}

# Claim a LAN address nothing else is using.
#
#   rig_claim_address <prefix> <first> <last> [who]
#   rig_claim_address 192.168.1 200 239 poolshare   # -> RIG_ADDRESS
#
# Two checks again, and the second one is the reason this exists rather than a
# hash of the run tag: 192.168.1.243 was picked out of the air by a harness
# and turned out to be a LIVE HOST on the lab LAN.  A derived address is only
# free of other RUNS; ping asks whether it is free of everything else.  A
# machine that answers is skipped and never claimed, so the range may safely
# overlap real hosts.
#
# An address that is claimed stays claimed for the life of the shell, so the
# ARP caches on the LAN cannot alias two runs onto one address -- which is the
# failure this is really about, and it survives the run that caused it by as
# long as the switch's table does.
rig_claim_address() { # prefix first last [who]
    local prefix="$1" first="$2" last="$3" who="${4:-$$}"
    local dir n addr fd off span i
    dir=$(rig_lockdir)
    span=$((last - first + 1))
    [ "$span" -gt 0 ] || { echo "empty address range" >&2; return 1; }

    rig_prober_usable || return 1

    off=$(( ($$ * 7919 + $(date +%s)) % span ))
    for (( i = 0; i < span; i++ )); do
        n=$(( first + (off + i) % span ))
        addr="$prefix.$n"
        : >> "$dir/addr-$addr.lock" 2> /dev/null || continue
        exec {fd}>>"$dir/addr-$addr.lock" || continue
        if flock -n "$fd" 2> /dev/null; then
            ping -c 1 -W 1 "$addr" > /dev/null 2>&1
            if [ $? -eq 1 ]; then
                printf 'address=%s pid=%s who=%s since=%s\n' \
                       "$addr" "$$" "$who" "$(date +%FT%T)" \
                       > "$dir/addr-$addr.lock"
                RIG_ADDRESS="$addr"
                RIG_HELD_FDS["addr-$addr"]="$fd"
                return 0
            fi
        fi
        exec {fd}>&-
    done

    echo "no free address in $prefix.$first..$prefix.$last." >&2
    echo "  Every one is either claimed by another run under $dir or" >&2
    echo "  answered a ping, which means a real machine has it." >&2
    return 1
}

# WHO IS LISTENING ON A LOOPBACK PORT.  Three functions, because the useful
# question is not "which pid" but "is it MINE", and that one has an exact
# answer that costs nothing.
#
# NOT ss -p.  It was, and on playhouse3 it silently declines to name the owner:
# a live `amiberry --log` listening on 127.0.0.1:12709 shows as
#
#     LISTEN 0 1 127.0.0.1:12709 0.0.0.0:*
#
# with the Process column EMPTY, under the same user that started it, while
# sshd's socket on the same output carries `users:(("sshd",pid=...))`.  A check
# that quietly returns "cannot tell" on the rig it was written for is not a
# check.  /proc has the mapping unconditionally: the listening socket's inode
# is in /proc/net/tcp, and a process holds it if one of its descriptors is a
# symlink to socket:[<inode>].

# The inode of the LISTEN socket on 127.0.0.1:<port>, or nothing.
# /proc/net/tcp: field 2 is local_address as HEX:HEX, field 4 is the state
# (0A is TCP_LISTEN), field 10 is the inode.
rig_listen_inode() { # port
    local hexport
    hexport=$(printf '%04X' "$1")
    awk -v want=":$hexport" '$4 == "0A" && $2 ~ want"$" { print $10; exit }' \
        /proc/net/tcp 2> /dev/null
}

# Does <pid> hold the socket with <inode> open?  0 yes, 1 no.
rig_pid_holds_socket() { # pid inode
    local fd
    for fd in "/proc/$1/fd"/*; do
        [ -L "$fd" ] || continue
        case "$(readlink "$fd" 2> /dev/null)" in
            "socket:[$2]") return 0 ;;
        esac
    done
    return 1
}

# Which pid holds <inode>?  Only ever called on the failure path -- naming the
# other run is worth a scan of /proc that the happy path must not pay for.
rig_owner_pid() { # inode
    local d pid
    for d in /proc/[0-9]*; do
        pid="${d#/proc/}"
        if rig_pid_holds_socket "$pid" "$1" 2> /dev/null; then
            printf '%s\n' "$pid"
            return 0
        fi
    done
    return 1
}

# IS AN OLD READER AIMED AT THIS PORT?  Prints the offenders, one per line, or
# nothing.
#
# A serial reader is a CLIENT: it holds nothing until an emulator binds, so the
# bind probe above cannot see it.
#
# ANCHORED AT THE START OF THE COMMAND LINE, and that is not decoration.
# `pgrep -f` matches anywhere in the whole argv, so an UNANCHORED pattern hits
# any shell whose command line happens to contain the text -- including the very
# command a person types to test this.
rig_port_readers() { # port
    pgrep -af "^[^ ]*python3[^ ]* .*serial-timestamp\.py 127\.0\.0\.1 $1 " \
        2> /dev/null || true
    pgrep -af "^[^ ]*nc 127\.0\.0\.1 $1\$" 2> /dev/null || true
}

# A one-line description of a pid, for a refusal that names the other run
# rather than only its number.
rig_pid_describe() { # pid
    local pid="$1" cmd
    cmd=$(tr '\0' ' ' < "/proc/$pid/cmdline" 2> /dev/null)
    [ -n "$cmd" ] || cmd=$(ps -o args= -p "$pid" 2> /dev/null)
    printf '%s %s\n' "$pid" "${cmd:-<gone>}"
}
