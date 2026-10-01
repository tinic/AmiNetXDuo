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

# flock(1) IS THE MUTEX.  Without it every claim below would look like a lock
# that was taken and a port that was busy, and rig_claim_port would blame a
# holder that does not exist -- macOS has no flock(1), and that is exactly what
# it reported.  Answer once, and answer with the cause.
RIG_FLOCK=${RIG_FLOCK:-}
rig_have_flock() {
    [ -n "$RIG_FLOCK" ] && { [ "$RIG_FLOCK" = yes ] && return 0 || return 1; }
    if command -v flock > /dev/null 2>&1; then RIG_FLOCK=yes; return 0; fi
    RIG_FLOCK=no
    return 1
}

rig_no_flock() {
    echo "flock(1) is not installed, so nothing on this host can arbitrate" >&2
    echo "  the rig.  A host that runs emulator arms needs util-linux's" >&2
    echo "  flock; a host that only builds does not, and does not claim." >&2
    return 1
}

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
    rig_have_flock || { rig_no_flock; return 1; }
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
    rig_have_flock || { rig_no_flock; return 1; }
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

# ONE BRIDGED GUEST PER HOST.
#
#   rig_claim_bridge <backend> <who>   # 0 held (or not bridged), 6 busy,
#                                      # 2 cannot arbitrate here
#   rig_drop_bridge                    # close it in this shell or subshell
#
# A guest on a host NIC shares the segment with every other one on the host,
# and two of them is not a run that can be read: on 2026-10-01 a manual
# run-ifsurvive and CI's ifsurvive arm were up at once on ens18, under one
# MAC, and the ICMP replies went to whichever guest the switch picked.  The
# `bridged-rig` lock that was here was SHARED for ordinary runs, so it let
# exactly that through.
#
# AN EXCLUSIVE flock(2) ON ONE FIXED PATH, NOT under rig_lockdir(): that one
# follows $TMPDIR, and a runner and a login shell that disagree about $TMPDIR
# must still meet here.  The descriptor is inherited by the emulator, so the
# claim lasts exactly as long as the guest does, even if the script that took
# it is killed with -9.  Long-lived children that are NOT the guest (serial
# readers, log cappers) call rig_drop_bridge first, or an orphaned reader
# would hold the rig for ever.
#
# BLOCKING, BOUNDED.  It waits AMINETXDUO_BRIDGE_WAIT seconds (default 1800)
# for the run ahead to finish, saying whose run that is, and then refuses.
#
# THE RETURN IS THE EXIT CODE, so a caller writes `|| exit $?`: 6 is another
# guest held the rig for the whole wait (rig_busy in tools/test-verdict.sh);
# 2 is a missing ingredient -- no flock(1), or a lock file that cannot be
# created or opened -- which is amiberry-run.sh's code for the same thing and
# must never read as somebody else's guest.
# The wait is a background flock(1) and `wait`, so a TERM to the harness is
# acted on at once rather than after the timeout.
#
# slirp, slirp_inbound, none and no backend at all are not on the segment and
# return 0 without touching anything: those stay parallel.  So do STANDING
# guests, which never call this (rig_standing_exempt below).
# Kept across a second `.` of this file, which must not forget a claim.
RIG_BRIDGE_FD="${RIG_BRIDGE_FD:-}"
RIG_BRIDGE_WAITER="${RIG_BRIDGE_WAITER:-}"

rig_bridge_path() {
    printf '%s\n' "${AMINETXDUO_BRIDGE_LOCK:-/tmp/aminetxduo-bridge.lock}"
}

rig_backend_bridged() { # backend
    case "${1:-}" in
        ""|slirp|slirp_inbound|none) return 1 ;;
    esac
    return 0
}

