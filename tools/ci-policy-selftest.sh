#!/usr/bin/env bash
# Real Git ranges and fake GitHub answers: selective coverage fails closed.
# SPDX-License-Identifier: MIT
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$root/build/ci-policy-tests"
work=$(mktemp -d "$root/build/ci-policy-tests/case.XXXXXX")
trap 'rm -rf "$work"' EXIT
repo="$work/repo"
git init -q "$repo"
git -C "$repo" config user.name CI-fixture
git -C "$repo" config user.email ci-fixture@example.invalid
git -C "$repo" commit -q --allow-empty -m base
base=$(git -C "$repo" rev-parse HEAD)
cases=0 failures=0
expect() {
    cases=$((cases + 1))
    if ! printf '%s\n' "$3" | grep -qx "$2"; then
        echo "FAIL $1: expected $2, got $3"
        failures=$((failures + 1))
    fi
}
plan() {
    GIT_DIR="$repo/.git" GIT_WORK_TREE="$repo" \
        "$root/tools/ci-plan.sh" "$1" "$2" "${3:-pull_request}"
}
change() {
    git -C "$repo" checkout -q --detach "$base"
    for path in "$@"; do
        mkdir -p "$repo/$(dirname "$path")"
        printf 'fixture\n' > "$repo/$path"
    done
    git -C "$repo" add -A
    git -C "$repo" commit -q -m fixture
}
for path in src/tools/netsetup.c tests/sana2/host/new.c src/tools/web/client/main.ts docs/aminet-survey/own-tools.tsv; do
    change "$path"
    expect "$path" mode=shipping "$(plan "$base" HEAD)"
done
for path in src/netstack/new.c src/bsdsocket/new.c src/netdev/new.c \
    src/tools/shared.h tests/sana2/host/shared.h src/tools/web/shared.h src/tools/web/CMakeLists.txt \
    third_party/netxduo .github/workflows/ci.yml tools/ci.sh CMakeLists.txt \
    cmake/options.cmake unknown.input; do
    change "$path"
    expect "$path" mode=full "$(plan "$base" HEAD)"
done
change README.md
expect prose mode=docs "$(plan "$base" HEAD)"
expect manual mode=full "$(plan "$base" HEAD workflow_dispatch)"
expect weekly mode=full "$(plan "$base" HEAD schedule)"
expect unknown-base mode=full "$(plan bad-ref HEAD)"
expect empty-base mode=full "$(plan '' HEAD)"
expect zero-base mode=full "$(plan 0000000000000000000000000000000000000000 HEAD)"
expect unchanged mode=docs "$(plan HEAD HEAD)"
change README.md src/tools/netsetup.c src/netstack/new.c
expect mixed mode=full "$(plan "$base" HEAD)"
# Deletions and renames must include old paths, not just the prose destination.
old=$(git -C "$repo" rev-parse HEAD)
git -C "$repo" mv src/netstack/new.c README-renamed.md
git -C "$repo" commit -q -m rename
expect rename mode=full "$(plan "$old" HEAD)"
old=$(git -C "$repo" rev-parse HEAD)
git -C "$repo" rm -q src/tools/netsetup.c
git -C "$repo" commit -q -m delete
expect delete mode=shipping "$(plan "$old" HEAD)"

mkdir "$work/bin"
cat > "$work/bin/gh" <<'GH'
#!/usr/bin/env bash
[ "${FAKE_ERROR:-0}" = 0 ] || exit 1
case "$1 $2" in
    'run list') printf '%s\n' "$FAKE_RUNS" ;;
    api*) printf '%s\n' "$FAKE_ARTIFACTS" ;;
    *) exit 1 ;;
esac
GH
chmod +x "$work/bin/gh"
change src/netstack/new.c
source=$(git -C "$repo" rev-parse HEAD)
tree=$(git -C "$repo" rev-parse 'HEAD^{tree}')
merge=$(printf 'merge\n' | git -C "$repo" commit-tree "$tree" -p "$base" -p "$source")
export FAKE_ERROR=0
export FAKE_RUNS="[{\"databaseId\":42,\"headSha\":\"$source\",\"status\":\"completed\",\"conclusion\":\"success\",\"event\":\"pull_request\"}]"
export FAKE_ARTIFACTS="{\"artifacts\":[{\"name\":\"ci-options-tree-$tree\",\"expired\":false}]}"
reuse() {
    GIT_DIR="$repo/.git" GIT_WORK_TREE="$repo" PATH="$work/bin:$PATH" \
        "$root/tools/ci-matrix-reuse.sh" "$1" "$2"
}
expect identical-tree options=reused "$(reuse "$base" "$merge")"
expect reported-run reuse_run=42 "$(reuse "$base" "$merge")"
expect wrong-base options=build "$(reuse "$source" "$merge")"
expect linear-commit options=build "$(reuse "$base" "$source")"
different=$(printf 'different\n' | git -C "$repo" commit-tree "$base^{tree}" -p "$base" -p "$source")
expect changed-tree options=build "$(reuse "$base" "$different")"
FAKE_ERROR=1
expect unavailable-api options=build "$(reuse "$base" "$merge")"
FAKE_ERROR=0
saved_runs=$FAKE_RUNS saved_artifacts=$FAKE_ARTIFACTS
for answer in '[]' '{}' 'not-json' \
    "[{\"databaseId\":42,\"headSha\":\"$source\",\"status\":\"in_progress\",\"conclusion\":null,\"event\":\"pull_request\"},${saved_runs#\[}" \
    "[{\"databaseId\":43,\"headSha\":\"$source\",\"status\":\"completed\",\"conclusion\":\"failure\",\"event\":\"pull_request\"},${saved_runs#\[}"; do
    FAKE_RUNS=$answer
    expect missing-bad-pending-failed-run options=build "$(reuse "$base" "$merge")"
