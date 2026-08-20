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
# tools/lint-guest-arch.sh is NOT one: it names harnesses to exempt them from
# a lint, which is a mention and not an invocation, and counting it as a
# runner reported install/test/run-all.sh as covered.
RUNNERS=(.github/workflows/emulator.yml .github/workflows/release.yml
         tools/ci.sh tools/emurun.sh tools/tlsgate.sh tools/demo.sh)

bad=0
say()  { printf '%s=%s\n' "$1" "$2"; }
err()  { printf 'harness_error=%s\n' "$*"; bad=$((bad + 1)); }

[ -f "$MANIFEST" ] || { err "no_$MANIFEST"; exit 1; }

# Does FILE invoke PATH?  A non-comment line mentioning it.  Both YAML and
# shell comment with '#', and every false positive found while writing this
# was prose in a comment block ("the same reason run-httpd.sh is not here").
#
# NO PIPE INTO grep -q.  It was `grep -v '^#' "$file" | grep -qF -- "$path"`,
# and `set -o pipefail` is on: the second grep exits the moment it matches, the
# first takes SIGPIPE and returns 141, and the pipeline is then a failure even
# though the path was found.  It is a RACE -- it depends on how much of the
# file is still to be written when the match lands -- so it passed on one
# machine and reported run-mdnsctl.sh and run-onoff.sh as uninvoked on
# another, which is this script's loudest error and was untrue.  A check that
# says "no runner invokes this" at random is worse than no check.
invokes() {
    local file="$1" path="$2" body
    [ -f "$file" ] || return 1
    body=$(grep -v '^[[:space:]]*#' "$file") || return 1
    case "$body" in *"$path"*) return 0 ;; esac
    return 1
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

# --------------------------------------------------- references that dangle --
#
# A harness that runs a script which is not there is not a harness, and the
# manifest cannot see it.  install/test/run-all.sh was the case that motivated
# this check: it called install/test/run-installer-fsuae.sh once per scenario
# after that file was deleted with the rest of the fs-uae harnesses, and with
# `set -uo pipefail` and no -e all five scored 127 while it exited 5.  Nothing
# invoked it, so nothing ever saw that.  It has since been repaired onto
# run-workbench.sh; the check stays, because the next one will not announce
# itself either.
# An INVOCATION is separated from a MENTION, because both exist and they are
# not the same defect.  A mention is a stale sentence in an error message,
# worth fixing and not worth failing a build over; an invocation is a script
# that cannot run.
tmp=$(mktemp)
while IFS= read -r f; do
    [ -f "$f" ] || continue
    while IFS= read -r line; do
        case "${line#"${line%%[![:space:]]*}"}" in '#'*) continue ;; esac
        printf '%s\n' "$line" |
        grep -oE '(\$(HERE|ROOT|SELFDIR)/)?((tests|install)/[a-z0-9/_-]*)?run-[a-z0-9-]+\.sh' |
        while IFS= read -r ref; do
            case "$ref" in
                '$HERE/'*)  target="$(dirname "$f")/${ref#\$HERE/}" ;;
                '$ROOT/'*)  target="${ref#\$ROOT/}" ;;
                tests/*|install/*) target="$ref" ;;
                *)          target="$(dirname "$f")/$ref" ;;
            esac
            [ -f "$target" ] && continue
            # Executed, as opposed to printed: the reference opens a command,
            # or follows one of the words that introduce one.
            case "$line" in
                *'echo '*|*'printf '*|*'>&2'*)
                    printf 'dangling_mention=%s->%s\n' "$f" "$target" ;;
                *"exec $ref"*|*"bash $ref"*|*"sh $ref"*|*"\"$ref\""*|*"if $ref"*|\
                *"RUNNER=$ref"*|*"= $ref"*)
                    printf 'dangling_invocation=%s->%s\n' "$f" "$target" ;;
                *)  printf 'dangling_invocation=%s->%s\n' "$f" "$target" ;;
            esac
        done
    done < "$f"
done < <(find tests install/test -name 'run-*.sh'
         printf '%s\n' "${RUNNERS[@]}") | sort -u > "$tmp"
cat "$tmp"
dangle=$(grep -c '^dangling_invocation=' "$tmp" || true)
say dangling_invocations "$dangle"
say dangling_mentions "$(grep -c '^dangling_mention=' "$tmp" || true)"
bad=$((bad + dangle))
rm -f "$tmp"

say harness_errors "$bad"
if [ "$bad" -eq 0 ]; then say check_harnesses PASS; exit 0; fi
say check_harnesses FAIL
exit 1
