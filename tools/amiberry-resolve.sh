#!/usr/bin/env bash
#
# Resolve the Amiberry binary, and refuse anything that is not one.
#
#   . "$ROOT/tools/amiberry-resolve.sh"
#   amiberry_resolve          # sets AMIBERRY, or exits 2 saying why
#
# $AMIBERRY is a PATH and has ONE owner; do not reuse the name for a flag
# (tests/perf/run-fitzbench.sh's is USE_AMIBERRY for that reason).
#
# CALL IT BEFORE DOING ANYTHING EXPENSIVE, so a bad environment is not reported
# as a defect several minutes and one boot timeout later.  It prints the path it
# picked, unconditionally: that is what tells a swapped emulator from a defect.
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
