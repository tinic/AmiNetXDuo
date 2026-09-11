#!/usr/bin/env bash
#
# WHICH SocketBaseTagList() TAGS WE ANSWER, COUNTED RATHER THAN CLAIMED.
#
# docs/GAPS.md carried "Three, against Roadshow's 53; we answer 51" for long
# enough that nobody rechecked it.  Both numbers were wrong: Roadshow's header
# has 53 SBTC_ defines but one of them is a MACRO -- SBTC_ERRNOPTR(size), which
# selects among the three ERRNO*PTR codes -- so there are 52 codes, and
# src/bsdsocket/errno.c answers 49 of them.
#
# A sentence in a document cannot notice a tag being added on either side.
# This can, so the document says what this prints.
#
# SPDX-License-Identifier: MIT

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

ERRNO="src/bsdsocket/errno.c"

# The three Roadshow answers, with the reason each is not here.  A fourth
# appearing means the list below is stale, not that the gate is wrong.
KNOWN_MISSING="SBTC_IP_FILTER_HOOK SBTC_LOG_FILE_NAME SBTC_LOG_HOOK"

# IN A SUBSHELL.  Sourcing amiga-toolchain.sh runs whatever it does in THIS
# shell, and on a machine with no toolchain it exits -- which killed this gate
# before it printed a word, and `|| true` does not catch an exit from a sourced
# file.  macOS CI failed exactly that way, with an empty log.
TC="${AMIGA_TOOLCHAIN_ROOT:-}"
if [ -z "$TC" ]; then
    # shellcheck disable=SC1091
    TC=$( . "$ROOT/tools/amiga-toolchain.sh" >/dev/null 2>&1
          printf '%s' "${AMIGA_TOOLCHAIN_ROOT:-}" ) || TC=""
fi

HDR="$TC/m68k-amigaos/ndk-include/libraries/bsdsocket.h"

if [ -z "$TC" ] || [ ! -f "$HDR" ]; then
    # A gate that cannot see says so rather than passing quietly.
    echo "sbtc_tags=skipped reason=no_sdk_header path=${HDR:-<none>}"
    exit 0
fi

# The SDK header carries NUL bytes; tr keeps grep out of binary mode.  A define
# followed by whitespace is a code, one followed by "(" is a macro.
# `|| true` on every pipeline: grep exits 1 on no match, and under `set -e` an
# assignment from a failing substitution ends the script silently.
theirs=$(tr -d '\000' < "$HDR" |
         grep -oE '^#define[[:space:]]+SBTC_[A-Z0-9_]+[[:space:]]' |
         grep -oE 'SBTC_[A-Z0-9_]+' | sort -u || true)

# Ours: a row in one of the tables, or a case label.  A name in a comment is
# not an answer.
ours=$(grep -oE '^[[:space:]]*(case[[:space:]]+)?\{?[[:space:]]*SBTC_[A-Z0-9_]+' "$ERRNO" |
       grep -oE 'SBTC_[A-Z0-9_]+' | sort -u || true)

if [ -z "$theirs" ] || [ -z "$ours" ]; then
    echo "sbtc_tags=skipped reason=nothing_matched theirs=$(printf '%s' "$theirs" | wc -w)\
 ours=$(printf '%s' "$ours" | wc -w)"
    exit 0
fi

missing=$(comm -23 <(printf '%s\n' "$theirs") <(printf '%s\n' "$ours") | tr '\n' ' ' || true)
missing=$(echo $missing)
expected=$(echo $KNOWN_MISSING)

echo "sbtc_tags theirs=$(printf '%s\n' "$theirs" | wc -l)\
 ours=$(printf '%s\n' "$ours" | wc -l) missing=${missing:-none}"

if [ "$missing" != "$expected" ]; then
    echo "sbtc_tags=FAIL the unanswered set moved" >&2
    echo "  was: $expected" >&2
    echo "  now: ${missing:-none}" >&2
    echo "  Answer the tag, or update KNOWN_MISSING here and the count in" >&2
    echo "  docs/GAPS.md with it." >&2
    exit 1
fi

echo "sbtc_tags=PASS"
