#!/usr/bin/env bash
#
# THE GUEST'S SERIAL LOG: WHERE IT IS, WHETHER ANYTHING COULD BE IN IT, AND
# WHAT TO SAY WHEN THERE IS NOT.
#
#   . "$ROOT/tools/serial-log.sh"
#
#   serial_log_path [tag]                       the file tools/amiberry-run.sh wrote
#   serial_log_stage_env <stagedir> <level>     ENV:ANXDLOGLEVEL for the guest
#   serial_log_state <builddir>                 on | off | unknown
#   serial_log_have  <file> <builddir> <what>   0 if <what> can be checked
#
# AMI_ERROR, AMI_WARN and AMI_INFO are compiled into EVERY build, this tree's
# shipping configuration included.  What decides whether a line reaches the
# serial port is the RUNTIME level, ami_log_level(), which starts at
# AMI_LOG_WARN.  A harness that wants the AMI_LOG_INFO tier -- the bring-up
# marks, the address lines -- stages ENV:ANXDLOGLEVEL, and that is the whole
# difference between the guest it runs and the machine a user has.
#
# So an empty log is still a RESULT and not a passing assertion, but the cause
# is no longer a build option: either nothing happened that logs at the level
# asked for, or the level was never staged, or the run never started.
#
# tools/amiberry-run.sh writes `build/amiberry-serial-<tag>.log`, never
# `build/serial-<tag>.log`; tools/enforcer-run.sh writes the other spelling,
# which is the reason the wrong one looks plausible.
#
# SPDX-License-Identifier: MIT

# Callers set ROOT before sourcing this; every harness in the tree already does.

# The file tools/amiberry-run.sh wrote for this run.  The tag defaults the same
# way the runner defaults it, so a caller that exported AMINETXDUO_RUN_TAG need
# pass nothing.
serial_log_path() {
    echo "${ROOT}/build/amiberry-serial-${1:-${AMINETXDUO_RUN_TAG:-amiberry}}.log"
}

# Ask the guest for <level>, 0..4, by writing the variable the stack reads at
# bring-up.  <stagedir>/env is merged into DH0:env by tools/amiberry-run.sh,
# and envsetup assigns ENV: to it before anything under test runs, so the
# caller has only to pass "$STAGE/env" among the extras.
#
# No trailing newline: ami_config_log_level() accepts one, and leaving it out
# keeps the file the same two bytes a `SetEnv` on the guest would write.
serial_log_stage_env() {
    local stage="$1" level="$2"

    mkdir -p "$stage/env"
    printf '%s' "$level" > "$stage/env/ANXDLOGLEVEL"
}

# Does the library in <builddir> carry the diagnostic sentences at all?
#
# Echoes on, off or unknown, and returns 0, 1 or 2 to match.  There is no
# build option to read any more, so the LIBRARY ITSELF is the only answer: the
# format strings are linked because the macros expand to a call, and a build
# that has none of them is a stale tree or one somebody configured by hand
# with the macros defined away.  `off` is a defect now, not a configuration.
serial_log_state() {
    local build="$1" lib

    # A caller's -b is relative to the tree, and every harness cd's to it
    # before this is reached; an absolute one is left alone.
    case "$build" in
        /*) ;;
        *)  build="${ROOT:-.}/$build" ;;
    esac

    for lib in "$build/src/bsdsocket/bsdsocket.library" \
               "$build/bsdsocket.library"; do
        [ -f "$lib" ] || continue
        if strings -a "$lib" 2>/dev/null | grep -q 'netstack: mark'; then
            echo on; return 0
        fi
        echo off; return 1
    done

    echo unknown
    return 2
}

# Can <what> be checked at all?
#
#   serial_log_have <file> <builddir> <what>
#
# Prints the machine-readable trio every caller of this wants in its output --
#
#   serial=<path> serial_bytes=<n> serial_log=<on|off|unknown>
#
# -- and returns 0 when there is something to read.  A caller takes the
# non-zero branch as a FAILURE, not a skip: its assertion has no input, which
# is a fact about the rig and has to be as loud as a red check.
#
# <builddir> may be empty when the caller has no build to blame; the state is
# then reported as unknown and the advice is unchanged.
serial_log_have() {
    local file="$1" build="${2:-}" what="$3"
    local bytes=0 state=unknown

    if [ -f "$file" ]; then bytes=$(wc -c < "$file" | tr -d ' '); fi
    if [ -n "$build" ]; then state=$(serial_log_state "$build") || true; fi

    echo "serial=$file serial_bytes=$bytes serial_log=$state"
    if [ "$bytes" -gt 0 ]; then return 0; fi

    {
        echo "!! NOTHING IS IN THE SERIAL LOG, so '$what' was NOT CHECKED."
        echo "!! $file is $bytes bytes."
        case "$state" in
            off)
                echo "!! $build holds no ami_log() sentences at all.  They are in every"
                echo "!! build now, so this is a stale tree or one configured by hand."
                echo "!! Reconfigure and rebuild it." ;;
            on)
                echo "!! ${build:-the build} HAS the sentences, so what is left is the"
                echo "!! LEVEL or the run.  ami_log_level() starts at AMI_LOG_WARN, so"
                echo "!! nothing at AMI_LOG_INFO reaches the port unless the harness"
                echo "!! stages ENV:ANXDLOGLEVEL (serial_log_stage_env).  Failing that,"
                echo "!! the guest reached nothing that logs, or the run never started:"
                echo "!! read the emulator log beside this file." ;;
            *)
                echo "!! Nothing here can say what ${build:-the build} holds; there is no"
                echo "!! library to read.  Build it first." ;;
        esac
    } >&2
    return 1
}

# The same question asked by a harness whose ENTIRE product is read out of the
# serial log, before it spends a boot finding out.  Prints the same trio, and
# exits 2 -- the tree's code for a missing ingredient -- when the build cannot
# produce one.
serial_log_require_build() {
    local build="$1" name="$2" state

    state=$(serial_log_state "$build") || true
    echo "serial_log=$state build=$build"
    [ "$state" = off ] || return 0

    {
        echo "!! $name reads guest ami_log() output, and there is not one sentence"
        echo "!! in $build.  AMI_ERROR, AMI_WARN and AMI_INFO are compiled into"
        echo "!! every build, so a library without them is stale or was configured"
        echo "!! by hand.  Reconfigure and rebuild it:"
        echo "!!   cmake -S . -B $build \\"
        echo "!!     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-m68k-amigaos.cmake \\"
        echo "!!     -DCMAKE_BUILD_TYPE=Release"
    } >&2
    exit 2
}
