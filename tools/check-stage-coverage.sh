#!/usr/bin/env bash
#
# Every stage tools/ci.sh declares must be INVOKED by a workflow, called by
# another stage, or allowlisted with the reason.
#
# tools/check-ci-arm-coverage.sh asserts this one level down, for the cross
# arms inside CROSS_CONFIGS.  Nothing asserted it for the stages themselves,
# and stage_rate is what that cost.  It is the only gate in the tree that
# measures a byte per second, it was written after 0.26.0 and 0.26.1 shipped a
# receive path running at a quarter rate, and NO WORKFLOW HAS EVER CALLED IT --
# not ci.yml, not emulator.yml, not release.yml.  A regression then shipped in
# 0.26.3 (92bff6b3, a Forbid()/Permit() around a transmit slot handback) worth
# 5.5% of transmit and about 2% of receive, measured against 242be840 on
# playhouse3.  A gate must be proven to RUN, not to exist.
#
# SPDX-License-Identifier: MIT
set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT" || exit 2

CI="tools/ci.sh"
[ -r "$CI" ] || { echo "stage_coverage=FAIL missing $CI"; exit 2; }

# stage:reason -- a stage here is deliberately invoked by no workflow.
ALLOW="
console:tier 2, needs a second host with python3 -- NOT a display, that half was wrong: only the RTG arm wants one and run-console.sh starts its own Xvfb, the other four groups run SDL_VIDEODRIVER=dummy. IT HAS NOW BEEN RUN, 2026-09-09: run-console.sh -c playhouse2 -C ham6 -m A1200 on playhouse3 gave CONSOLE_RC=0 RESULT=PASS arms_run=1 ham6_pixels_mismatched=0, so the reason this sits here is no longer 'unproven' -- it is that no workflow calls it. Wiring it into emulator.yml removes this entry; docs/BACKLOG.md
submodules:preamble, called unconditionally by ci.sh itself
toolchain:preamble, called by ci.sh when AMIGA_TOOLCHAIN_ROOT is unset
"

stages=$(grep -oE '^stage_[a-z0-9_]+' "$CI" | sed 's/^stage_//' | sort -u)

# Workflows fold run: blocks over several lines, so flatten before reading the
# words after a tools/ci.sh call.
invoked=$(cat .github/workflows/*.yml 2>/dev/null \
          | tr '\n' ' ' | tr -s ' ' \
          | grep -oE 'ci\.sh( +[a-z0-9_]+)+' | sed 's/ci\.sh//' \
          | tr ' ' '\n' | grep -v '^$' | sort -u)

# WHICH FILE invokes a stage, because "a workflow invokes it" is not the same
# as "it runs".  emulator.yml's runner (playhouse3) has been offline for a
# week: every run of that tier since 2026-08-30 went queued -> cancelled at the
# 24h timeout, so every stage wired there -- rate, bridged, lossgate, cards,
# console, e2e, smb, fetchtls -- is invoked by a workflow that never executes.
# This gate cannot see that from the tree, so it prints the file and leaves the
# reader to know which tiers are live.
#
# THE TERMINATOR IS NOT A SPACE.  emulator.yml runs the loss gate as
# `'"'"'tests/endurance/fetch-fitz.sh && tools/ci.sh lossgate'"'"'` -- a quoted
# compound -- so the stage name is followed by a quote and `( |$)` did not
# match it.  lossgate read `by=?` while being invoked on line 588 of the very
# file this was searching, and the count of stages wired into the dead tier
# came out one short.
where() {
    local st="$1" f
    for f in .github/workflows/*.yml; do
        tr '\n' ' ' < "$f" | tr -s ' ' | grep -qE "ci\.sh( +[a-z0-9_]+)* +$st([^a-z0-9_]|\$)" \
            && { basename "$f"; return; }
    done
    echo "?"
}

errors=0
for s in $stages; do
    if printf '%s\n' "$invoked" | grep -qx "$s"; then
        echo "stage_invoked=$s by=$(where "$s")"
        continue
    fi

    reason=$(printf '%s\n' "$ALLOW" | sed -n "s/^$s://p")
    if [ -n "$reason" ]; then
        echo "stage_allowed=$s reason=$reason"
        continue
    fi

    echo "stage_unrun=$s declared_in_ci.sh invoked_by_no_workflow"
    errors=$((errors + 1))
done

# A stale allowlist is the same defect one level down.
while IFS= read -r e; do
    s=${e%%:*}
    [ -n "$s" ] || continue
    printf '%s\n' "$stages" | grep -qx "$s" || {
        echo "stage_allowlist_stale=$s not_a_stage_in_ci.sh"
        errors=$((errors + 1))
    }
    printf '%s\n' "$invoked" | grep -qx "$s" && {
        echo "stage_allowlist_covered=$s a_workflow_calls_it_now"
        errors=$((errors + 1))
    }
done <<< "$ALLOW"

echo "stage_coverage_errors=$errors"
echo "stages=$(printf '%s\n' "$stages" | wc -l)\
 allowlisted=$(printf '%s\n' "$ALLOW" | grep -c ':')"
echo "stage_coverage=$([ "$errors" -eq 0 ] && echo PASS || echo FAIL)"
exit $([ "$errors" -eq 0 ] && echo 0 || echo 1)
