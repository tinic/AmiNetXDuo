#!/bin/sh
#
# WHEN DID EACH STAGE LAST PRODUCE A VERDICT?
#
# This tree already checks, thoroughly, that a gate is WIRED: check-gates-wired
# asserts the call sites exist, check-ci-arm-coverage requires every cross arm
# to be run by a workflow and caught two that were "compiled only where someone
# typed them", check-option-coverage compiles both states of every option,
# check-harnesses makes tests/HARNESSES true against the tree,
# check-stage-coverage requires a workflow to name every stage and re-checks
# its own allowlist for staleness, and cmake/HostTests.cmake derives the build
# targets from the registrations so a test cannot be "Not Run".  Seven
# mechanisms, all aimed at declared-but-not-executed.
#
# THEY ALL VERIFY WIRING, AND WIRING IS NOT A RUN.  Every one of them is green
# on emulator.yml, which names sixteen of the twenty-six stages and has 0
# successes in its last 20 runs -- 16 cancelled, 1 failure.  Correctly wired,
# perfectly documented, and silent for weeks.  That is the one question none of
# the others ask, and it is the outermost ring: not "is this stage called" but
# "did the thing that calls it finish".
#
# What lived in that silence: `-p minimal` installed the FULL stack for as long
# as the option existed, because e2e and e2ecards are the only stages that
# exercise it.  Against that, `sanitize` is tier 1 and hosted, and it caught a
# bad sprintf the same day it was pushed.  The difference is a runner.
#
# Needs `gh` and network.  Without either it exits 0 with SKIPPED on its own
# line -- and says so, rather than printing a verdict it did not earn.
#
# SPDX-License-Identifier: MIT

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 2

MAX_AGE_DAYS="${AMINETXDUO_STAGE_MAX_AGE_DAYS:-14}"
WINDOW="${AMINETXDUO_STAGE_WINDOW:-20}"

# STAGES KNOWN TO HAVE NO RUNNER, and the reason.  Same contract as
# AMINETXDUO_WARNING_EXEMPT in cmake/ci-warnings.cmake, including the lesson
# written there on 2026-09-08: an entry that is no longer true is WORSE than
# one that is, because nothing is watching the hole it leaves.  So this list is
# checked in BOTH directions -- an unlisted dead stage fails, and a listed
# stage that has started passing fails too, naming itself for removal.
#
# All sixteen are emulator.yml, the tier-2 workflow, whose self-hosted runner
# (playhouse3) is not connected to Actions.  docs/BACKLOG.md carries the row.
# They are not "allowed to be broken": they are counted, in the repo, where a
# release has to look at them.
BASELINE_DEAD="${AMINETXDUO_STAGE_BASELINE- bridged capture cards cards6 e2e e2ecards emulator fetchtls
lossgate ltoprobe matrix rate reachability smb tlsloop wirequiet }"

if ! command -v gh > /dev/null 2>&1; then
    echo "stage_freshness=SKIPPED reason=no_gh_cli"
    exit 0
fi

# A GATE THAT CANNOT SEE MUST FAIL.  Run from the wrong directory this used to
# find no ci.sh, iterate zero stages and print PASS -- a vacuous green, which
# is the shape this whole file exists to complain about.  Caught by trying to
# prove the gate fails and getting PASS from a copy in /tmp.
if [ ! -r tools/ci.sh ]; then
    echo "check_stage_freshness=FAIL cannot read tools/ci.sh from $(pwd)"
    exit 2
fi

stages=$(grep -oE '^stage_[a-z0-9_]+' tools/ci.sh | sed 's/^stage_//' | sort -u)

if [ -z "$stages" ]; then
    echo "check_stage_freshness=FAIL no stages found in tools/ci.sh"
    exit 2
fi

# Which workflow names each stage, flattened the same way
# check-stage-coverage.sh flattens it.
wf_for_stage() {
    for w in .github/workflows/*.yml; do
        named=$(tr '\n' ' ' < "$w" | tr -s ' ' \
                | grep -oE 'ci\.sh( +[a-z0-9_]+)+' | sed 's/ci\.sh//' \
                | tr ' ' '\n' | grep -v '^$' | sort -u)
        for n in $named; do
            [ "$n" = "$1" ] && { basename "$w"; return 0; }
        done
    done
    return 1
}

never=0
dead=0
known=0
revived=0
stale=0
fresh=0
unnamed=0

for s in $stages; do
    w=$(wf_for_stage "$s") || {
        echo "stage_freshness stage=$s workflow=none verdict=NOT_NAMED"
        unnamed=$((unnamed + 1))
        continue
    }

    # THE RECENT WINDOW, NOT MERELY THE NEWEST SUCCESS.  The first version of
    # this file asked `gh run list --status success --limit 1` and compared its
    # age, which certified emulator.yml as OK on an 18-day-old success while
    # its last 20 runs held zero -- 16 cancelled, 1 failure.  A workflow that
    # succeeded once and has not since is exactly what this gate exists to
    # catch, so asking only for the last success reproduced the defect it was
    # written against.
    wins=$(gh run list --workflow "$w" --limit "$WINDOW" \
             --json conclusion -q '[.[]|select(.conclusion=="success")]|length' \
             2>/dev/null)
    [ -n "$wins" ] || wins=0

    last=$(gh run list --workflow "$w" --status success --limit 1 \
             --json updatedAt -q '.[0].updatedAt' 2>/dev/null)

    if [ -z "$last" ] || [ "$last" = "null" ]; then
        echo "stage_freshness stage=$s workflow=$w verdict=NEVER"
        never=$((never + 1))
        continue
    fi

    if [ "$wins" -eq 0 ]; then
        if echo "$BASELINE_DEAD" | tr -s ' \n' ' ' | grep -q " $s "; then
            echo "stage_freshness stage=$s workflow=$w wins_in_last_$WINDOW=0 verdict=DEAD_KNOWN"
            known=$((known + 1))
        else
            echo "stage_freshness stage=$s workflow=$w wins_in_last_$WINDOW=0 verdict=DEAD_NEW"
            dead=$((dead + 1))
        fi
        continue
    fi

    if echo "$BASELINE_DEAD" | tr -s ' \n' ' ' | grep -q " $s "; then
        echo "stage_freshness stage=$s workflow=$w verdict=REVIVED_REMOVE_FROM_BASELINE"
        revived=$((revived + 1))
        continue
    fi

    age=$(python3 - "$last" <<'PY'
import sys, datetime
t = datetime.datetime.fromisoformat(sys.argv[1].replace('Z', '+00:00'))
now = datetime.datetime.now(datetime.timezone.utc)
print(int((now - t).total_seconds() // 86400))
PY
)
    if [ "$age" -gt "$MAX_AGE_DAYS" ]; then
        echo "stage_freshness stage=$s workflow=$w age_days=$age verdict=STALE"
        stale=$((stale + 1))
    else
        echo "stage_freshness stage=$s workflow=$w age_days=$age verdict=OK"
        fresh=$((fresh + 1))
    fi
done

echo "stage_freshness_total fresh=$fresh dead_known=$known dead_new=$dead revived=$revived stale=$stale never=$never not_named=$unnamed window=$WINDOW"

if [ "$never" -gt 0 ] || [ "$stale" -gt 0 ] || [ "$dead" -gt 0 ] || [ "$revived" -gt 0 ]; then
    echo "check_stage_freshness=FAIL"
    echo "  a stage whose workflow has not succeeded in its last $WINDOW runs is"
    echo "  not a passing stage, however long ago it last worked."
    echo "  Either give it a runner, or stop counting it as coverage."
    exit 1
fi

echo "check_stage_freshness=PASS"
