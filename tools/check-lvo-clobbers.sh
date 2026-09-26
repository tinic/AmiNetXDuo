#!/usr/bin/env bash
#
# Every inline-asm library call names all four scratch registers (#70).
#
#   tools/check-lvo-clobbers.sh [FILE...]
#
# An LVO may destroy d0, d1, a0 and a1; an asm statement that lists one of
# them only as an input lets GCC reuse a value the call has overwritten.  The
# scan is tools/check-lvo-clobbers.py; this is its CI entry point.
#
# Output: `lvo_clobbers=fail file=... line=... missing=...` per site, then
# `lvo_clobbers=ok|fail statements=N files=N flagged=N`.  Exit 1 on any site.
#
# SPDX-License-Identifier: MIT

set -eu
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec python3 "$ROOT/tools/check-lvo-clobbers.py" --root "$ROOT" "$@"
