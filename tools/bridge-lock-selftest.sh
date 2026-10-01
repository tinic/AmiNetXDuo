#!/usr/bin/env bash
#
# Does tools/amiberry-run.sh keep bridged guests apart, end to end?
#
#   tools/bridge-lock-selftest.sh
#
# Two scratch checkouts and a stub emulator, so it needs no Amiberry, no ROM,
# no toolchain and no network, and never waits on a live guest: the bridge
# lock is a file of its own.  It replays 2026-10-01 -- a manual and a CI copy
# of one arm, one tag, two checkouts, bridged on one NIC -- and asserts:
#
#   bridge_serial   the second bridged boot starts only after the first exits,
#                   and within a few seconds of it
#   bridge_released after a normal exit a new contender gets it without waiting
#   mac_per_tag     by default the two share the tag's MAC (no new leases)
#   mac_per_run     AMINETXDUO_MAC_PER_RUN=1 gives two invocations two MACs
#   same_tree_hd    a second bridged run of the same tag in the same checkout
#                   leaves the running guest's drive alone while it waits
#   bridge_kill9    the wrapper killed with -9 while its guest runs: the lock
#                   stays held until the GUEST exits, then the next one boots
#   bridge_refuse   a boot that outwaits AMINETXDUO_BRIDGE_WAIT exits 6 unbooted
#   noflock_rc      no flock(1) on PATH is a missing ingredient (2), never 6
#   unwritable_rc   a lock path that cannot be created is 2, never 6
#   slirp_parallel  two SLIRP boots run at the same time
#   standing_exempt a standing guest (classicwb, demo-rtg, console-instance)
#                   takes no lock and says so; a test run boots beside it
#   standing_mac    its MAC is from the standing range: not a tag MAC, and
#                   different per kind
#   optout_standing amiberry-run.sh with AMINETXDUO_STANDING on a standing MAC
#                   boots while the lock is held, and leaves the lock free
#   optout_testmac  the same opt-out on a test-range MAC is refused with 2
#   demo_standing   tools/demo.sh opts out, on a standing MAC
#   standing_one_per_mac  a second standing launch on a MAC that is up is
#                   refused with 2 and leaves the first one's drive alone;
#                   another standing MAC boots
#   standing_helper_free  a helper outliving its guest does not keep the MAC
#   override_range  each standing launcher's own refusal block, executed:
#                   a test-range MAC (what AMINETXDUO_CWB_MAC or
#                   AMINETXDUO_DEMO_MAC could hold) exits non-zero before the
#                   emulator start, a standing one reaches it; the block comes
#                   before the launcher's first wipe, staging, Xvfb, tcpdump,
#                   kill or emulator line
#   mac_pinned      AMINETXDUO_AMIBERRY_MAC is used as given
#
# Output is key=value and an exit code: 0 all held, 1 one did not, 3 not
# evaluable here (no flock(1) or python3).
#
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

command -v flock > /dev/null 2>&1 && command -v python3 > /dev/null 2>&1 || {
    echo "bridge_selftest=unproven reason=needs-flock(1)-and-python3"
    exit 3
}

S=$(mktemp -d "${TMPDIR:-/tmp}/bridge-selftest.XXXXXX")
# Scoped to this scratch tree: the -9 case orphans a reader on purpose.
trap 'pkill -P $$ 2> /dev/null; pkill -f "$S/" 2> /dev/null; rm -rf "$S"' EXIT

# What tools/amiberry-run.sh reads from its own tree, and nothing else.
for c in A B C D; do
    mkdir -p "$S/$c/tools/envsetup" "$S/$c/build"
    for f in amiberry-run.sh amiberry-resolve.sh emu-board.sh emu-watch.sh \
             emu-rig-lock.sh emu-mac.sh emu-bridge.sh logcap.sh \
             serial-timestamp.py envsetup/envsetup.c; do
        cp "$ROOT/tools/$f" "$S/$c/tools/$f"
    done
    printf '#!/bin/sh\n' > "$S/$c/build/envsetup-m68020"
    chmod +x "$S/$c/build/envsetup-m68020"
done
: > "$S/kick.rom"
: > "$S/Guest"

# The emulator: says it opened the backend, binds the serial port, writes
# DH0:.done after STUB_SECS, and logs start and end -- with the MAC it was
# given and the time -- to one file every instance appends to.  STUB_LIFE
# makes it exit on its own, as a guest outliving a killed wrapper would.
cat > "$S/amiberry" <<'PY'
#!/usr/bin/env python3
import os, re, signal, socket, sys, time
cfg = open(sys.argv[-1]).read()
def g(p):
    m = re.search(p, cfg, re.M)
    return m.group(1) if m else ""
hd = g(r"^uaehf0=dir,rw,DH0:DH0:(.*),0$")
port = int(g(r"^serial_port=tcp://127\.0\.0\.1:(\d+)/wait$"))
mac = g(r"^a2065_rom_options=mac=([^,]*),")
backend = g(r"^a2065_rom_options=mac=[^,]*,([^,\n]*)")
who = os.environ["STUB_WHO"]
def log(s):
    with open(os.environ["STUB_EVENTS"], "a") as f:
        f.write("%s t=%.2f\n" % (s, time.time()))
