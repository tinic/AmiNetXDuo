#!/usr/bin/env bash
#
# WHAT A THIRD-PARTY AMIGA BINARY OPENS BEFORE IT REACHES main().
#
#   . tools/amiga-startup-libs.sh
#   stage_startup_libs <binary> <libsdir> [assets]
#
# A harness boots a BARE DIRECTORY hard drive.  It has no LIBS: beyond what
# the harness puts there, and a C library that opens locale.library or the
# maths pair at startup dies before any of our code runs:
#
#   locale.library failed to load
#   mathieeedoubbas.library failed to load
#   ----- rc 20, 100 ms -----
#
# That is not a stack defect and it is not even a network test; it is a
# missing file.  It has now cost three separate harnesses --
# tests/compare/run-tickprobe.sh staged the maths pair for Roadshow's
# commands, install/test/run-workbench.sh hit it, and tests/tools/run-binkd.sh
# hit it again -- each rediscovering it from the guest's own error message.
#
# So this does not carry a list.  It READS THE BINARY, because the binary is
# the only thing that knows: every library an AmigaOS program opens by name
# appears as a string in its hunks.  What the asset store has, it stages.
# What it cannot supply, it NAMES, so a harness can decide whether that is a
# skip or a fact about the program.
#
# ROM libraries are not staged and not reported: exec, dos, intuition,
# graphics, utility and the rest are in Kickstart, and a bare drive has them.
# Ours are not staged either -- a harness stages the bsdsocket.library and
# usergroup.library it is testing, and having this copy a different one over
# the top is precisely the accident worth avoiding.
#
# SPDX-License-Identifier: MIT

# In Kickstart, or supplied by the harness itself.  Anything here is silence.
_ASL_ROM='exec|dos|intuition|graphics|utility|layers|mathffp|mathieeesingbas|icon|expansion|keymap|gadtools|workbench|asl|commodities|iffparse|datatypes|diskfont|input|timer|console|rexxsyslib'
_ASL_OURS='bsdsocket|usergroup|tls|anxnet'

# stage_startup_libs <binary> <libsdir> [assets]
# Echoes one line per library it could not supply:  missing <name>.library
# Returns 0 whether or not anything was missing; the caller decides.
stage_startup_libs() {
    _bin="$1"
    _libs="$2"
    _assets="${3:-${AMINETXDUO_ASSETS:-$HOME/amiga-assets}}"

    [ -f "$_bin" ] || return 0
    mkdir -p "$_libs"

    strings -a "$_bin" 2>/dev/null |
        grep -oE '[a-z0-9_]+\.library' |
        sort -u |
    while read -r _want; do
        _stem=${_want%.library}

        # A hunk string table runs names together, so a name can arrive with
        # the tail of the one before it stuck to the front: `ulocale.library'
        # for locale, `ubsdsocket.library' for bsdsocket.  Try the whole name
        # first, then each shorter suffix, and take the first one that is
        # either known or actually on disk.  Without this the real dependency
        # is missed AND a phantom one is reported.
        _resolved=""
        _try="$_stem"
        while [ -n "$_try" ]; do
            for _known in $(printf '%s %s' "$_ASL_ROM" "$_ASL_OURS" | tr '|' ' '); do
                [ "$_try" = "$_known" ] && { _resolved=SKIP; break; }
            done
            [ -n "$_resolved" ] && break
            if [ -f "$_assets/libs/$_try.library" ] ||
               [ -f "$_assets/nglibs/$_try.library" ]; then
                _resolved="$_try"
                break
            fi
            _try=${_try#?}
        done

        case "$_resolved" in
            SKIP) continue ;;
            "")   # Nothing on disk matches any suffix.  Report the longest
                  # plausible name rather than a one-letter tail.
                  echo "missing $_stem.library"
                  continue ;;
        esac

        if [ -f "$_assets/libs/$_resolved.library" ]; then
            cp "$_assets/libs/$_resolved.library" "$_libs/$_resolved.library"
        else
            cp "$_assets/nglibs/$_resolved.library" "$_libs/$_resolved.library"
        fi
    done
}
