#!/usr/bin/env bash
# check-ci-success.sh against a fake gh: which run a release is cut from.
#
#   tools/check-ci-success-selftest.sh
#
# Prints check_ci_success_selftest=PASS|FAIL cases=N failures=M; exit 0/1.
# SPDX-License-Identifier: MIT

set -u

here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
sha=0123456789abcdef0123456789abcdef01234567

# The fake reads $work/runs.json (gh run list's answer) and
# $work/art-<id> (one artifact name per line) for gh api .../runs/<id>/artifacts.
mkdir -p "$work/bin"
cat > "$work/bin/gh" <<'GH'
#!/usr/bin/env bash
case "$1 $2" in
    "run list") cat "$FAKE/runs.json" ;;
    api*) id=$(printf '%s' "$2" | sed -n 's|.*/runs/\([0-9]*\)/artifacts|\1|p')
          if [ -f "$FAKE/art-$id" ]; then
              printf '{"artifacts":['
              sep=
              while read -r n e; do
                  case "${e:-false}" in
                      missing) printf '%s{"name":"%s"}' "$sep" "$n" ;;
                      *) printf '%s{"name":"%s","expired":%s}' "$sep" "$n" "${e:-false}" ;;
                  esac
                  sep=,
              done < "$FAKE/art-$id"
              printf ']}'
          else
              printf '{"artifacts":[]}'
          fi ;;
esac
GH
chmod +x "$work/bin/gh"

cases=0 failures=0
run_case() {                    # name  want-rc  want-text  runs-json  [id:artifact ...]
    local name=$1 want_rc=$2 want_text=$3 json=$4; shift 4
    rm -f "$work"/art-*
    printf '%s' "$json" > "$work/runs.json"
    for a in "$@"; do printf '%s\n' "${a#*:}" > "$work/art-${a%%:*}"; done
    local out rc
    out=$(FAKE="$work" PATH="$work/bin:$PATH" "$here/check-ci-success.sh" "$sha" 0 2>&1)
    rc=$?
    cases=$((cases + 1))
    if [ "$rc" != "$want_rc" ] ||
       { [ -n "$want_text" ] && ! printf '%s' "$out" | grep -q -- "$want_text"; }; then
        failures=$((failures + 1))
        echo "  FAIL $name: rc=$rc (want $want_rc) out=$out"
    fi
}

r() { printf '{"databaseId":%s,"headSha":"%s","status":"%s","conclusion":"%s","url":"u%s","event":"%s"}' \
        "$1" "$sha" "$2" "$3" "$1" "$4"; }
cand="release-candidate-$sha"

o() { printf '{"databaseId":%s,"headSha":"%s","status":"completed","conclusion":"success","url":"u%s","event":"%s"}' \
        "$1" "$2" "$1" "$3"; }
other=fedcba9876543210fedcba9876543210fedcba98

# A docs-only push, newer, without the candidate; the full dispatch has it.
run_case "docs-only push newer than the full dispatch" 0 "run=2 " \
    "[$(r 3 completed success push),$(r 2 completed success workflow_dispatch)]" \
    "3:build-logs" "2:$cand"
# The full push run is newer and has it.
run_case "full push newer than docs-only" 0 "run=5 " \
    "[$(r 5 completed success push),$(r 4 completed success push)]" \
    "5:$cand" "4:build-logs"
# The newest run failed: an older success with the candidate does not
# outvote it.
run_case "newest run failed" 1 "run=7 .*conclusion=failure" \
    "[$(r 7 completed failure workflow_dispatch),$(r 6 completed success push)]" \
    "7:$cand" "6:$cand"
# A newer run still going is waited for, not skipped (wait 0: FAIL).
run_case "newer run in progress" 1 "state=active" \
    "[$(r 11 in_progress '' workflow_dispatch),$(r 10 completed success push)]" \
    "10:$cand"
# Docs-only success, then a FAILED run, then an older candidate: the walk
# stops at the failure; it does not step over it.
run_case "docs-only then failed then candidate" 1 "run=14 .*conclusion=failure" \
    "[$(r 15 completed success push),$(r 14 completed failure workflow_dispatch),$(r 13 completed success push)]" \
    "15:build-logs" "13:$cand"
# Docs-only success, then a run STILL GOING, then an older candidate: wait.
run_case "docs-only then active then candidate" 1 "state=active" \
    "[$(r 18 completed success push),$(r 17 in_progress '' workflow_dispatch),$(r 16 completed success push)]" \
    "18:build-logs" "16:$cand"
# An expired candidate is not a candidate, nor one whose expiry is absent.
run_case "expired candidate" 1 "reason=no_successful_run_with_candidate" \
    "[$(r 12 completed success push)]" "12:$cand true"
run_case "candidate with no expired field" 1 "reason=no_successful_run_with_candidate" \
    "[$(r 19 completed success push)]" "19:$cand missing"
# No run uploaded it.
run_case "no candidate anywhere" 1 "reason=no_successful_run_with_candidate" \
    "[$(r 8 completed success push)]" "8:build-logs"
# Another commit's candidate under another name is not this one.
run_case "wrong artifact name" 1 "reason=no_successful_run_with_candidate" \
    "[$(r 20 completed success push)]" "20:release-candidate-$other"
# A run of another commit is never taken, even with this SHA's candidate.
run_case "wrong headSha" 1 "reason=no_successful_run_with_candidate" \
    "[$(o 21 "$other" push)]" "21:$cand"
# A pull_request run of the exact SHA is never taken.
run_case "pull_request event ignored" 1 "reason=no_successful_run_with_candidate" \
    "[$(o 9 "$sha" pull_request)]" "9:$cand"

if [ "$failures" = 0 ]; then
    echo "check_ci_success_selftest=PASS cases=$cases failures=0"
    exit 0
fi
echo "check_ci_success_selftest=FAIL cases=$cases failures=$failures"
exit 1