done
FAKE_RUNS=$saved_runs
for answer in '{"artifacts":[]}' 'not-json' \
    "{\"artifacts\":[{\"name\":\"ci-options-tree-$tree\",\"expired\":true}]}" \
    '{"artifacts":[{"name":"ci-options-tree-wrong","expired":false}]}'; do
    FAKE_ARTIFACTS=$answer
    expect missing-bad-expired-wrong-proof options=build "$(reuse "$base" "$merge")"
done
FAKE_ARTIFACTS=$saved_artifacts

# The sentinel must fail closed if a future planner requests builds but
# accidentally returns an empty matrix.
no_build=$(awk '
    /- name: No optional builds selected$/ { found=1 }
    found && /^        run: \|$/ { body=1; next }
    body && /^      - uses: actions\/checkout@v4$/ { exit }
    body { sub(/^          /, ""); print }
' "$root/.github/workflows/ci.yml")
[ -n "$no_build" ]
export GITHUB_STEP_SUMMARY="$work/option-summary"
OPTION_PLAN=skipped bash -euo pipefail -c "$no_build"
cases=$((cases + 1))
if OPTION_PLAN=build bash -euo pipefail -c "$no_build" > "$work/no-build.log" 2>&1; then
    echo "FAIL empty-build-matrix accepted"
    failures=$((failures + 1))
fi
cases=$((cases + 1))

# Execute the workflow's actual stable gate, not a separate imitation.
report=$(awk '
    /- name: Report$/ { found=1 }
    found && /^        run: \|$/ { body=1; next }
    body && /^      # Only/ { exit }
    body { sub(/^          /, ""); print }
' "$root/.github/workflows/ci.yml")
[ -n "$report" ]
export MODE=full PLAN=success GITLINKS=success ANALYZE=success OPTIONS=success
export OPTION_PLAN=build REUSE_RUN='' STACKFRAMES=success TIER1=success
export HOST_CLANG=success SHELLCHECK=success PACKAGE=success DOCS=skipped
gate() {
    cases=$((cases + 1))
    rc=0
    bash -euo pipefail -c "$report" > "$work/report.log" 2>&1 || rc=$?
    if [ "$rc" != "$2" ]; then
        echo "FAIL gate $1: rc=$rc expected=$2"
        cat "$work/report.log"
        failures=$((failures + 1))
    fi
}
gate full 0
OPTIONS=skipped
gate missing-matrix 1
OPTIONS=success
OPTION_PLAN=reused REUSE_RUN=42
gate reused 0
REUSE_RUN=
gate missing-reuse-id 1
REUSE_RUN=abc
gate invalid-reuse-id 1
MODE=shipping OPTION_PLAN=skipped REUSE_RUN=
gate shipping 0
OPTIONS=skipped
gate missing-shipping-verdict 1
OPTIONS=success
for field in PLAN GITLINKS ANALYZE STACKFRAMES TIER1 HOST_CLANG SHELLCHECK PACKAGE; do
    for status in skipped failure cancelled; do
        export "$field=$status"
        gate "$field-$status" 1
    done
    export "$field=success"
done
OPTION_PLAN=build
gate unexpected-matrix 1
OPTION_PLAN=skipped
MODE=unknown
gate unknown-mode 1
MODE=docs PLAN=success GITLINKS=skipped ANALYZE=skipped OPTIONS=success
STACKFRAMES=skipped TIER1=skipped HOST_CLANG=skipped SHELLCHECK=skipped PACKAGE=skipped DOCS=success
gate docs 0
DOCS=failure
gate failed-docs 1
echo "ci_policy_selftest cases=$cases failures=$failures"
[ "$failures" = 0 ]