def term(*_):
    log("end %s" % who)
    sys.exit(0)
signal.signal(signal.SIGTERM, term)
log("start %s mac=%s" % (who, mac))
open(os.path.join(hd, "stub-alive"), "w").write("1\n")
print("UAENET: '%s' open successful" % backend, flush=True)
s = socket.socket()
s.bind(("127.0.0.1", port))
s.listen(1)
t0 = time.time()
life = float(os.environ.get("STUB_LIFE", "0"))
secs = float(os.environ.get("STUB_SECS", "2"))
while time.time() - t0 < secs:
    if life and time.time() - t0 >= life:
        term()
    time.sleep(0.2)
open(os.path.join(hd, ".done"), "w").write("0\n")
while not life or time.time() - t0 < life:
    time.sleep(0.2)
term()
PY
chmod +x "$S/amiberry"

EV="$S/events"
: > "$EV"

# run <checkout> <who> <backend> [VAR=value...]
run() {
    local c="$1" who="$2" be="$3"; shift 3
    env -i PATH="$PATH" HOME="$HOME" TMPDIR="${TMPDIR:-/tmp}" \
        AMIBERRY="$S/amiberry" AMINETXDUO_KICKSTART="$S/kick.rom" \
        AMINETXDUO_RIG_LOCKDIR="$S/rig" \
        AMINETXDUO_BRIDGE_LOCK="$S/bridge.lock" \
        AMINETXDUO_RUN_TAG=ifsurvive AMINETXDUO_ALLOW_SLIRP=1 \
        STUB_EVENTS="$EV" STUB_WHO="$who" "$@" \
        "$S/$c/tools/amiberry-run.sh" -N a2065 -B "$be" -t 30 "$S/Guest" \
        > "$S/$who.out" 2>&1
    echo $? > "$S/$who.rc"
}

WRONG=0
kv() { printf '%s=%s\n' "$1" "$2"; [ "$2" = ok ] || WRONG=$((WRONG + 1)); }

# --------------------------------------------- two bridged, one tag, two trees
# Job control on, so each is its own process group, as two invocations from
# two shells are: that is what emu_run_id tells apart.
set -m
run A manual ens18 STUB_SECS=3 &
sleep 1
run B ci ens18 STUB_SECS=1 &
wait
set +m
cp "$EV" "$S/events.pair"

order=$(awk '{print $1 "-" $2}' "$EV" | tr '\n' ' ')
# How long the lock sat free between the first guest's exit and the second's
# start: the waiter is woken by the release, not by a poll.
gap=$(awk '$1 == "end" && $2 == "manual" { e = substr($NF, 3) }
           $1 == "start" && $2 == "ci" { s = substr($NF, 3) }
           END { printf "%.1f", s - e }' "$EV")
if [ "$order" = "start-manual end-manual start-ci end-ci " ] &&
   [ "$(cat "$S/manual.rc")" = 0 ] && [ "$(cat "$S/ci.rc")" = 0 ] &&
   grep -q 'another bridged guest is up' "$S/ci.out" &&
   awk -v g="$gap" 'BEGIN { exit !(g >= 0 && g < 3) }'; then
    kv bridge_serial ok
else
    kv bridge_serial "wrong:order=[$order]:gap=${gap}s:rc=$(cat "$S/manual.rc"),$(cat "$S/ci.rc")"
fi
echo "bridge_handover_s=$gap"

# Released on a normal exit: a new contender with no wait to spare boots.
: > "$EV"
run C after ens18 STUB_SECS=0.5 AMINETXDUO_BRIDGE_WAIT=1
if [ "$(cat "$S/after.rc")" = 0 ] && grep -q '^start after' "$EV" &&
   ! grep -q 'another bridged guest is up' "$S/after.out"; then
    kv bridge_released ok
else
    kv bridge_released "wrong:rc=$(cat "$S/after.rc")"
fi

mac_a=$(sed -n 's/^start manual mac=\([^ ]*\) .*/\1/p' "$S/events.pair")
mac_b=$(sed -n 's/^start ci mac=\([^ ]*\) .*/\1/p' "$S/events.pair")
if [ -n "$mac_a" ] && [ "$mac_a" = "$mac_b" ] &&
   case "$mac_a" in 02:*) true ;; *) false ;; esac; then
    kv mac_per_tag ok
else
    kv mac_per_tag "wrong:$mac_a,$mac_b"
fi

# The opt-in: two invocations, each its own process group as from two shells.
: > "$EV"
set -m
run A perrun1 ens18 STUB_SECS=0.5 AMINETXDUO_MAC_PER_RUN=1 &
wait
run B perrun2 ens18 STUB_SECS=0.5 AMINETXDUO_MAC_PER_RUN=1 &
wait
set +m
mac_a=$(sed -n 's/^start perrun1 mac=\([^ ]*\) .*/\1/p' "$EV")
mac_b=$(sed -n 's/^start perrun2 mac=\([^ ]*\) .*/\1/p' "$EV")
if [ -n "$mac_a" ] && [ -n "$mac_b" ] && [ "$mac_a" != "$mac_b" ] &&
   case "$mac_a$mac_b" in 02:*02:*) true ;; *) false ;; esac; then
    kv mac_per_run ok
