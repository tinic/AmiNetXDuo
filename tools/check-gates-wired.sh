#!/usr/bin/env bash
#
# The gates are still wired in, and the prose gate is wired in three times.
#
#   tools/check-gates-wired.sh
#
# A gate that can be quietly unwired is not a gate. This asserts the call
# sites exist, so deleting one turns CI red instead of turning the rule off.
#
# The prose gate specifically is checked in three independent places, because
# each one alone has a way past it:
#
#   .githooks/pre-commit          local; `--no-verify` skips it
#   tools/ci.sh stage_host        CI; only runs if someone pushes
#   .github/workflows/release.yml the publish itself; the last word
#
# SPDX-License-Identifier: MIT

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

rc=0
ok=0

# The pattern must match an INVOCATION and not a mention. A gate whose call is
# deleted usually leaves its name behind in the comment above it and in the
# failure message below it, and an earlier version of this script was satisfied
# by exactly that.
want() {           # want <label> <file> <script-basename>
    if [ -r "$2" ] &&
       grep -qE "(^|[;&|[:space:]\(])\"?(\\\$ROOT/|\\\$\{ROOT\}/|\./)?tools/$3(\"|[[:space:]]|\||>|$)" "$2"; then
        ok=$((ok + 1))
    else
        echo "gates_wired=MISSING gate=$1 file=$2"
        echo "  expected to find: $3"
        rc=1
    fi
}

# --------------------------------------------------- the prose gate, x3 ---
want prose-hook     .githooks/pre-commit             'check-changelog-prose\.sh'
want doc-only-hook  .githooks/pre-commit             'check-doc-only\.sh'
want entry-hook     .githooks/pre-commit             'check-changelog-entry\.sh'
want prose-ci       tools/ci.sh                      'check-changelog-prose\.sh'
want prose-release  .github/workflows/release.yml    'check-changelog-prose\.sh'

# The hook is only reachable when git is told where hooks live. A clone that
# has not run tools/install-hooks.sh has two gates, not three, and should be
# told so rather than believing it has all three.
hooks_path=$(git config --get core.hooksPath 2>/dev/null || true)
if [ "$hooks_path" = ".githooks" ]; then
    ok=$((ok + 1))
else
    echo "gates_wired=HOOKS_NOT_INSTALLED core.hooksPath='${hooks_path:-unset}'"
    echo "  Run tools/install-hooks.sh. CI and the release job still gate this,"
    echo "  so this is a warning about THIS clone and not a failure."
fi

# ------------------------------------------------------- the other gates ---
want image-size     tools/ci.sh                      'check-image-size\.sh'
want ram-size       tools/ci.sh                      'check-ram-size\.sh'
want rate           tools/ci.sh                      'check-rate\.sh'
want diag-strings   tools/ci.sh                      'check-no-diag-strings\.sh'
want backlog        tools/ci.sh                      'check-backlog\.sh'
want doc-budget     tools/ci.sh                      'check-doc-budget\.sh'
# stage-coverage is the gate that caught stage_rate being declared and invoked
# by no workflow, which is how the 0.26.3 transmit regression shipped.
# It protects the other gates' call sites at the stage level, so its own call
# site is worth protecting here.
want stage-coverage tools/ci.sh                      'check-stage-coverage\.sh'
want rx-posted      tools/ci.sh                      'check-rx-posted\.sh'
want option-stubs   tools/ci.sh                      'check-option-stubs\.sh'
want lvo-matrix     tools/ci.sh                      'check-lvo-matrix\.sh'
want generated      .githooks/pre-commit             'check-generated\.sh'

# Gate 5: the push itself.  .githooks/pre-push refuses a tree the host stage
# has not passed on, and stage_host writes the stamp it reads.  Both halves
# are named here because either one alone is silently off: a hook with no
# stamp writer refuses everything, and a stamp writer with no hook is a file
# nothing reads.
# want() names tools/<script>, and neither half of this is a script call, so
# they are asserted directly.
for _pp in ".githooks/pre-push:host-stage.ok" "tools/ci.sh:host-stage.ok" \
           ".githooks/pre-push:tools/tree-stamp.sh" \
           "tools/ci.sh:tools/tree-stamp.sh"; do
    _f="${_pp%%:*}"; _pat="${_pp#*:}"
    if [ -r "$_f" ] && grep -qF "$_pat" "$_f"; then
        ok=$((ok + 1))
    else
        echo "gates_wired=MISSING gate=push-stamp file=$_f"
        echo "  expected to find: $_pat"
        rc=1
    fi
done

# THE RECEIVE-PATH GATES WERE PROTECTED BY NOTHING UNTIL 2026-09-08.  All
# three guard properties no test and no A/B can see, which is exactly the kind
# that goes quiet without anyone noticing:
#   hot-calls        a per-frame helper stopped being inlined
#   rearm-invariants the device started writing a field the re-arm hoisted out
#   hotpath-budget   a per-frame receive function grew, at ~0.029% of
#                    receive an instruction -- under what the rig can measure
want hot-calls      tools/ci.sh                      'check-hot-calls\.sh'
want rearm-invar    tools/ci.sh                      'check-rearm-invariants\.sh'
want hotpath-budget tools/ci.sh                      'check-hotpath-budget\.sh'

