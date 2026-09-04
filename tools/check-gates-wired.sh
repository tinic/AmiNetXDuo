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
want rate           tools/ci.sh                      'check-rate\.sh'
want diag-strings   tools/ci.sh                      'check-no-diag-strings\.sh'
want backlog        tools/ci.sh                      'check-backlog\.sh'
want doc-budget     tools/ci.sh                      'check-doc-budget\.sh'

# ------------------------------------------ and the gate scripts still run ---
for g in check-changelog-prose check-image-size check-rate check-gates-wired; do
    if [ ! -x "tools/$g.sh" ]; then
        echo "gates_wired=NOT_EXECUTABLE gate=$g"
        rc=1
    fi
done

[ "$rc" = 0 ] && echo "gates_wired=PASS sites=$ok"
exit "$rc"