else
    kv mac_per_run "wrong:$mac_a,$mac_b"
fi

# One checkout, one tag, the first guest up: the second must not wipe its
# drive on the way to waiting.  The stub leaves a marker in DH0: at start.
: > "$EV"
run A holder ens18 STUB_SECS=4 &
HOLD=$!
for _ in $(seq 1 50); do grep -q '^start holder' "$EV" && break; sleep 0.2; done
HDA="$S/A/build/amiberry-testhd-ifsurvive"
run A sametree ens18 AMINETXDUO_BRIDGE_WAIT=1 AMINETXDUO_DRIVE_WAIT=1
if [ -e "$HDA/stub-alive" ] && [ "$(cat "$S/sametree.rc")" = 6 ]; then
    kv same_tree_hd ok
else
    kv same_tree_hd "wrong:marker=$([ -e "$HDA/stub-alive" ] && echo kept || echo gone):rc=$(cat "$S/sametree.rc")"
fi
wait "$HOLD"

# ------------------- the wrapper is killed with -9 while its guest is running
: > "$EV"
run A doomed ens18 STUB_SECS=60 STUB_LIFE=5 &
for _ in $(seq 1 50); do grep -q '^start doomed' "$EV" && break; sleep 0.2; done
WRAP=$(pgrep -f "^[^ ]*bash $S/A/tools/amiberry-run.sh" | head -1)
[ -n "$WRAP" ] || WRAP=$(pgrep -f "$S/A/tools/amiberry-run.sh" | head -1)
kill -9 "$WRAP" 2> /dev/null
wait
alive=$(pgrep -f "$S/amiberry" | head -1)
run B heir ens18 STUB_SECS=0.5 AMINETXDUO_BRIDGE_WAIT=30
order=$(awk '{print $1 "-" $2}' "$EV" | tr '\n' ' ')
if [ -n "$WRAP" ] && [ -n "$alive" ] &&
   [ "$order" = "start-doomed end-doomed start-heir end-heir " ] &&
   [ "$(cat "$S/heir.rc")" = 0 ] &&
   grep -q 'another bridged guest is up' "$S/heir.out"; then
    kv bridge_kill9 ok
else
    kv bridge_kill9 "wrong:wrapper=${WRAP:-none}:guest_after_kill=${alive:-none}:order=[$order]:rc=$(cat "$S/heir.rc")"
fi

# ----------------------------------- a missing ingredient is not a busy rig
# PATH with everything on it except flock(1).
mkdir -p "$S/noflock"
IFS=: read -r -a _dirs <<< "$PATH"
for d in "${_dirs[@]}"; do
    for x in "$d"/*; do
        n=${x##*/}
        [ "$n" = flock ] || [ -e "$S/noflock/$n" ] || [ ! -x "$x" ] ||
            ln -s "$x" "$S/noflock/$n" 2> /dev/null
    done
