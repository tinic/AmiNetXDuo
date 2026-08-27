#!/usr/bin/env bash
#
# Every on-Amiga harness has a declared home, and the declaration is true.
#
#   tools/check-harnesses.sh
#
# tests/HARNESSES says, for each run-*.sh under tests/ and install/test/, what
# invokes it.  This checks the file against the tree and against the runners in
# both directions: a harness with no row, a row whose file is gone, a row naming
# a runner that does not invoke it, a `manual` row for something a runner does
# invoke, a `manual` row with no reason, and a chain that does not end at a
# runner.
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

declare -A ROW_RUNNER ROW_NOTE ROW_SCHED
rows=0
while IFS= read -r line; do
    case "$line" in ''|'#'*) continue ;; esac
    case "$line" in *:*:*) ;; *) err "malformed_row:${line%% *}"; continue ;; esac

    path=$(printf '%s' "$line" | cut -d: -f1 | tr -d '[:space:]')
    runner=$(printf '%s' "$line" | cut -d: -f2 | sed 's/^ *//; s/ *$//')
    note=$(printf '%s' "$line" | cut -d: -f3- | sed 's/^ *//; s/ *$//')

    # A wired runner carries its SCHEDULE: `<runner>@<when>`.  See WHEN A
    # WIRED ROW ACTUALLY RUNS below for why the runner alone is not the claim.
    sched=""
    case "$runner" in *@*) sched="${runner##*@}"; runner="${runner%@*}" ;; esac

    if [ -n "${ROW_RUNNER[$path]:-}" ]; then err "duplicate_row:$path"; continue; fi
    ROW_RUNNER[$path]="$runner"
    ROW_SCHED[$path]="$sched"
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
        [ -z "${ROW_SCHED[$path]}" ] ||
            err "manual_with_a_schedule:$path->${ROW_SCHED[$path]}"
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
# THE HOLE THE ROWS ABOVE CANNOT SEE: a test BINARY with no harness at all has
# no row to go stale, so it is invisible from inside tests/HARNESSES.
#
# THE RULE, which needs no list of what is a test: a target registered with
# add_test() is a host test and ctest runs it; every OTHER add_executable()
# under tests/ is a guest binary, and something has to name it -- a run-*.sh,
# tools/ci.sh, a workflow, or a row below saying it is an instrument rather
# than a gate.  Resolved through OUTPUT_NAME, because that is what the
# harnesses spell.  An exemption that something DOES run is an error, so the
# list cannot go stale in the quiet direction either.

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
# Rows written against SLIRP.  Bridged is the only mode this project uses, so
# a 10.0.2.x literal in the guest's command list, its staged configuration or
# its assertions is a rewrite and not a missing home; counting those as
# UNWIRED said four harnesses were waiting for a runner when what they are
# waiting for is a gateway discovered at run time.
say harnesses_slirp "$(grep -c ': manual : SLIRP' "$MANIFEST" || true)"
# Rows that DO run and DO assert and are failing on a product finding.  They
# are not holes and counting them as UNWIRED hid seven of them behind one
# number; they are also not coverage, so they get a line of their own.
say harnesses_red "$(grep -c ': manual : RED' "$MANIFEST" || true)"

# ------------------------------------------------- every SELFTEST too --
#
# THE SAME HOLE, ONE CLASS OVER, and it is the class with no manifest in
# front of it.  tools/web/console-selftest.mjs decodes with the real planar
# modules and compares every pixel against a reference built the other way
# round -- 111 checks -- and it was named by no stage at all, so it had
# never gone red in CI.  A harness at least has a row here to go stale; a
# selftest has nothing, and `tests/*/*-verdict-selftest.sh` in tools/ci.sh
# looks like a rule that covers them all until you notice where it does not
# reach.
#
# TWO WAYS TO BE RUN, and both are checked:
#
#   the glob in tools/ci.sh, which is `tests/*/*-verdict-selftest.sh` and
#     SKIPS A FILE THAT IS NOT EXECUTABLE (`[ -x "$st" ] || continue`), so a
#     chmod is enough to take a grader out of CI in silence.  A file that
#     matches the pattern and is not +x is reported.
#   a literal path in a runner or in a harness, the way the rest of this
#     script asks the question.
#
# ci.yml is not a runner here for the reason it is not one above: every
# selftest path in it is an argument to shellcheck.
SELFTEST_GLOB='tests/*/*-verdict-selftest.sh'

# Not a gate, with the reason.  Held both directions: an exemption something
# does run is an error.
SELFTEST_EXEMPT="
tools/profiler/selftest.sh  proves the profiler end to end and needs an emulator, a ROM and a build configured with AMINETXDUO_PROFILER=ON; the profiler is an instrument and profverify is its gate
"
declare -A SELFTEST_WHY
while read -r t why; do
    [ -n "$t" ] || continue
    SELFTEST_WHY[$t]="$why"
