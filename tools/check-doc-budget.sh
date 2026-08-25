#!/usr/bin/env bash
#
#   tools/check-doc-budget.sh
#
# A rule is a gate script, a behaviour is a test, open work is one
# docs/BACKLOG.md row.  Narrative in docs/*.md is none of those.
#
# key=value and an exit code, like every other gate here.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT" || exit 2

TOTAL_MAX="${AMINETXDUO_DOC_TOTAL_MAX:-1150}"
FILE_MAX="${AMINETXDUO_DOC_FILE_MAX:-250}"

# AmiNetXDuo.guide is the shipped manual, not narrative.  Nothing else may
# join this list.
EXEMPT="docs/user/AmiNetXDuo.guide"

total=0
over=0
files=0

for f in docs/*.md; do
    [ -e "$f" ] || continue
    case " $EXEMPT " in
        *" $f "*) echo "doc_exempt=$f"; continue ;;
    esac

    n=$(wc -l < "$f")
    files=$((files + 1))
    total=$((total + n))

    if [ "$n" -gt "$FILE_MAX" ]; then
        over=$((over + 1))
        echo "doc_over_file_max=$f lines=$n max=$FILE_MAX"
    fi
done

echo "doc_files=$files"
echo "doc_file_max=$FILE_MAX"
echo "doc_over_file_max_count=$over"
echo "doc_total_lines=$total"
echo "doc_total_max=$TOTAL_MAX"

rc=0
[ "$over" -eq 0 ] || rc=1
if [ "$total" -gt "$TOTAL_MAX" ]; then
    echo "doc_total_over=$((total - TOTAL_MAX))"
    rc=1
fi

if [ "$rc" -eq 0 ]; then
    echo "check_doc_budget=PASS"
else
    echo "check_doc_budget=FAIL"
fi
exit "$rc"
