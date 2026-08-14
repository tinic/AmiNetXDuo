#!/usr/bin/env bash
#
# The compiler runtime, on every 68k this ships one binary for.
#
#   tests/common/run-rt.sh [-b BUILDDIR] [-t SECONDS] [-c CLASSES]
#
# WHAT IT PROVES
#
#   tests/common/rt_test.c runs each of __mulsi3, __udivsi3, __umodsi3,
#   __divsi3, __modsi3, __muldi3, __udivdi3, __umoddi3, __divdi3, __moddi3,
#   __lshrdi3, __ashldi3 and __ashrdi3 against a reference built from MULU.W
#   and single-bit shifts, in both of the forms ami_rt_cpu_select() chooses
#   between, on a 68000, a 68020, a 68030, a 68040 and a 68060.
#
# WHY IT NEEDS FIVE BOOTS AND NOT ONE
#
#   Because the routines differ per machine.  src/common/ami_udivdi3.c is this
#   toolchain's libgcc -- the shipped libgcc.a is a zero-byte file -- and one
#   binary for every 68k means it is compiled -m68000 and picks its MULU.L and
#   DIVU.L paths from AttnFlags at run time.  A green run on an A1200 says
#   nothing about the A600 arm, and the 68000 form is the one nothing else in
#   tier 2 exercises: it is what a 68000 runs and it has no other witness.
#
#   The 68060 arm is a boot like the others and is NOT where the 68060 code
#   path is covered.  AFF_68060 is set by 68060.library rather than by any
#   ROM, so under an emulator that does not load it a 68060 reports AttnFlags
#   0x0F and selects the same forms an 040 does.  rt_test asks for the
#   combination by hand on every 020-and-up machine, which is the "no-mulul"
#   pass in its output.
#
# WHAT IT FOUND
#
#   __lshrdi3, __ashldi3 and __ashrdi3 each compiled into a call to
#   themselves: `value >> count` on a 64-bit value with a variable count is
#   exactly what GCC lowers to a call to __lshrdi3.  Reached from
#   __udivmoddi4's wide-divisor branch, it ate the 4 KB Shell stack and took
#   the machine down.  tools/check-rt-recursion.sh is the static half.
#
# Output is key=value and one RESULT= line.  Exit codes: 0 pass, 1 a class
# failed or crashed, 2 refused before starting.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT" || exit 2

TIMEOUT=200
BUILD="${AMINETXDUO_BUILD:-build/ci/default}"
CLASSES="68000 68020 68030 68040 68060"

while getopts "b:t:c:" opt; do
    case "$opt" in
        b) BUILD="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        c) CLASSES="$OPTARG" ;;
        *) echo "usage: $0 [-b builddir] [-t seconds] [-c classes]" >&2
           echo "RESULT=refused"; exit 2 ;;
    esac
done
case "$BUILD" in /*) ;; *) BUILD="$ROOT/$BUILD" ;; esac

BIN="$BUILD/tests/common/rt_test"
[ -f "$BIN" ] || {
    echo "reason=no_binary path=$BIN"
    echo "build it first: cmake --build $BUILD --parallel --target rt_test" >&2
    echo "RESULT=refused"; exit 2; }

[ -n "${AMINETXDUO_KICKSTART:-}${AMINETXDUO_KICKSTART_A600:-}" ] || {
    echo "reason=no_kickstart"
    echo "No Kickstart.  Set AMINETXDUO_KICKSTART=<rom>." >&2
    echo "RESULT=refused"; exit 2; }

# The machine each class is asked for on.  A 68030, a 68040 and a 68060 are
# the same A3000 with -c, which is how tools/amiberry-run.sh spells a CPU that
# is not the model's own; the ROM follows the model, and a mismatched pair is
# a black screen rather than a failure with a message.
model_of() {
    case "$1" in
        68000) echo "A600" ;;
        68020) echo "A1200" ;;
        *)     echo "A3000" ;;
    esac
}
# -c only where the CPU is not the model's own.  An A3000 IS a 68030, and
# asking for one explicitly is not a no-op: it leaves Amiberry with
# `no CPU emulation cores available CPU=680000` and a guest that never boots.
cpu_arg_of() {
    case "$1" in
        68040|68060) echo "-c $1" ;;
        *) echo "" ;;
    esac
}

fails=0
ran=0

# All five at once.  They need no network, no peer and no port, so nothing
# they touch is shared but the host's cores -- and five boots one after another
# is four minutes of gate for a test whose whole point is that it is cheap.
# Each writes its own log and its own emulator state, keyed by the run tag.
for class in $CLASSES; do
    model=$(model_of "$class")
    # shellcheck disable=SC2046
    AMINETXDUO_RUN_TAG="rt$class" "$ROOT/tools/amiberry-run.sh" \
        -t "$TIMEOUT" -m "$model" $(cpu_arg_of "$class") "$BIN" \
        > "$ROOT/build/rt-$class.log" 2>&1 &
    eval "pid_$class=\$!"
done

for class in $CLASSES; do
    model=$(model_of "$class")
    log="$ROOT/build/rt-$class.log"

    eval "wait \$pid_$class"
    rc=$?
    ran=$((ran + 1))

    # The guest's own verdict, not the runner's exit code: a crash leaves the
    # harness to time out, and a PASS line that is not there is the failure.
    verdict=$(grep -oE '^(PASS|FAIL)$' "$log" | tail -1)
    counts=$(grep -oE '^checks=[0-9]+ failures=[0-9]+$' "$log" | tail -1)
    attn=$(grep -oE '^attnflags=[0-9A-Fa-f]+' "$log" | tail -1)
    # Unique, because the harness prints the guest's serial log and the file
    # it wrote, and every line is therefore in there twice.
    forms=$(grep -oE '^(portable|hardware|no-mulul)=ok' "$log" | sort -u | wc -l | tr -d ' ')

    if [ "$verdict" = PASS ] && [ "$rc" = 0 ]; then
        echo "rt_$class=pass model=$model $attn forms=$forms ${counts:-checks=?}"
    else
        fails=$((fails + 1))
        echo "rt_$class=FAIL model=$model $attn rc=$rc ${counts:-checks=none}"
        grep -E 'ILLEGAL|Illegal instruction|TIMEOUT|^FAIL ' "$log" |
            head -5 | sed 's/^/  /'
        echo "  log=$log"
    fi
done

echo "rt_classes=$ran rt_failed=$fails"
if [ "$fails" = 0 ]; then
    echo "RESULT=pass"
    exit 0
fi
echo "RESULT=fail"
exit 1