done
fn_rc=$(PATH="$S/noflock" AMINETXDUO_BRIDGE_LOCK="$S/bridge.lock" bash -c '
    . "$1/tools/emu-rig-lock.sh"
    rig_claim_bridge ens18 noflock > /dev/null 2>&1
    echo $?' _ "$ROOT")
: > "$EV"
PATH="$S/noflock" run C noflock ens18
if [ "$fn_rc" = 2 ] && [ "$(cat "$S/noflock.rc")" = 2 ] && [ ! -s "$EV" ]; then
    kv noflock_rc ok
else
    kv noflock_rc "wrong:function=$fn_rc:run=$(cat "$S/noflock.rc")"
fi

# The drive lock degrades without flock(1) when nothing is on the wire: one
# warning and 0, unlocked.  On a bridged backend it stays strict.  Through
# amiberry-run.sh a SLIRP run gets past the drive with the warning; its 2 is
# rig_claim_port's, which needs flock(1) on main as well.
dn=$(PATH="$S/noflock" AMINETXDUO_BRIDGE_LOCK="$S/bridge.lock" bash -c '
    . "$1/tools/emu-rig-lock.sh"
    rig_claim_drive "$2/nf-drive" nf slirp 2> "$2/nf-slirp.err"; a=$?
    rig_claim_drive "$2/nf-drive" nf ens18 2> /dev/null; b=$?
    echo "$a,$b"' _ "$ROOT" "$S")
: > "$EV"
PATH="$S/noflock" run C noflockslirp slirp
if [ "$dn" = 0,2 ] &&
   grep -q '^no flock: drive .*nf-drive not locked; do not run two of this tag at once$' \
       "$S/nf-slirp.err" &&
   grep -q '^no flock: drive .*amiberry-testhd-ifsurvive not locked' "$S/noflockslirp.out" &&
   ! grep -q 'drive lock' "$S/noflockslirp.out"; then
    kv noflock_drive ok
else
    kv noflock_drive "wrong:slirp,bridged=$dn:run=$(cat "$S/noflockslirp.rc")"
fi

: > "$EV"
run C unwritable ens18 AMINETXDUO_BRIDGE_LOCK="$S/no/such/dir/bridge.lock"
if [ "$(cat "$S/unwritable.rc")" = 2 ] && [ ! -s "$EV" ] &&
   grep -qE 'cannot create the (drive|bridge) lock' "$S/unwritable.out"; then
    kv unwritable_rc ok
else
    kv unwritable_rc "wrong:rc=$(cat "$S/unwritable.rc")"
fi

# ------------------------------- a boot that cannot get the bridge in time
: > "$EV"
(
    AMINETXDUO_BRIDGE_LOCK="$S/bridge.lock"
    export AMINETXDUO_BRIDGE_LOCK
    # shellcheck source=emu-rig-lock.sh
    . "$ROOT/tools/emu-rig-lock.sh"
    rig_claim_bridge ens18 "a-held-rig" > /dev/null 2>&1 &&
        touch "$S/held"
    sleep 8
) &
HOLDER=$!
for _ in 1 2 3 4 5 6 7 8 9 10; do [ -e "$S/held" ] && break; sleep 0.2; done
run C refused ens18 AMINETXDUO_BRIDGE_WAIT=2
if [ "$(cat "$S/refused.rc")" = 6 ] && [ ! -s "$EV" ] &&
   grep -q 'a-held-rig' "$S/refused.out"; then
    kv bridge_refuse ok
else
    kv bridge_refuse "wrong:rc=$(cat "$S/refused.rc"):events=$(wc -l < "$EV")"
fi

# ---------------------------- SLIRP is not held back, even beside a holder
run C slirp1 slirp STUB_SECS=3 AMINETXDUO_AMIBERRY_MAC=02:41:4d:49:aa:bb &
sleep 0.5
run D slirp2 slirp STUB_SECS=3 &
wait "$!"
wait
kill "$HOLDER" 2> /dev/null
order=$(awk '{print $1}' "$EV" | tr '\n' ' ')
if [ "$order" = "start start end end " ] &&
   [ "$(cat "$S/slirp1.rc")" = 0 ] && [ "$(cat "$S/slirp2.rc")" = 0 ]; then
    kv slirp_parallel ok
else
    kv slirp_parallel "wrong:order=[$order]"
fi
if grep -q '^start slirp1 mac=02:41:4d:49:aa:bb ' "$EV"; then
    kv mac_pinned ok
else
    kv mac_pinned "wrong:$(grep '^start slirp1' "$EV")"
fi

# ----------------------------------- a standing guest is exempt from the lock
# The three launchers, statically: they announce the exemption, take their
# MAC from the standing range, and never claim.
st_bad=""
for f in tools/classicwb.sh tools/demo-rtg.sh tests/tools/console-instance.sh; do
    body=$(grep -v '^[[:space:]]*#' "$ROOT/$f")
    case "$body" in *rig_claim_bridge*) st_bad="$st_bad $f:claims" ;; esac
    case "$body" in *rig_standing_exempt*) ;; *) st_bad="$st_bad $f:silent" ;; esac
    case "$body" in *emu_mac_standing*) ;; *) st_bad="$st_bad $f:mac" ;; esac
done

# And live: a standing guest is started the way they start one -- announce,
# then the emulator -- and a test run boots on the bridge while it is up.
: > "$EV"
SHD="$S/standing-hd"; mkdir -p "$SHD"
(
    # shellcheck source=emu-mac.sh
    . "$ROOT/tools/emu-mac.sh"
    # shellcheck source=emu-rig-lock.sh
    . "$ROOT/tools/emu-rig-lock.sh"
    smac=$(emu_mac_standing classicwb "A1200:full:$S")
    rig_standing_exempt "classicwb in $S" "$smac" ens18
    port=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])')
    printf '%s\n' "config_description=AmiNetXDuo standing" \
        "uaehf0=dir,rw,DH0:DH0:$SHD,0" \
        "a2065_rom_options=mac=$smac,ens18" \
        "serial_port=tcp://127.0.0.1:$port/wait" > "$S/standing.uae"
    AMINETXDUO_BRIDGE_LOCK="$S/bridge.lock" STUB_EVENTS="$EV" \
        STUB_WHO=standing STUB_SECS=60 STUB_LIFE=10 \
        exec "$S/amiberry" -f "$S/standing.uae"
) > "$S/standing.out" 2>&1 &
STAND=$!
for _ in $(seq 1 50); do grep -q '^start standing' "$EV" && break; sleep 0.2; done
free_beside=no
( exec 9>>"$S/bridge.lock"; flock -n 9 ) && free_beside=yes
run B beside ens18 STUB_SECS=0.5 AMINETXDUO_BRIDGE_WAIT=1
order=$(awk '{print $1 "-" $2}' "$EV" | tr '\n' ' ')
kill "$STAND" 2> /dev/null
wait "$STAND" 2> /dev/null
if [ -z "$st_bad" ] && [ "$free_beside" = yes ] &&
   [ "$order" = "start-standing start-beside end-beside " ] &&
   [ "$(cat "$S/beside.rc")" = 0 ] &&
   grep -q '^bridge_lock=exempt standing=classicwb' "$S/standing.out"; then
    kv standing_exempt ok
else
    kv standing_exempt "wrong:static=[${st_bad# }]:lock_free=$free_beside:order=[$order]:rc=$(cat "$S/beside.rc")"
fi

