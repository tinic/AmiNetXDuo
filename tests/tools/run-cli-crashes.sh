#!/usr/bin/env bash
# Shipped CLI option/crash matrix, no hardware or installed-system writes.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"
exec python3 tests/tools/cli/run.py "$@"
