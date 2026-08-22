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

# ------------------------------------------- every guest TEST BINARY too --
#
# THE HOLE THE ROWS ABOVE CANNOT SEE.  Everything above is about run-*.sh: a
# harness with no row fails, a row naming a deleted file fails.  A test
# BINARY with no harness at all has no row to go stale, so it is invisible
# from inside tests/HARNESSES -- and four were in exactly that state on
# 2026-08-20: tests/atf, and three programs under tests/crypto68k, and
# tests/ipv6/ipv6_test.c.  Every one had been compiled by every cross
# configuration since it was written and executed by nothing, and every one
# carried an amiberry-run.sh command line in its CMakeLists comment.  A
# command line in a comment is not a gate, and crypto68k_25519_test proved
# what that is worth: it had never reached its first check on a guest, so
# nobody had seen it die on a missing mathieeedoubbas.library.
#
# THE RULE, AND WHY IT NEEDS NO LIST OF WHAT IS A TEST.  A target registered
# with add_test() is a host test and ctest runs it.  Every OTHER
# add_executable() under tests/ is a guest binary, and something has to name
# it: a run-*.sh, tools/ci.sh, a workflow, or a row below saying it is an
# instrument rather than a gate.  Resolved through OUTPUT_NAME, because that
# is what the harnesses spell.
#
# The exemptions are held to the same rule the `manual` rows are: one that
# something DOES run is an error, so the list cannot go stale in the quiet
# direction either.

# Instruments, not gates.  A benchmark, a probe or a calibration has no pass
# to go red, and putting one in a runner buys a number nobody reads.
INSTRUMENTS="
bracket_test        prices the ThreadX/Exec bracket; tests/bracket says whether it is correct
cpucal              what the emulator charges for an instruction, which every other number here rests on
crypto68k_bench     a reference RSA-2048 private operation, minutes of it
crypto68k_bulk      AES and SHA-256 instruction cost, 68020 only
crypto68k_ec_bench  P-256 against reference elliptic curve operations
crypto68k_amissl    ours against AmiSSL; needs an SDK this tree does not vendor
n68kmv              times every multiversioned inner loop against the others
perf_test           where a megabyte of TCP goes, per primitive
profverify          whether the SAMPLER reports the PC it thinks it does; it verifies the instrument, not the stack
rfbil               RFB interleave measurement
rfbprof             RFB encode profile
tcpprof             the sampling profiler itself
tls_bench           handshake and record timings
test_ptrprobe       where an injected IECLASS_POINTERPOS lands, per display mode; a measured table
test_ifnames        a Developer drawer example, staged by dist/make-dist.sh
test_v6only         a Developer drawer example, staged by dist/make-dist.sh
"

declare -A INSTRUMENT_WHY
while read -r t why; do
    [ -n "$t" ] || continue
    INSTRUMENT_WHY[$t]="$why"
done <<< "$INSTRUMENTS"

cmakes=$(find tests -name CMakeLists.txt | sort)

# Targets ctest runs: the word after COMMAND, and anything named through
# $<TARGET_FILE:>, which is how a test names a helper it is not itself.
#
# The word after COMMAND ANYWHERE, not `add_test(NAME ... COMMAND x` on one
# line: half of tests/fuzz puts COMMAND on the next line, and reading only the
# first form reported eight fuzz drivers as run by nothing while ctest was
# running each of them twice.  Over-approximating here only suppresses a
# report, so a stray COMMAND costs nothing; missing one costs a false alarm,
# which is the failure this whole script exists to avoid.
hosttests=$( { awk '{ for (i = 1; i < NF; i++) if ($i == "COMMAND") print $(i + 1) }' \
                   $cmakes
               grep -ho '\$<TARGET_FILE:[A-Za-z0-9_.+-]*>' $cmakes |
                   sed 's/.*://; s/>//'; } | tr -d ')' | sort -u)

binaries=0 unrun=0 instruments=0
while read -r target; do
    case "$target" in ''|'${'*) continue ;; esac
    printf '%s\n' "$hosttests" | grep -qx "$target" && continue

    binaries=$((binaries + 1))

    out=$(grep -h "set_target_properties($target PROPERTIES OUTPUT_NAME" $cmakes |
              sed 's/.*OUTPUT_NAME "//; s/").*//' | head -1)
    [ -n "$out" ] || out="$target"

    named=""
    while IFS= read -r f; do
        invokes "$f" "$out" && { named="$f"; break; }
    done < <(find tests install/test -name 'run-*.sh'; printf '%s\n' "${RUNNERS[@]}")

    if [ -n "${INSTRUMENT_WHY[$target]+set}" ]; then
        instruments=$((instruments + 1))
        # The same direction the `manual` rows are held to: an instrument
        # something runs is not an instrument, it is a gate with a stale
        # exemption in front of it.
        [ -n "$named" ] &&
            err "instrument_but_$named-runs_it:$target"
        continue
    fi

    [ -n "$named" ] && continue
    err "no_runner_for_guest_test:$target(${out})"
    unrun=$((unrun + 1))
done < <(grep -h 'add_executable' $cmakes |
         sed 's/.*add_executable(//' | awk '{print $1}' | tr -d '()' | sort -u)

say guest_binaries "$binaries"
say guest_instruments "$instruments"
say guest_unrun "$unrun"
say harnesses_manual "$manual"
say harnesses_unwired "$(grep -c ': manual : UNWIRED' "$MANIFEST" || true)"
# Rows that DO run and DO assert and are failing on a product finding.  They
# are not holes and counting them as UNWIRED hid seven of them behind one
# number; they are also not coverage, so they get a line of their own.
say harnesses_red "$(grep -c ': manual : RED' "$MANIFEST" || true)"

# --------------------------------------------- a runner that does not fire --
#
# THE GAP THE ROWS ABOVE CANNOT SEE.  Everything so far asks whether a runner
# INVOKES the harness.  Whether that runner ever RUNS is a separate question
# and no row records it: fifteen rows name .github/workflows/emulator.yml,
# which is `workflow_dispatch`, a nightly `schedule` and `push: tags`, so
# nothing in it fires on a pull request or a push to a branch.  That is a
# deliberate choice -- the tier takes 7-20 minutes and would be cancelled by
# the next push -- but it means "wired" there and "wired" to tools/ci.sh are
# not the same claim, and a reader of this manifest cannot tell them apart.
#
# REPORTED, NOT FAILED.  The row is correct; it is the runner's schedule that
# is the fact, and turning a deliberate schedule into a build failure would
# only teach people to stop reading this script.  What it must not do is stay
# invisible.
ondemand=0
for path in "${!ROW_RUNNER[@]}"; do
    runner="${ROW_RUNNER[$path]}"
    case "$runner" in .github/workflows/*.yml) ;; *) continue ;; esac
    [ -f "$runner" ] || continue
    # A workflow that runs on a branch push or a pull request fires on the
    # work; one with only dispatch, cron and tags does not.  `push:` with
    # nothing but `tags:` under it is the second kind, which is why the test
    # is for `branches:` and `pull_request:` rather than for `push:`.
    if grep -qE '^[[:space:]]*(pull_request|branches):' "$runner"; then
        continue
    fi
    printf 'harness_runner_ondemand=%s->%s\n' "$path" "$runner"
    ondemand=$((ondemand + 1))
done
say harnesses_ondemand "$ondemand"

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
