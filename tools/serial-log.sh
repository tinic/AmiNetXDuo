#!/usr/bin/env bash
#
# THE GUEST'S SERIAL LOG: WHERE IT IS, WHETHER ANYTHING COULD BE IN IT, AND
# WHAT TO SAY WHEN THERE IS NOT.
#
#   . "$ROOT/tools/serial-log.sh"
#
#   serial_log_path [tag]                    the file tools/amiberry-run.sh wrote
#   serial_log_state <builddir>              on | off | unknown
#   serial_log_have  <file> <builddir> <what>   0 if <what> can be checked
#
# WHY THIS EXISTS
#
# Of twenty serial logs written in one session, thirteen came back 0 bytes, and
# every assertion reading one of those thirteen passed without looking at
# anything.  tests/tools/run-cycledrill.sh counted orphaned SANA-II reader
# stacks with `grep -c` over an empty file, got 0, and printed it as a result.
#
# The logs were empty because NOTHING WAS EVER WRITTEN TO THEM.  AMI_ERROR,
# AMI_WARN and AMI_INFO compile to `if (0)` unless AMINETXDUO_LOG is defined
# (include/aminetxduo/compat.h), and it is OFF by default (CMakeLists.txt).  A
# library built that way puts not one byte on the serial port, so the seven
# logs that did have content were the seven whose GUEST PROGRAM carries a
# RawPutChar tracer of its own -- tests/libraries, tests/concurrent,
# tests/netstack, tests/udpdrill.  The thirteen empty ones were the runs driven
# by ToolsSmoke, which writes to stdout, plus tests/leak and tests/stack, which
# do the same.  Measured on the same rig in the same minute: tests/stack
# against a -DAMINETXDUO_LOG=OFF build wrote 0 bytes and against
# -DAMINETXDUO_LOG=ON wrote 3,847, and the second carried every
# `netstack: mark` line tests/ipv6/run-bringup.sh reports.
#
# So a harness that reads guest ami_log() output has a BUILD REQUIREMENT, and
# until this file existed nothing checked it.  Two things follow, and both are
# here rather than in each harness:
#
#   * the state of the build is a fact about the build, readable from it
#   * an empty log is a RESULT OF ITS OWN and never a passing assertion
#
# THE PATH IS HERE TOO, because four harnesses had it wrong.
# run-livetools.sh, run-tcphandler.sh, run-routes.sh and
# tests/ipv6/run-tools-amiberry.sh each built `build/serial-<tag>.log`, which
# tools/amiberry-run.sh has never written -- it writes
# `build/amiberry-serial-<tag>.log`.  Every one of them guarded its serial
# assertions with `[ -s "$SERIAL" ]`, so a filename that never exists read as a
# clean skip on every run.  tools/enforcer-run.sh writes the other spelling and
# is the reason it looks plausible.
#
# SPDX-License-Identifier: MIT

# Callers set ROOT before sourcing this; every harness in the tree already does.

# The file tools/amiberry-run.sh wrote for this run.  The tag defaults the same
# way the runner defaults it, so a caller that exported AMINETXDUO_RUN_TAG need
# pass nothing.
serial_log_path() {
    echo "${ROOT}/build/amiberry-serial-${1:-${AMINETXDUO_RUN_TAG:-amiberry}}.log"
}

# Was the library in <builddir> built with the serial log compiled in?
#
# Echoes on, off or unknown, and returns 0, 1 or 2 to match.  CMakeCache.txt is
# the answer when there is one.  Failing that the LIBRARY ITSELF is asked: the
# format strings are only linked when the macros expand to a call, so a build
# with logging out has none of them.  Measured: 353,476 bytes without,
# 380,264 with, and `netstack: mark` present in exactly one of the two.  The
# binary probe is what covers a tree somebody staged by hand or configured
# through CMAKE_C_FLAGS rather than the option.
serial_log_state() {
    local build="$1" cache lib

    # A caller's -b is relative to the tree, and every harness cd's to it
    # before this is reached; an absolute one is left alone.
    case "$build" in
        /*) ;;
        *)  build="${ROOT:-.}/$build" ;;
    esac
    cache="$build/CMakeCache.txt"

    if [ -f "$cache" ]; then
        case "$(sed -n 's/^AMINETXDUO_LOG:BOOL=//p' "$cache" | head -1)" in
            [Oo][Nn]|1|[Tt][Rr][Uu][Ee]|[Yy][Ee][Ss]) echo on;  return 0 ;;
            [Oo][Ff][Ff]|0|[Ff][Aa][Ll][Ss][Ee]|[Nn][Oo]) echo off; return 1 ;;
        esac
    fi

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
# is a fact about the rig and has to be as loud as a red check.  The diagnosis
# on stderr names the rebuild, because on this rig the answer has been the
# build every time it has been looked at.
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
                echo "!! $build was built with AMINETXDUO_LOG=OFF, so AMI_ERROR,"
                echo "!! AMI_WARN and AMI_INFO compiled to nothing and the guest"
                echo "!! never wrote a byte to the serial port.  Rebuild it:"
                echo "!!   cmake -S . -B $build \\"
                echo "!!     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-m68k-amigaos.cmake \\"
                echo "!!     -DCMAKE_BUILD_TYPE=Release \\"
                echo "!!     -DAMINETXDUO_LOG=ON -DAMINETXDUO_LOG_LEVEL=2" ;;
            on)
                echo "!! ${build:-the build} HAS the serial log compiled in, so this is"
                echo "!! not the usual cause: the guest reached nothing that logs, or"
                echo "!! the run never started.  Read the emulator log beside this file." ;;
            *)
                echo "!! Nothing here can say whether ${build:-the build} has the serial"
                echo "!! log compiled in; there is no CMakeCache.txt and no library to"
                echo "!! read.  If it was built without -DAMINETXDUO_LOG=ON, that is the"
                echo "!! whole of it." ;;
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
        echo "!! $name reads guest ami_log() output, and $build was built with"
        echo "!! AMINETXDUO_LOG=OFF: AMI_ERROR, AMI_WARN and AMI_INFO compile to"
        echo "!! nothing, so the serial log comes back 0 bytes and every figure"
        echo "!! this harness reports would be missing.  Rebuild it:"
        echo "!!   cmake -S . -B $build \\"
        echo "!!     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-m68k-amigaos.cmake \\"
        echo "!!     -DCMAKE_BUILD_TYPE=Release \\"
        echo "!!     -DAMINETXDUO_LOG=ON -DAMINETXDUO_LOG_LEVEL=2"
    } >&2
    exit 2
}