done <<< "$SELFTEST_EXEMPT"

selftests=0 selftests_unrun=0 selftests_exempt=0
while IFS= read -r st; do
    selftests=$((selftests + 1))

    named=""
    # shellcheck disable=SC2254
    case "$st" in
    $SELFTEST_GLOB)
        if invokes tools/ci.sh "$SELFTEST_GLOB"; then
            if [ -x "$st" ]; then
                named="tools/ci.sh (the glob)"
            else
                err "selftest_not_executable_so_the_glob_skips_it:$st"
            fi
        fi ;;
    esac

    if [ -z "$named" ]; then
        # THIS SCRIPT IS NOT A RUNNER, and it took itself for one: the
        # exemption table above names tools/profiler/selftest.sh, which is a
        # mention, so the exempt row reported itself as run.
        while IFS= read -r f; do
            case "$f" in "$st"|"${BASH_SOURCE[0]#./}"|tools/check-harnesses.sh)
                continue ;; esac
            invokes "$f" "$st" && { named="$f"; break; }
        done < <( { find tests install/test -name 'run-*.sh'
                    find tools -name '*.sh'
                    printf '%s\n' "${RUNNERS[@]}"; } | sed 's|^\./||' | sort -u)
    fi

    if [ -n "${SELFTEST_WHY[$st]+set}" ]; then
        selftests_exempt=$((selftests_exempt + 1))
        [ -n "$named" ] && err "selftest_exempt_but_$named-runs_it:$st"
        continue
    fi

    [ -n "$named" ] && continue
    err "no_runner_for_selftest:$st"
    selftests_unrun=$((selftests_unrun + 1))
done < <(find tools tests install -name '*selftest*' \
              ! -name '*.log' ! -name '*.txt' | sed 's|^\./||' | sort)

say selftests "$selftests"
say selftests_exempt "$selftests_exempt"
say selftests_unrun "$selftests_unrun"

# ------------------------------------- WHEN A WIRED ROW ACTUALLY RUNS --------
#
# THE GAP THE ROWS ABOVE CANNOT SEE.  Everything so far asks whether a runner
# INVOKES the harness.  Whether that runner ever RUNS is a different question,
# and "wired" was one word for three unlike claims: fifteen rows named
# .github/workflows/emulator.yml, which is dispatch, a nightly cron and tags,
# so nothing in it fires on a pull request; twelve more named tools/ci.sh in a
# stage no workflow passes to it at all, which is a person typing a command.
# Both read as covered beside a row that runs on every push.
#
# So the schedule is part of the row now: `<runner>@<when>`, and it is
# COMPUTED here and compared, the same both-directions honesty the `manual`
# rows are held to.  A row cannot claim a schedule it does not have, and a row
# whose runner starts firing on pushes cannot keep calling itself nightly.
#
#   push      a workflow that fires on a branch push or a pull request
#   nightly   a workflow with a `schedule:`
#   release   a workflow that fires on a tag and nothing else
#   hand      no workflow runs it.  A person types it.  Legal -- tools/emurun.sh
#             and tools/demo.sh are entry points, not schedulers -- and it is
#             the number to watch, because it is the one that looks like
#             coverage from a distance and is not.
#
# NOT a value judgement about the schedule.  Tier 2 boots an emulator and takes
# 7-20 minutes; nightly is the right answer for it and turning that into a
# build failure would only teach people to stop reading this script.  What it
# must not do is stay invisible.

# The strongest trigger a workflow has.  A workflow with both a cron and a tag
# push is nightly: the nightly one is what fires without anybody doing
# anything.
wf_class() { # workflow -> push|nightly|release|none
    local f="$1"
    [ -f "$f" ] || { printf none; return; }
    if grep -qE '^[[:space:]]*(pull_request|branches):' "$f"; then
        printf push
    elif grep -qE '^[[:space:]]*schedule:' "$f"; then
        printf nightly
    elif grep -qE '^[[:space:]]*tags:' "$f"; then
        printf release
    else
        printf none
    fi
}

rank() { case "$1" in push) printf 4 ;; nightly) printf 3 ;; release) printf 2 ;;
                      hand) printf 1 ;; *) printf 0 ;; esac; }
stronger() { [ "$(rank "$1")" -ge "$(rank "$2")" ] && printf '%s' "$1" ||
             printf '%s' "$2"; }

# EVERY workflow, globbed rather than listed: a new one that runs a stage on
# every push is a schedule this has to see, and a hand-kept list would report
# the old answer for a year.
WORKFLOWS=()
while IFS= read -r w; do WORKFLOWS+=("$w"); done < <(
    find .github/workflows -name '*.yml' -o -name '*.yaml' | sort)
[ "${#WORKFLOWS[@]}" -gt 0 ] || err "no_workflows_in_.github/workflows"

