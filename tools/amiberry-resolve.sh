#!/usr/bin/env bash
#
# Resolve the Amiberry binary, and refuse anything that is not one.
#
#   . "$ROOT/tools/amiberry-resolve.sh"
#   amiberry_resolve          # sets AMIBERRY, or exits 2 saying why
#
# WHY THIS IS A FILE OF ITS OWN
#
#   $AMIBERRY is a PATH, read by tools/amiberry-run.sh and exported by
#   amiga-assets/env.sh.  tests/perf/run-fitzbench.sh used the same name for
#   its -a flag, `AMIBERRY=1`; assigning to an already-exported variable keeps
#   the export attribute, so the flag reached the child as a path of "1" and
#   every bridged run died on "amiberry not found" with the emulator installed,
#   working, and on disk where the script was about to look.  One owner of the
#   name is the fix; the flag is USE_AMIBERRY now, and the check below names
#   the trap if anything sets it that way again.
#
# CALL IT BEFORE DOING ANYTHING EXPENSIVE.  A run that resolves the emulator
# after staging a drive and starting a server on the peer reports a bad
# environment several minutes and one boot timeout later, which reads as a
# defect in the code under test.
#
# AND IT SAYS WHICH BINARY IT PICKED.  On 2026-08-11 a bring-up failure was
# reported as a regression in this stack and chased for hours; the cause was
# $AMIBERRY pointing at an instrumented build with a write_log() on every read
# of a polled register, left exported in one shell.  Every harness printed the
# board, the backend, the model and the ROM, and none of them printed the one
# thing that had changed.  A line naming the path, its size and its date is
# what tells a swapped emulator apart from a product defect, so it is printed
# on every resolve and not behind a verbose flag.
#
# SPDX-License-Identifier: MIT

# The path, where it came from, and enough of a fingerprint to tell two builds
# of it apart.  stat(1) differs between GNU and BSD; ls -l has the two fields
# on every system this runs on.
amiberry_describe() {
    local origin="$1"
    local detail

    detail=$(ls -lL "$AMIBERRY" 2>/dev/null | awk '{print $5 " bytes, " $6 " " $7 " " $8}')

    echo "==> amiberry: $AMIBERRY ($origin${detail:+, $detail})"
}

amiberry_resolve() {
    local origin="from \$AMIBERRY"
    case "${AMIBERRY:-}" in
        0|1|true|false|yes|no|on|off)
            echo "AMIBERRY=${AMIBERRY} is a flag, not a path." >&2
            echo "It is the PATH to the emulator binary, and a caller has" >&2
            echo "overwritten it with a boolean -- an exported variable keeps" >&2
            echo "its export attribute across a plain assignment, so the" >&2
            echo "value reaches this script.  The flag wants its own name." >&2
            return 2 ;;
    esac

    if [ -z "${AMIBERRY:-}" ]; then
        local candidate
        origin="found on PATH"
        for candidate in "$(command -v amiberry || true)" \
                         "$HOME/amiberry/build/amiberry"
        do
            [ -n "$candidate" ] && [ -x "$candidate" ] && {
                AMIBERRY="$candidate"; break; }
            origin="found at ~/amiberry/build"
        done
    fi

    [ -n "${AMIBERRY:-}" ] || {
        echo "amiberry not found.  Either build it at ~/amiberry/build/amiberry," >&2
        echo "put it on PATH, or set AMIBERRY=<path>.  On the lab machines," >&2
        echo ". ~/amiga-assets/env.sh sets it." >&2
        return 2
    }

    [ -x "$AMIBERRY" ] || {
        echo "AMIBERRY=$AMIBERRY is not an executable file." >&2
        return 2
    }

    export AMIBERRY
    amiberry_describe "$origin"
    return 0
}