smac=$(sed -n 's/^start standing mac=\([^ ]*\) .*/\1/p' "$EV")
tmac=$(sed -n 's/^start beside mac=\([^ ]*\) .*/\1/p' "$EV")
# shellcheck source=emu-mac.sh
. "$ROOT/tools/emu-mac.sh"
kinds=$(for k in classicwb demo-rtg console-instance; do
            emu_mac_standing "$k" same-instance; done | sort -u | wc -l | tr -d " ")
# The fourth byte is the one an A2065 keeps, so that is where they must part.
if [ -n "$smac" ] && [ -n "$tmac" ] && [ "$smac" != "$tmac" ] &&
   [ "$(echo "$smac" | cut -d: -f4)" != "$(echo "$tmac" | cut -d: -f4)" ] &&
   [ "$kinds" = 3 ]; then
    kv standing_mac ok
else
    kv standing_mac "wrong:standing=$smac:test=$tmac:kinds=$kinds"
fi
echo "standing_macs=$smac,$tmac"

# ------------------------- amiberry-run.sh's opt-out, for tools/demo.sh
: > "$EV"
(
    AMINETXDUO_BRIDGE_LOCK="$S/bridge.lock"
    export AMINETXDUO_BRIDGE_LOCK
    # shellcheck source=emu-rig-lock.sh
    . "$ROOT/tools/emu-rig-lock.sh"
    rig_claim_bridge ens18 "a-test-arm" > /dev/null 2>&1 && touch "$S/held2"
    sleep 12
) &
HOLDER2=$!
for _ in $(seq 1 25); do [ -e "$S/held2" ] && break; sleep 0.2; done
run D demo ens18 STUB_SECS=2 AMINETXDUO_BRIDGE_WAIT=1 AMINETXDUO_STANDING=demo &
DEMO=$!
for _ in $(seq 1 50); do grep -q '^start demo' "$EV" && break; sleep 0.2; done
wait "$DEMO"
dmac=$(sed -n 's/^start demo mac=\([^ ]*\) .*/\1/p' "$EV")
if [ "$(cat "$S/demo.rc")" = 0 ] && [ -n "$dmac" ] &&
   emu_mac_is_standing "$dmac" &&
   grep -q '^bridge_lock=exempt standing=demo' "$S/demo.out" &&
   ! grep -q 'another bridged guest is up' "$S/demo.out"; then
    kv optout_standing ok
else
    kv optout_standing "wrong:rc=$(cat "$S/demo.rc"):mac=$dmac"
fi
kill "$HOLDER2" 2> /dev/null
wait "$HOLDER2" 2> /dev/null

# And the opt-out does not hold the lock itself: with nobody else on the
# bridge, the lock file is free while the standing guest is up.
: > "$EV"
run D demo2 ens18 STUB_SECS=3 AMINETXDUO_STANDING=demo &
DEMO=$!
for _ in $(seq 1 50); do grep -q '^start demo2' "$EV" && break; sleep 0.2; done
free_demo=no
( exec 9>>"$S/bridge.lock"; flock -n 9 ) && free_demo=yes
wait "$DEMO"
[ "$free_demo" = yes ] || kv optout_standing "wrong:the opt-out held the lock"

: > "$EV"
run D testmac ens18 AMINETXDUO_STANDING=demo \
    AMINETXDUO_AMIBERRY_MAC=02:41:4d:49:00:77
if [ "$(cat "$S/testmac.rc")" = 2 ] && [ ! -s "$EV" ] &&
   grep -q 'REFUSING to start standing guest' "$S/testmac.out"; then
    kv optout_testmac ok
else
    kv optout_testmac "wrong:rc=$(cat "$S/testmac.rc")"
fi

demo=$(grep -v '^[[:space:]]*#' "$ROOT/tools/demo.sh")
case "$demo" in
    *"AMINETXDUO_STANDING=demo"*"emu_mac_standing demo"*) kv demo_standing ok ;;
    *) kv demo_standing "wrong:tools/demo.sh does not opt out on a standing MAC" ;;
esac

