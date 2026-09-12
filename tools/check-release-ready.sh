#!/usr/bin/env bash
# Prove that HEAD is eligible for the package-only release workflow.
#
#   tools/check-release-ready.sh --list
#   tools/check-release-ready.sh
#
# Compilation is deliberately absent: CI has already performed it for the
# exact SHA, and release.yml refuses any SHA without that successful verdict.
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

workflow=.github/workflows/release.yml
if ! grep -q 'tools/check-ci-success\.sh "\$GITHUB_SHA"' "$workflow" ||
   ! grep -q '^[[:space:]]*needs: verify' "$workflow"; then
    echo "check-release-ready=FAIL release publication is not gated by exact-SHA CI" >&2
    exit 1
fi

emulator=.github/workflows/emulator.yml
if ! grep -q 'name: release-candidate-${{ github\.sha }}' "$emulator" ||
   ! grep -q 'tools/check-release-candidate\.sh build/dist "\$GITHUB_SHA"' "$emulator"; then
    echo "check-release-ready=FAIL emulator E2E does not consume the CI candidate" >&2
    exit 1
fi

case "${1:-}" in
    --list)
        echo "release-gate: exact-sha successful CI"
        echo "ci-package-job: shipping profiles, clients, archive checks"
        echo "release-job: verify candidate identity, publish without rebuilding"
        exit 0
        ;;
    "") ;;
    *) echo "check-release-ready: unknown argument $1" >&2; exit 2 ;;
esac

sha=$(git rev-parse HEAD)
tools/check-ci-success.sh "$sha"
tools/check-shipping-config.sh
tools/check-changelog-prose.sh

echo "check-release-ready=PASS sha=$sha"