# Who has it open, by pid, for the refusal.  /proc only; an emulator with
# file capabilities is non-dumpable and does not show, which is why the
# record in the file names the run that took it.
rig_bridge_holders() {
    local f="$1" d l
    [ -d /proc/self/fd ] || return 0
    for d in /proc/[0-9]*/fd/*; do
        l=$(readlink "$d" 2> /dev/null) || continue
        [ "$l" = "$f" ] || continue
        d="${d#/proc/}"
        rig_pid_describe "${d%%/*}"
    done 2> /dev/null | sort -u
}

rig_claim_bridge() { # backend who
    rig_backend_bridged "${1:-}" || return 0
    [ -z "$RIG_BRIDGE_FD" ] || return 0
    rig_have_flock || { rig_no_flock; return 2; }
    local who="${2:-$$}" f fd rc start waited
    local limit="${AMINETXDUO_BRIDGE_WAIT:-1800}"
    f=$(rig_bridge_path)

    # Append, never truncate, until it is ours: the record is the holder's.
    ( umask 000; : >> "$f" ) 2> /dev/null || {
        echo "cannot create the bridge lock $f" >&2; return 2; }
    # No `2> /dev/null` on this exec: it would silence stderr for the rest of
    # the run (see rig_claim_port).  The `:` above is the quiet test.
    exec {fd}>>"$f" || {
        echo "cannot open the bridge lock $f" >&2; return 2; }

    if ! flock -n -x "$fd" 2> /dev/null; then
        echo "==> another bridged guest is up on this host; waiting up to" \
             "${limit}s for it to exit:" >&2
        sed 's/^/    /' "$f" >&2 2> /dev/null || true
        start=$(date +%s)
        flock -x -w "$limit" "$fd" 2> /dev/null &
        RIG_BRIDGE_WAITER=$!
        rc=0
        wait "$RIG_BRIDGE_WAITER" || rc=$?
        RIG_BRIDGE_WAITER=""
        waited=$(( $(date +%s) - start ))
        if [ "$rc" != 0 ]; then
            echo >&2
            echo "REFUSING to boot a second bridged guest on this host" \
                 "(waited ${waited}s)." >&2
            echo "  $(rig_bridge_path) is held by:" >&2
            sed 's/^/    /' "$f" >&2 2> /dev/null || true
            rig_bridge_holders "$f" | sed 's/^/    open in: /' >&2
            echo "  Two guests on one segment split each other's replies," >&2
            echo "  so a run beside another proves nothing.  Wait for it," >&2
            echo "  run this one with -B slirp, or raise" \
                 "AMINETXDUO_BRIDGE_WAIT." >&2
            exec {fd}>&-
            return 6
        fi
        echo "==> bridge free after ${waited}s"
    fi

    printf 'bridge pid=%s who=%s since=%s\n' \
           "$$" "$who" "$(date +%FT%T)" > "$f"
    RIG_BRIDGE_FD="$fd"
    echo "==> exclusive bridge lock held ($f)"
    return 0
}

# A STANDING GUEST does not claim the bridge: it is up for hours and would
# hold every test run off for the whole of it.  It is kept apart by its own
# MAC range instead (emu_mac_standing in tools/emu-mac.sh), and says so.
#
#   rig_standing_exempt <who> <mac> <backend>   # 0 exempt and held, 2 refused
#   rig_drop_standing                           # in a child that is not the guest
#
# THE RANGE IS CHECKED HERE, so no launcher can skip it: an override such as
# AMINETXDUO_CWB_MAC set to a test-range address would otherwise give a guest
# that skips the lock AND can share a test guest's MAC.
#
# AND THE ADDRESS IS CLAIMED: emu_mac_standing reduces an instance to one of
# 4096 addresses per kind, so two instances can land on one.  A non-blocking
# flock on aminetxduo-standing-<mac>.lock, beside the bridge lock and so
# host-wide -- a lock on a drive directory would miss the same address from
# another checkout.  The second is refused, naming the first.  Like the bridge
# lock the descriptor is inherited by the emulator and held for its life;
# helpers that are not the guest call rig_drop_standing first.
#
# Returns 2 (a rig fault, as everywhere in this file) and says why; callers
# `|| exit $?`, before they wipe or start anything.
RIG_STANDING_FD="${RIG_STANDING_FD:-}"

rig_standing_path() { # mac
    printf '%s/aminetxduo-standing-%s.lock\n' "$(dirname "$(rig_bridge_path)")" \
           "$(printf '%s' "$1" | tr -d ':' | tr '[:upper:]' '[:lower:]')"
}

rig_standing_exempt() { # who mac backend
    local f fd
    if ! printf '%s' "$2" | grep -qiE '^02:41:4d:47(:[0-9a-f]{2}){2}$'; then
        echo "REFUSING to start standing guest $1 on mac=$2." >&2
        echo "  A standing guest skips the bridge lock only on a standing-range" >&2
        echo "  MAC (02:41:4d:47:xx:xx, emu_mac_standing in tools/emu-mac.sh)," >&2
        echo "  which no test guest can have.  Drop the override, or pin one" >&2
        echo "  inside that range." >&2
        return 2
    fi
    [ -z "$RIG_STANDING_FD" ] || return 0
    rig_have_flock || { rig_no_flock; return 2; }
    f=$(rig_standing_path "$2")
    ( umask 000; : >> "$f" ) 2> /dev/null || {
        echo "cannot create the standing lock $f" >&2; return 2; }
    exec {fd}>>"$f" || { echo "cannot open the standing lock $f" >&2; return 2; }
    if ! flock -n -x "$fd" 2> /dev/null; then
        echo "REFUSING to start standing guest $1: mac=$2 is already up." >&2
        sed 's/^/    /' "$f" >&2 2> /dev/null || true
        echo "  Two standing instances hashed to one address; booting would" >&2
        echo "  put two guests on the wire under it.  Pin another standing-range" >&2
        echo "  MAC for one of them, or stop the other." >&2
        exec {fd}>&-
        return 2
    fi
    printf 'standing mac=%s pid=%s who=%s since=%s\n' \
           "$2" "$$" "$1" "$(date +%FT%T)" > "$f"
    RIG_STANDING_FD="$fd"
    printf 'bridge_lock=exempt standing=%s mac=%s backend=%s\n' "$1" "$2" "$3"
}

rig_drop_standing() {
    [ -n "$RIG_STANDING_FD" ] || return 0
    eval "exec ${RIG_STANDING_FD}>&-" 2> /dev/null || true
    RIG_STANDING_FD=""
}

# Close this shell's copy.  In a subshell that only drops the subshell's.
rig_drop_bridge() {
    if [ -n "$RIG_BRIDGE_WAITER" ]; then
        kill "$RIG_BRIDGE_WAITER" 2> /dev/null || true
        RIG_BRIDGE_WAITER=""
    fi
    [ -n "$RIG_BRIDGE_FD" ] || return 0
    eval "exec ${RIG_BRIDGE_FD}>&-" 2> /dev/null || true
    RIG_BRIDGE_FD=""
}

# ONE RUN PER TEST DRIVE, ON EVERY BACKEND.
#
#   rig_claim_drive <dir> <who> <backend> [wait]
#                                        # 0 held, 6 busy, 2 cannot arbitrate
#   rig_drop_drive                       # close it in this shell or subshell
#
# The bridge lock is about the wire and skips SLIRP, so two runs of one tag in
# one checkout -- one drive directory -- still wiped each other's live drive
# before either reached it.  Every launcher takes this before its first wipe,
# restage, Xvfb, tcpdump or emulator start.
#
# KEYED BY THE DRIVE'S REAL PATH, beside the bridge lock (host-wide, and never
# under build/, where a wipe or a clean could delete it):
# aminetxduo-drive-<cksum of the resolved path>.lock, the path in the record.
# Two checkouts with the same tag are two drives and stay parallel.
#
# Blocking and bounded like the bridge: [wait], else AMINETXDUO_DRIVE_WAIT,
# else 1800 seconds, then 6.  2 is a lock file that cannot be made, or no
# flock(1) on a bridged backend.  WITHOUT flock(1) A NON-BRIDGED RUN DEGRADES:
# one warning and 0, unlocked, so a SLIRP or no-network run on a host with no
# flock (macOS) still runs; the wire needs arbitration, a lone drive does not.  Inherited by the emulator, so the drive stays claimed while a guest
# outlives a killed launcher; long-lived helpers call rig_drop_drive first.
#
# LOCK ORDER, EVERYWHERE: DRIVE BEFORE BRIDGE.  A path that held the bridge
# and waited for a drive, beside one that held the drive and waited for the
# bridge, would deadlock until both waits expired.  rig_standing_exempt and
# the port, name and address claims never wait, so they may come anywhere.
RIG_DRIVE_FD="${RIG_DRIVE_FD:-}"
RIG_DRIVE_WAITER="${RIG_DRIVE_WAITER:-}"

# The drive's absolute path with its parent resolved; the drive itself may not
# exist yet, so realpath(1) -- whose -m is GNU-only -- is not used.
rig_drive_realpath() { # dir
    local p b r
    b=$(basename "$1")
    p=$(dirname "$1")
    if r=$(cd "$p" 2> /dev/null && pwd -P); then
        printf '%s/%s\n' "${r%/}" "$b"
    else
        case "$1" in /*) printf '%s\n' "$1" ;; *) printf '%s/%s\n' "$PWD" "$1" ;; esac
    fi
}

rig_drive_path() { # dir
    printf '%s/aminetxduo-drive-%s.lock\n' "$(dirname "$(rig_bridge_path)")" \
           "$(rig_drive_realpath "$1" | cksum | cut -d' ' -f1)"
}

rig_claim_drive() { # dir who backend [wait]
    [ -n "${1:-}" ] || { echo "rig_claim_drive: no drive named" >&2; return 2; }
    [ -n "${3:-}" ] || { echo "rig_claim_drive: no backend given" >&2; return 2; }
    [ -z "$RIG_DRIVE_FD" ] || return 0
    if ! rig_have_flock; then
        rig_backend_bridged "${3:-}" && { rig_no_flock; return 2; }
        echo "no flock: drive $(rig_drive_realpath "$1") not locked;" \
             "do not run two of this tag at once" >&2
        return 0
    fi
    local who="${2:-$$}" limit="${4:-${AMINETXDUO_DRIVE_WAIT:-1800}}"
    local f fd rc start waited real
    mkdir -p "$(dirname "$1")" 2> /dev/null || true
    real=$(rig_drive_realpath "$1")
    f=$(rig_drive_path "$1")

    ( umask 000; : >> "$f" ) 2> /dev/null || {
        echo "cannot create the drive lock $f" >&2; return 2; }
    exec {fd}>>"$f" || { echo "cannot open the drive lock $f" >&2; return 2; }

    if ! flock -n -x "$fd" 2> /dev/null; then
        echo "==> another run is using $real; waiting up to ${limit}s:" >&2
        sed 's/^/    /' "$f" >&2 2> /dev/null || true
        start=$(date +%s)
        rc=0
        if [ "$limit" -gt 0 ] 2> /dev/null; then
            flock -x -w "$limit" "$fd" 2> /dev/null &
            RIG_DRIVE_WAITER=$!
            wait "$RIG_DRIVE_WAITER" || rc=$?
            RIG_DRIVE_WAITER=""
        else
            rc=1
        fi
        waited=$(( $(date +%s) - start ))
        if [ "$rc" != 0 ]; then
            echo "REFUSING to touch $real (waited ${waited}s); it is held by:" >&2
            sed 's/^/    /' "$f" >&2 2> /dev/null || true
            echo "  Wiping it would destroy that run's live drive.  Wait for it," >&2
            echo "  use another AMINETXDUO_RUN_TAG, or raise AMINETXDUO_DRIVE_WAIT." >&2
            exec {fd}>&-
            return 6
        fi
        echo "==> drive free after ${waited}s"
    fi

    printf 'drive path=%s pid=%s who=%s since=%s\n' \
           "$real" "$$" "$who" "$(date +%FT%T)" > "$f"
    RIG_DRIVE_FD="$fd"
    return 0
}

rig_drop_drive() {
    if [ -n "$RIG_DRIVE_WAITER" ]; then
        kill "$RIG_DRIVE_WAITER" 2> /dev/null || true
        RIG_DRIVE_WAITER=""
    fi
    [ -n "$RIG_DRIVE_FD" ] || return 0
    eval "exec ${RIG_DRIVE_FD}>&-" 2> /dev/null || true
    RIG_DRIVE_FD=""
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
    rig_have_flock || { rig_no_flock; return 1; }
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
# `pgrep -a` is procps and macOS does not have it, so the listing is done with
# ps(1), which both have.  The regex still goes to pgrep -f, so the anchoring
# that keeps a shell merely MENTIONING a port out of the answer is unchanged.
rig_pgrep_af() { # regex
    local pids p args
    pids=$(pgrep -f "$1" 2> /dev/null) || return 0
    for p in $pids; do
        [ "$p" = "$$" ] && continue
        args=$(ps -o args= -p "$p" 2> /dev/null | head -1)
        [ -n "$args" ] && printf '%s %s\n' "$p" "$args"
    done
    return 0
}

# `[Pp]ython3?` because Homebrew's python3 execs a binary called `Python`,
# inside Python.app, so an anchored `python3` matches nothing on a Mac and the
# reader goes unseen.
rig_port_readers() { # port
    rig_pgrep_af "^[^ ]*[Pp]ython3?[^ ]* .*serial-timestamp\.py 127\.0\.0\.1 $1 "
    rig_pgrep_af "^[^ ]*nc 127\.0\.0\.1 $1\$"
}

# A one-line description of a pid, for a refusal that names the other run
# rather than only its number.
rig_pid_describe() { # pid
    local pid="$1" cmd
    cmd=$(tr '\0' ' ' < "/proc/$pid/cmdline" 2> /dev/null)
    [ -n "$cmd" ] || cmd=$(ps -o args= -p "$pid" 2> /dev/null)
    printf '%s %s\n' "$pid" "${cmd:-<gone>}"
}