# Every standing launcher's own refusal block, RUN: the lines from its
# rig_standing_exempt call to the block's closing brace are cut out of the
# launcher and executed with the MAC an override could hold, followed by a
# stand-in for the emulator start.  A test-range MAC must exit non-zero with
# the stand-in never reached; a standing one must reach it.
#
# AND IT MUST COME FIRST: the validation line has to precede the launcher's
# first destructive step -- a wipe, a staging copy or mkdir, the shared
# Workbench build, Xvfb, tcpdump, a kill or the emulator -- so a refused
# launch leaves everything as it was.  console-instance is scanned from the
# end of its action switch: above that are `stop` and `status`, which are not
# the start path.
DESTRUCTIVE='rm -r|rm -f|mkdir |cp |wb31_assemble|Xvfb |tcpdump -i|kill |pkill |stop_pid |start_emulator|(exec|setsid) +(setsid +)?"[$]AMIBERRY"|: > |lha -?x'
ov_bad=""
for spec in tools/classicwb.sh:1 tools/demo-rtg.sh:1 \
            tests/tools/console-instance.sh:"esac"; do
    f=${spec%%:*}; from=${spec#*:}
    [ "$from" != "esac" ] ||
        from=$(grep -n '^esac' "$ROOT/$f" | head -1 | cut -d: -f1)
    blk=$(awk '/^rig_standing_exempt /{on=1} on{print} on&&/^}/{exit}' "$ROOT/$f")
    at=$(grep -n '^rig_standing_exempt ' "$ROOT/$f" | head -1 | cut -d: -f1)
    first=$(awk -v from="$from" -v re="$DESTRUCTIVE" '
        NR >= from && $0 !~ /^[[:space:]]*#/ && $0 ~ re { print NR; exit }' \
        "$ROOT/$f")
    echo "standing_validation_$(basename "$f" .sh)=line$at first_destructive=line${first:-none}"
    [ -n "$blk" ] && [ -n "$at" ] && [ -n "$first" ] && [ "$at" -lt "$first" ] ||
        { ov_bad="$ov_bad $f:order(${at:-none}<${first:-none})"; continue; }
    for mac in 02:41:4d:49:00:77 02:41:4d:47:2a:01 held; do
        rm -f "$S/launched"
        if [ "$mac" = held ]; then
            # The address is already up: another instance holds its lock.
            mac=02:41:4d:47:2a:02
            ( AMINETXDUO_BRIDGE_LOCK="$S/bridge.lock"
              . "$ROOT/tools/emu-rig-lock.sh"
              rig_standing_exempt other "$mac" ens18 > /dev/null 2>&1 &&
                  touch "$S/mac-held"
              sleep 4 ) &
            for _ in $(seq 1 25); do [ -e "$S/mac-held" ] && break; sleep 0.2; done
        fi
        printf '%s\n' "export AMINETXDUO_BRIDGE_LOCK=\"$S/bridge.lock\"" \
            ". \"$ROOT/tools/emu-rig-lock.sh\"" 'say() { :; }' \
            'SNIFFER=""; TAG=t; RUN=r; BACKEND=ens18; MODEL=A1200; VARIANT=v' \
            "MAC=$mac" "$blk" "touch \"$S/launched\"" > "$S/block.sh"
        bash "$S/block.sh" > /dev/null 2>&1
        brc=$?
        case "$mac" in
            02:41:4d:49:*) [ "$brc" != 0 ] && [ ! -e "$S/launched" ] ||
                               ov_bad="$ov_bad $f:test-mac-rc=$brc" ;;
            *:02)          [ "$brc" = 2 ] && [ ! -e "$S/launched" ] ||
                               ov_bad="$ov_bad $f:held-mac-rc=$brc" ;;
            *)             [ "$brc" = 0 ] && [ -e "$S/launched" ] ||
                               ov_bad="$ov_bad $f:standing-mac-rc=$brc" ;;
        esac
        wait 2> /dev/null; rm -f "$S/mac-held"
    done

    # Every long-lived helper closes the standing and drive locks in its own
    # subshell (Xvfb, tcpdump, the log capper), and the script drops its
    # copies right after the emulator starts, so the watchdog and readers
    # never get them.
    while IFS=: read -r n _; do
        sed -n "$((n - 1)),${n}p" "$ROOT/$f" |
            grep -q 'rig_drop_standing; rig_drop_drive' ||
            ov_bad="$ov_bad $f:$n:helper-keeps-lock"
    done < <(grep -nE '^[[:space:]]*(\( *rig_drop_standing; *(rig_drop_drive; *)?)?(exec +)?(Xvfb |tcpdump -i|"[$]ROOT/tools/logcap\.sh")' "$ROOT/$f")
    while IFS=: read -r n _; do
        sed -n "$((n + 1)),$((n + 4))p" "$ROOT/$f" |
            grep -q '^rig_drop_standing; rig_drop_drive' ||
            ov_bad="$ov_bad $f:$n:script-keeps-lock"
    done < <(grep -nE '^(setsid "[$]AMIBERRY"|start_emulator "[$]CFG"|\( trap .* exec "[$]AMIBERRY")' "$ROOT/$f")
done
if [ -z "$ov_bad" ]; then
    kv override_range ok
else
    kv override_range "wrong:${ov_bad# }"
fi

# ----------------------------------------- one standing address, one guest
# Through amiberry-run.sh's opt-out: the same standing MAC a second time is
# refused with 2, never boots, and leaves the first one's drive alone, even
# in the same checkout under the same tag; another standing MAC boots.
: > "$EV"
run D sm1 ens18 STUB_SECS=60 STUB_LIFE=6 AMINETXDUO_STANDING=demo \
    AMINETXDUO_AMIBERRY_MAC=02:41:4d:47:4a:01 &
SM1=$!
for _ in $(seq 1 50); do grep -q '^start sm1' "$EV" && break; sleep 0.2; done
run D sm1dup ens18 AMINETXDUO_STANDING=demo \
    AMINETXDUO_AMIBERRY_MAC=02:41:4d:47:4a:01
run C sm2 ens18 STUB_SECS=0.5 AMINETXDUO_STANDING=demo \
    AMINETXDUO_AMIBERRY_MAC=02:41:4d:47:4a:02