# No test runs longer than ten seconds.  Wired into FOUR ctest arms, and the
# reason it is named here rather than left to the blanket rule below is that
# it went in wired to two of them -- and the slowest test in the tree, at
# 16.15 s, was in one of the other two.
want duration-ci    tools/ci.sh                      'check-test-duration\.sh'

# ------------------------------------------- and NO gate is unwired at all ---
#
# The `want' lines above name the gates that had a known way of being quietly
# unwired.  This says the same thing about every OTHER tools/check-*.sh, and
# about the next one somebody adds: a gate nothing calls is a gate that is
# off, and a gate added but never wired looks exactly like a gate that works.
# tools/check-test-duration.sh went in with its call sites in two of the four
# ctest arms, and the slowest test in the tree was in one of the other two.
#
# A call site is a mention of the basename anywhere that RUNS things: a shell
# script, a workflow, a hook, or a CMakeLists that registers it as a test.
#
# TWO THINGS DO NOT COUNT, and both of them made an earlier version of this
# pass for a gate whose only call site had just been deleted:
#
#   this file          naming a gate here is how it is REGISTERED, not run
#   the shellcheck     naming a script in `shellcheck a.sh b.sh ...' is how it
#   argument list      is LINTED, and every gate is in that list
#   the gate itself    "Usage: check-test-duration.sh <log>" in its own header
#                      satisfied this rule with every call site deleted
#
corpus="$(mktemp)"
trap 'rm -f "$corpus"' EXIT

# COMMENT LINES ARE DROPPED, and that is not tidiness.  Shell, CMake and YAML
# all comment with #, and the prose ABOUT a gate names its path: "the budget
# tools/check-test-duration.sh holds" reads like a call site to any pattern
# that is not looking at whether the line is code.  Two such sentences, both
# written in the same hour as the gate, were enough to make this rule pass
# with every real call site deleted.
#
# The shellcheck command is one invocation with backslash continuations, so
# it is dropped from its first line to the first that does not continue:
# naming a script there is how it is LINTED, and every gate is in that list.
while IFS= read -r f; do
    case "$f" in
        .github/workflows/*)
            awk '
                /^[[:space:]]*#/ { next }
                /(^|[[:space:]])shellcheck([[:space:]]|$)/ { skip = 1 }
                skip { if ($0 !~ /\\[[:space:]]*$/) skip = 0; next }
                { print }
            ' "$f" ;;
        *)  awk '/^[[:space:]]*#/ { next } { print }' "$f" ;;
    esac
done < <(git ls-files '*.sh' '*.yml' '*.yaml' '*CMakeLists.txt' '*.cmake' \
                      '.githooks/*' |
         grep -vE '^third_party/|^tools/check-') > "$corpus"

for g in tools/check-*.sh; do
    b="$(basename "$g")"
    [ "$b" = "check-gates-wired.sh" ] && continue
    # The corpus holds no tools/check-*.sh at all, so a gate cannot satisfy
    # this with its own usage line.  One gate calling ANOTHER is a real call
    # site, so the other gates are searched separately, with this one left out.
    # -F and the whole basename: a substring of another gate's name would
    # otherwise let a shorter one ride on a longer one.
    # One gate calling ANOTHER is a real call site, so the other gates are
    # searched too -- without this one, without this file (naming a gate here
    # is how it is registered), and without their comments, for the reason
    # the corpus drops comments above.
    other=""
    for o in tools/check-*.sh; do
        case "$o" in
            "tools/$b"|tools/check-gates-wired.sh) continue ;;
        esac
        if awk '/^[[:space:]]*#/ { next } { print }' "$o" | grep -qF "$b"; then
            other="$o"; break
        fi
    done

    if grep -qF "$b" "$corpus" || [ -n "$other" ]; then
        ok=$((ok + 1))
    else
        echo "gates_wired=UNWIRED gate=$b"
        echo "  nothing runs it: no shell script, workflow, hook or"
        echo "  CMakeLists names it outside the shellcheck list.  Wire it,"
        echo "  or delete it."
        rc=1
    fi
done

# ------------------------------------------ and the gate scripts still run ---
for g in check-changelog-prose check-image-size check-ram-size check-rate \
         check-stage-coverage check-rx-posted check-option-stubs \
         check-lvo-matrix check-generated \
         check-hot-calls check-rearm-invariants check-hotpath-budget \
         check-gates-wired; do
    if [ ! -x "tools/$g.sh" ]; then
        echo "gates_wired=NOT_EXECUTABLE gate=$g"
        rc=1
    fi
done

[ "$rc" = 0 ] && echo "gates_wired=PASS sites=$ok"
exit "$rc"