# Which tools/ci.sh STAGES a workflow names.  A row saying `tools/ci.sh` is
# only as scheduled as the stage its harness sits in: `bridged` is in
# emulator.yml, `matrix` was in nothing.
declare -A WF_STAGES WF_CLASS
for w in "${WORKFLOWS[@]}"; do
    WF_CLASS[$w]=$(wf_class "$w")
    WF_STAGES[$w]=$(grep -v '^[[:space:]]*#' "$w" 2>/dev/null |
                    sed -n 's|.*tools/ci\.sh||p' | tr -d "'\"" | tr ' ' '\n' |
                    grep -E '^[a-z][a-z0-9]*$' | sort -u | tr '\n' ' ')
done

# Which stage function of tools/ci.sh runs each harness.
#
# A MESSAGE IS NOT AN INVOCATION, and both are in there: stage_conformance's
# failure text names tests/conformance/run-conformance.sh across four
# backslash-continued lines to say what cannot start, which would have put
# that harness in a stage ci.yml runs on every push.  The continuation is
# tracked, because the second and third lines of that string start with the
# path and look like commands.
declare -A PATH_STAGES
while IFS=$'\t' read -r fn hp; do
    case " ${PATH_STAGES[$hp]:-} " in *" $fn "*) continue ;; esac
    PATH_STAGES[$hp]="${PATH_STAGES[$hp]:-}$fn "
done < <(awk '
    { l = $0; sub(/^[[:space:]]*/, "", l) }
    substr(l, 1, 1) == "#" { next }
    /^[a-z0-9_]+\(\)[[:space:]]*\{/ { fn = $0; sub(/\(\).*/, "", fn) }
    /^\}/ { fn = "" }
    {
        if (!incont)
            msg = (l ~ /^(fail|note|skip|echo|printf|say|hr)[[:space:]]/)
        incont = ($0 ~ /\\[[:space:]]*$/)
    }
    msg { next }
    fn ~ /^stage_/ {
        s = $0
        while (match(s, /(tests|install)\/[a-z0-9\/_-]*run-[a-z0-9-]+\.sh/)) {
            print substr(fn, 7) "\t" substr(s, RSTART, RLENGTH)
            s = substr(s, RSTART + RLENGTH)
        }
    }
' tools/ci.sh)

computed_class() { # harness path, runner -> push|nightly|release|hand|none
    local path="$1" runner="$2" best=none w s
    case "$runner" in
    .github/workflows/*.yml)
        printf '%s' "${WF_CLASS[$runner]:-$(wf_class "$runner")}"
        return ;;
    tools/ci.sh)
        for s in ${PATH_STAGES[$path]:-}; do
            for w in "${WORKFLOWS[@]}"; do
                case " ${WF_STAGES[$w]} " in
                    *" $s "*) best=$(stronger "$best" "${WF_CLASS[$w]}") ;;
                esac
            done
        done ;;
    *)
        for w in "${WORKFLOWS[@]}"; do
            invokes "$w" "$runner" &&
                best=$(stronger "$best" "${WF_CLASS[$w]}")
        done ;;
    esac
    [ "$best" = none ] && best=hand
    printf '%s' "$best"
}

declare -A SCHED_COUNT
for k in push nightly release hand; do SCHED_COUNT[$k]=0; done

for path in "${!ROW_RUNNER[@]}"; do
    runner="${ROW_RUNNER[$path]}"
    [ "$runner" = manual ] && continue
    [ -f "$path" ] || continue

    # A chained row runs exactly when the harness that calls it runs.
    resolved="$runner"
    case "$runner" in
        chained:*) resolved="${ROW_RUNNER[${runner#chained:}]:-manual}"
                   [ "$resolved" = manual ] && continue
                   path_for_stage="${runner#chained:}" ;;
        *)         path_for_stage="$path" ;;
    esac

    got=$(computed_class "$path_for_stage" "$resolved")
    want="${ROW_SCHED[$path]}"

    if [ -z "$want" ]; then
        err "wired_row_with_no_schedule:$path(is:$got)"
        continue
    fi
    case "$want" in
        push|nightly|release|hand) ;;
        *) err "unknown_schedule:$path->$want"; continue ;;
    esac
    if [ "$want" != "$got" ]; then
        err "schedule_is_wrong:$path->declared_$want,actually_$got"
        continue
    fi
    SCHED_COUNT[$got]=$(( ${SCHED_COUNT[$got]} + 1 ))
    [ "$got" = hand ] && printf 'harness_hand=%s->%s\n' "$path" "$runner"
done

say harnesses_on_push    "${SCHED_COUNT[push]}"
say harnesses_nightly    "${SCHED_COUNT[nightly]}"
say harnesses_on_release "${SCHED_COUNT[release]}"
say harnesses_by_hand    "${SCHED_COUNT[hand]}"

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
