#!/usr/bin/env bash
#
# The set of cross arms tools/ci.sh declares and the set the GitHub matrix runs
# must be the SAME SET, checked in BOTH directions.
#
#   declared but not run   cpu68060 and if2 were in CROSS_CONFIGS and in no
#                          workflow matrix, so two arms compiled only where
#                          someone typed them.  Allowlistable with a reason.
#   run but not declared   `microcompat` stayed in the matrix after
#                          CROSS_CONFIGS dropped it.  `ci.sh cross` was handed
#                          a name nothing declares, failed at configure in 35
#                          seconds, and reddened main -- with this gate
#                          reporting PASS, because it only ever walked the
#                          arms.  Not allowlistable: a matrix entry that names
#                          no arm cannot do anything but fail.
#
# SPDX-License-Identifier: MIT
set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT" || exit 2

CI="tools/ci.sh"
WF=".github/workflows/ci.yml"
[ -r "$CI" ] && [ -r "$WF" ] || { echo "ci_arm_coverage=FAIL missing $CI or $WF"; exit 2; }

# arm:reason -- an arm here is deliberately not in the matrix.
ALLOW="
cpu68060:known red on tests/tls pcrel branches, tracked in docs/BACKLOG.md
default:built by the analyze and tier1 jobs, not the options matrix
"

arms=$(sed -n '/^CROSS_CONFIGS=(/,/^)/p' "$CI" \
       | grep -oE '^[[:space:]]*"[a-z0-9_]+' | tr -d ' "' | sort -u)
matrix=$(sed -n '/^[[:space:]]*config:[[:space:]]*\[/,/\]/p' "$WF" \
         | tr -d ' \n' | sed 's/.*config:\[//; s/\].*//' | tr ',' '\n' | sort -u)

errors=0
for a in $arms; do
    printf '%s\n' "$matrix" | grep -qx "$a" && continue
    reason=$(printf '%s\n' "$ALLOW" | sed -n "s/^$a://p")
    if [ -n "$reason" ]; then
        echo "ci_arm_allowed=$a reason=$reason"
        continue
    fi
    echo "ci_arm_unrun=$a declared_in_CROSS_CONFIGS no_workflow_matrix_entry"
    errors=$((errors + 1))
done

# The other direction.  No allowlist: see the header.
for m in $matrix; do
    printf '%s\n' "$arms" | grep -qx "$m" && continue
    echo "ci_arm_undeclared=$m in_workflow_matrix not_in_CROSS_CONFIGS"
    errors=$((errors + 1))
done

# A stale allowlist is the same defect one level down.
while IFS= read -r e; do
    a=${e%%:*}
    [ -n "$a" ] || continue
    printf '%s\n' "$arms" | grep -qx "$a" || {
        echo "ci_arm_allowlist_stale=$a not_in_CROSS_CONFIGS"
        errors=$((errors + 1))
    }
    printf '%s\n' "$matrix" | grep -qx "$a" && {
        echo "ci_arm_allowlist_covered=$a it_is_in_the_matrix_now"
        errors=$((errors + 1))
    }
done <<< "$ALLOW"

echo "ci_arm_coverage_errors=$errors"
echo "ci_arms=$(printf '%s\n' "$arms" | wc -l) matrix=$(printf '%s\n' "$matrix" | wc -l)"
echo "ci_arm_coverage=$([ "$errors" -eq 0 ] && echo PASS || echo FAIL)"
exit $([ "$errors" -eq 0 ] && echo 0 || echo 1)
