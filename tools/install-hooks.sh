#!/usr/bin/env bash
# Point git at .githooks. Idempotent; run once per clone.
# SPDX-License-Identifier: MIT
set -eu
cd "$(git rev-parse --show-toplevel)"
git config core.hooksPath .githooks
echo "hooks=installed path=$(git config core.hooksPath)"
