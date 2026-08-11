#!/usr/bin/env bash
#
# Every on-Amiga harness has a declared home, and the declaration is true.
#
#   tools/check-harnesses.sh
#
# tests/HARNESSES says, for each run-*.sh under tests/ and install/test/, what
# invokes it.  This checks the file against the tree and against the runners,
# in both directions:
#
#   * a harness with no row              a test nothing invokes, unrecorded
#   * a row whose file is gone           a stale row
#   * a row naming a runner that does
#     not invoke it                      a claim of coverage that is not there
#   * a row saying `manual` for
#     something a runner does invoke     a row that has gone stale quietly
#   * a `manual` row with no reason      a hole recorded as if it were a choice
#   * a chain that does not end at a
#     runner                             a harness invoked only by other
#                                        harnesses nothing invokes
#
# The third and fourth are the ones worth having.  tests/tools/httpd-drill.py
# held twelve WebDAV assertions nothing called; run-addifup.sh's verdict
# matched ShowNetStatus's column header and could not go red.  Neither was
# visible from inside the file, and both are the same shape: something that
# reads as coverage.
#
# Output is key=value plus an exit code.  Needs no toolchain and no emulator.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT" || exit 1

MANIFEST=tests/HARNESSES

# What executes things.  ci.yml is deliberately absent: the only harness paths
# in it are arguments to shellcheck, which does not run them, and counting
# those as coverage is the mistake this script exists to catch.
RUNNERS=(.github/workflows/emulator.yml .github/workflows/release.yml
         tools/ci.sh tools/lint-guest-arch.sh tools/emurun.sh
         tools/tlsgate.sh tools/demo.sh)

bad=0
say()  { printf '%s=%s\n' "$1" "$2"; }
err()  { printf 'harness_error=%s\n' "$*"; bad=$((bad + 1)); }

[ -f "$MANIFEST" ] || { err "no_$MANIFEST"; exit 1; }

# Does FILE invoke PATH?  A non-comment line mentioning it.  Both YAML and
# shell comment with '#', and every false positive found while writing this
# was prose in a comment block ("the same reason run-httpd.sh is not here").
invokes() {
    local file="$1" path="$2"
    [ -f "$file" ] || return 1
    grep -v '^[[:space:]]*#' "$file" | grep -qF -- "$path"
}

# ---------------------------------------------------------- read the rows --

declare -A ROW_RUNNER ROW_NOTE
rows=0
while IFS= read -r line; do
    case "$line" in ''|'#'*) continue ;; esac
    case "$line" in *:*:*) ;; *) err "malformed_row:${line%% *}"; continue ;; esac

    path=$(printf '%s' "$line" | cut -d: -f1 | tr -d '[:space:]')
    runner=$(printf '%s' "$line" | cut -d: -f2 | sed 's/^ *//; s/ *$//')
    note=$(printf '%s' "$line" | cut -d: -f3- | sed 's/^ *//; s/ *$//')

    if [ -n "${ROW_RUNNER[$path]:-}" ]; then err "duplicate_row:$path"; continue; fi
    ROW_RUNNER[$path]="$runner"
    ROW_NOTE[$path]="$note"
    rows=$((rows + 1))
done < "$MANIFEST"
say harness_rows "$rows"

# ------------------------------------------- every harness in the tree ... --

found=0 unrecorded=0
while IFS= read -r h; do
    found=$((found + 1))
    if [ -z "${ROW_RUNNER[$h]:-}" ]; then
        err "no_row_in_$MANIFEST:$h"
        unrecorded=$((unrecorded + 1))
    fi
done < <(find tests install/test -name 'run-*.sh' | sed 's|^\./||' | sort)
say harnesses_found "$found"
say harnesses_unrecorded "$unrecorded"

# ------------------------------------------- ... and every row in the tree --

manual=0 wired=0
for path in "${!ROW_RUNNER[@]}"; do
    runner="${ROW_RUNNER[$path]}"
    note="${ROW_NOTE[$path]}"

    [ -f "$path" ] || { err "row_for_missing_file:$path"; continue; }

    case "$runner" in
    manual)
        manual=$((manual + 1))
        [ -n "$note" ] || err "manual_with_no_reason:$path"
        # The direction that goes stale silently: somebody wires it up and the
        # manifest still calls it a hole, so the next reader closes a hole
        # that is already closed and the one beside it stays open.
        for r in "${RUNNERS[@]}"; do
            if invokes "$r" "$path"; then
                err "manual_but_$r-invokes_it:$path"
            fi
        done
        ;;
    chained:*)
        parent="${runner#chained:}"
        wired=$((wired + 1))
        [ -f "$parent" ] || { err "chain_parent_missing:$path->$parent"; continue; }
        invokes "$parent" "$path" || err "chain_parent_does_not_invoke:$path->$parent"
        [ -n "${ROW_RUNNER[$parent]:-}" ] || err "chain_parent_has_no_row:$parent"
        [ "${ROW_RUNNER[$parent]:-manual}" = "manual" ] &&
            err "chain_ends_in_manual:$path->$parent"
        ;;
    *)
        wired=$((wired + 1))
        [ -f "$runner" ] || { err "row_names_missing_runner:$path->$runner"; continue; }
        invokes "$runner" "$path" ||
            err "claimed_runner_does_not_invoke_it:$path->$runner"
        ;;
    esac
done
say harnesses_wired "$wired"
say harnesses_manual "$manual"
say harnesses_unwired "$(grep -c ': manual : UNWIRED' "$MANIFEST" || true)"

say harness_errors "$bad"
if [ "$bad" -eq 0 ]; then say check_harnesses PASS; exit 0; fi
say check_harnesses FAIL
exit 1
