#!/usr/bin/env bash
#
# WHAT A HARNESS MUST PROVE BEFORE IT BOOTS A GUEST.
#
# Every wrong verdict this tree has produced from a harness came from the same
# place: the harness's own environment, input or staleness answered the
# question the claim was supposed to ask.  An emulator run is minutes, so each
# of those cost a restart AND a reported finding that was not true.
#
# These are the checks that can be made mechanically.  The rest of the list
# (isolation, a negative control, reading the command's own output block) is
# per-claim and lives in the round that makes the claim.
#
# SPDX-License-Identifier: MIT

# pf_require_fresh <artifact> [srcdir...]
#
# The artifact under test must be newer than every source that goes into it.
# `-b build/cm` names a directory, not a commit: a harness pointed at a tree
# that was not rebuilt after the fix under test measures the OLD binary and
# reports the fix did not work.  That happened to run-ifslots.sh on
# 2026-09-11, against a change that was already correct.
pf_require_fresh() {
    local art="$1"; shift
    local dirs=("$@") newer

    [ ${#dirs[@]} -gt 0 ] || dirs=(src include port)

    if [ ! -f "$art" ]; then
        echo "!! $art does not exist.  Build it before running this." >&2
        return 2
    fi

    newer=$(find "${dirs[@]}" -type f \
                 \( -name '*.c' -o -name '*.h' -o -name '*.s' \) \
                 -newer "$art" -print -quit 2>/dev/null)

    [ -z "$newer" ] && return 0

    echo "!! $art is OLDER than $newer" >&2
    echo "!! The guest would run the previous build and the verdict would be" >&2
    echo "!! about that, not about the change under test.  Rebuild first." >&2
    echo "!! AMINETXDUO_STALE_OK=1 if you mean to test the old binary." >&2
    [ "${AMINETXDUO_STALE_OK:-0}" = 1 ] && {
        echo "!! AMINETXDUO_STALE_OK=1: continuing against the old binary." >&2
        return 0
    }
    return 2
}