HD1="$S/D/build/amiberry-testhd-ifsurvive"
if [ "$(cat "$S/sm1dup.rc")" = 2 ] && ! grep -q '^start sm1dup' "$EV" &&
   [ -e "$HD1/stub-alive" ] &&
   grep -q 'mac=02:41:4d:47:4a:01 is already up' "$S/sm1dup.out" &&
   [ "$(cat "$S/sm2.rc")" = 0 ] && grep -q '^start sm2' "$EV"; then
    kv standing_one_per_mac ok
else
    kv standing_one_per_mac "wrong:dup_rc=$(cat "$S/sm1dup.rc"):drive=$([ -e "$HD1/stub-alive" ] && echo kept || echo gone):other_rc=$(cat "$S/sm2.rc")"
fi
wait "$SM1" 2> /dev/null

# A helper that outlives its guest does not keep the address: the guest
# inherits the lock, the helper drops it, the launcher drops its own copy.
(
    AMINETXDUO_BRIDGE_LOCK="$S/bridge.lock"
    . "$ROOT/tools/emu-rig-lock.sh"
    rig_standing_exempt helper-case 02:41:4d:47:4a:03 ens18 > /dev/null || exit
    ( rig_drop_standing; exec sleep 15 ) &
    echo $! > "$S/helper.pid"
    sleep 2 &
    echo $! > "$S/guest.pid"
    rig_drop_standing
)
for _ in $(seq 1 30); do
    kill -0 "$(cat "$S/guest.pid")" 2> /dev/null || break; sleep 0.2
done
helper_alive=no
kill -0 "$(cat "$S/helper.pid")" 2> /dev/null && helper_alive=yes
again=$( AMINETXDUO_BRIDGE_LOCK="$S/bridge.lock"
         . "$ROOT/tools/emu-rig-lock.sh"
         rig_standing_exempt relaunch 02:41:4d:47:4a:03 ens18 > /dev/null 2>&1
         echo $? )
kill "$(cat "$S/helper.pid")" 2> /dev/null
if [ "$helper_alive" = yes ] && [ "$again" = 0 ]; then
    kv standing_helper_free ok
else
    kv standing_helper_free "wrong:helper_alive=$helper_alive:relaunch_rc=$again"
fi

# ------------------------------------------------ one run per drive, any backend
# Two SLIRP runs of one tag in one checkout share a drive: the second waits,
# without touching it, and boots once the first has exited.
: > "$EV"
run A d1 slirp STUB_SECS=3 &
D1=$!
for _ in $(seq 1 50); do grep -q '^start d1' "$EV" && break; sleep 0.2; done
run A d2 slirp STUB_SECS=0.5 &
D2=$!
sleep 1.5
HDA="$S/A/build/amiberry-testhd-ifsurvive"
kept=no; [ -e "$HDA/stub-alive" ] && grep -q '^start d1' "$EV" &&
    ! grep -q '^start d2' "$EV" && kept=yes
wait "$D1" "$D2"
order=$(awk '{print $1 "-" $2}' "$EV" | tr '\n' ' ')
if [ "$kept" = yes ] && [ "$order" = "start-d1 end-d1 start-d2 end-d2 " ] &&
   [ "$(cat "$S/d1.rc")" = 0 ] && [ "$(cat "$S/d2.rc")" = 0 ] &&
   grep -q 'another run is using' "$S/d2.out"; then
    kv drive_serial ok
else
    kv drive_serial "wrong:kept_while_waiting=$kept:order=[$order]:rc=$(cat "$S/d1.rc"),$(cat "$S/d2.rc")"
fi

# Two tags in one checkout are two drives: parallel.
: > "$EV"
run A t1 slirp STUB_SECS=2 AMINETXDUO_RUN_TAG=drive-t1 &
run A t2 slirp STUB_SECS=2 AMINETXDUO_RUN_TAG=drive-t2 &
wait
order=$(awk '{print $1}' "$EV" | tr '\n' ' ')
if [ "$order" = "start start end end " ] &&
   [ "$(cat "$S/t1.rc")" = 0 ] && [ "$(cat "$S/t2.rc")" = 0 ]; then
    kv drive_parallel ok
else
    kv drive_parallel "wrong:order=[$order]:rc=$(cat "$S/t1.rc"),$(cat "$S/t2.rc")"
fi

# A run that cannot get the drive in time exits 6, unbooted, and the holder's
# drive is untouched.
: > "$EV"
run A dh slirp STUB_SECS=4 &
DH=$!
for _ in $(seq 1 50); do grep -q '^start dh' "$EV" && break; sleep 0.2; done
run A dr slirp AMINETXDUO_DRIVE_WAIT=1
if [ "$(cat "$S/dr.rc")" = 6 ] && ! grep -q '^start dr' "$EV" &&
   [ -e "$HDA/stub-alive" ] && grep -q 'REFUSING to touch' "$S/dr.out"; then
    kv drive_refuse ok
else
    kv drive_refuse "wrong:rc=$(cat "$S/dr.rc"):drive=$([ -e "$HDA/stub-alive" ] && echo kept || echo gone)"
fi
wait "$DH"

# LOCK ORDER: drive before bridge.  A holder up on tag t1 and the bridge;
# behind it a t1 SLIRP run (wants the drive), a t2 bridged run (wants the
# bridge) and a t1 bridged run (wants both).  With one order nothing can hold
# one lock while waiting for the other's, so all four finish well inside the
# bound; mixed orders would sit out both waits.
: > "$EV"
t0=$(date +%s)
run A oh ens18 STUB_SECS=2 AMINETXDUO_RUN_TAG=order-t1 &
for _ in $(seq 1 50); do grep -q '^start oh' "$EV" && break; sleep 0.2; done
run A ox slirp STUB_SECS=1 AMINETXDUO_RUN_TAG=order-t1 \
    AMINETXDUO_DRIVE_WAIT=30 AMINETXDUO_BRIDGE_WAIT=30 &
run A oy ens18 STUB_SECS=1 AMINETXDUO_RUN_TAG=order-t2 \
    AMINETXDUO_DRIVE_WAIT=30 AMINETXDUO_BRIDGE_WAIT=30 &
run A oz ens18 STUB_SECS=1 AMINETXDUO_RUN_TAG=order-t1 \
    AMINETXDUO_DRIVE_WAIT=30 AMINETXDUO_BRIDGE_WAIT=30 &
wait
took=$(( $(date +%s) - t0 ))
rcs=$(cat "$S/oh.rc" "$S/ox.rc" "$S/oy.rc" "$S/oz.rc" | tr -d '\n')
ends=$(grep -c '^end o' "$EV")
if [ "$rcs" = 0000 ] && [ "$ends" = 4 ] && [ "$took" -lt 25 ]; then
    kv lock_order_live ok
else
    kv lock_order_live "wrong:rc=$rcs:ended=$ends:took=${took}s"
fi
echo "lock_order_took_s=$took"

# EVERY LAUNCHER, statically: any script that starts an emulator claims its
# drive before its first destructive step (a wipe, an extraction, a mkdir or
# cp restage, the shared Workbench build, Xvfb, tcpdump, a kill or the
# emulator) and before the emulator itself, and a bridged one claims the
# bridge after the drive and before the emulator.  Found by what they do, so
# a new launcher with no claim fails here rather than waiting for a reviewer
# to spot it.  A line run through ssh acts on the remote host, not on this
# host's drive, and is not a destructive step here.
#
# Emulator -> the line that starts it.  Discovery is the union of the rows;
# a launcher's emulator line is its first non-comment match of any row.
declare -A EMU_START=(
    [amiberry]='(exec|setsid) +(setsid +)?"[$]AMIBERRY"|"[$]AMIBERRY" +(--log +)?-f|start_emulator "'
    [winuae]='ssh .*powershell .*run[.]ps1 -Config '
)
# Launchers discovery must find; a pattern that drifts from one fails here.
KNOWN_LAUNCHERS="tools/amiberry-run.sh install/test/run-workbench.sh tools/winuae-run.sh"
START=""
for emu in "${!EMU_START[@]}"; do START="${START:+$START|}${EMU_START[$emu]}"; done
DESTRUCTIVE_ALL="$DESTRUCTIVE|lha +x|tar x|$START"
lo_bad=""
launchers=$(cd "$ROOT" && grep -rlE "$START" --include='*.sh' tools tests install |
            grep -v '^tools/bridge-lock-selftest\.sh$' | sort)
for f in $launchers; do
    from=1
    [ "$f" != tests/tools/console-instance.sh ] ||
        from=$(grep -n '^esac' "$ROOT/$f" | head -1 | cut -d: -f1)
    first() { awk -v from="$from" -v re="$1" -v skip="${2:-^$}" '
        NR >= from && $0 !~ /^[[:space:]]*#/ && $0 !~ skip && $0 ~ re { print NR; exit }' "$ROOT/$f"; }
    d=$(first '^[[:space:]]*rig_claim_drive ')
    b=$(first '^[[:space:]]*rig_claim_bridge ')
    x=$(first "$DESTRUCTIVE_ALL" '^[[:space:]]*ssh ')
    e=$(first "$START")
    echo "launcher_$(basename "$f" .sh)=drive:${d:-none} bridge:${b:-none} first_destructive:${x:-none} emulator:${e:-none}"
    [ -n "$e" ] || { lo_bad="$lo_bad $f:no-emulator-start"; continue; }
    [ -n "$d" ] || { lo_bad="$lo_bad $f:no-drive-claim"; continue; }
    [ "$d" -lt "$e" ] || lo_bad="$lo_bad $f:drive($d)>=emulator($e)"
    [ -z "$x" ] || [ "$d" -lt "$x" ] || lo_bad="$lo_bad $f:drive($d)>=destructive($x)"
    if [ -n "$b" ]; then
        [ "$d" -lt "$b" ] || lo_bad="$lo_bad $f:bridge($b)-before-drive($d)"
        [ "$b" -lt "$e" ] || lo_bad="$lo_bad $f:bridge($b)>=emulator($e)"
    fi
done
for f in $KNOWN_LAUNCHERS; do
    grep -qxF "$f" <<< "$launchers" || lo_bad="$lo_bad $f:not-discovered"
done
if [ -z "$lo_bad" ]; then
    kv launcher_order ok
else
    kv launcher_order "wrong:${lo_bad# }"
fi

echo "bridge_selftest=$WRONG"
[ "$WRONG" = 0 ] || {
    for f in "$S"/*.out; do echo "---- $f"; tail -15 "$f"; done
    exit 1
}
exit 0
