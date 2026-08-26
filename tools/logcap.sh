#!/usr/bin/env bash
#
# Bound a log that a long-running process writes without limit.  Reads stdin,
# writes stdout.
#
#   tools/logcap.sh [-h HEAD_BYTES] [-t TAIL_LINES] < noisy > bounded
#
# The output is at most HEAD + (TAIL lines) + two banners, whatever the input
# is, and it never blocks the writer: everything past the cap is read and
# discarded rather than left in the pipe.
#
#   AMINETXDUO_LOG_HEAD  bytes to print before capping   (default 4194304)
#   AMINETXDUO_LOG_TAIL  lines kept for the end          (default 200)
#   AMINETXDUO_LOG_RUN   lines of a repeat printed in full (default 4)
#   HEAD or TAIL may be 0: HEAD=0 caps immediately, TAIL=0 keeps no ring.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

HEAD="${AMINETXDUO_LOG_HEAD:-4194304}"
TAIL="${AMINETXDUO_LOG_TAIL:-200}"
RUN="${AMINETXDUO_LOG_RUN:-4}"

while getopts "h:t:r:" opt; do
    case "$opt" in
        h) HEAD="$OPTARG" ;;
        t) TAIL="$OPTARG" ;;
        r) RUN="$OPTARG" ;;
        *) echo "usage: $0 [-h head_bytes] [-t tail_lines] [-r run_lines]" >&2
           exit 2 ;;
    esac
done

# LC_ALL=C: the emulator writes bytes, not text, and a stray sequence that is
# not valid in the runner's locale makes some awks stop reading the stream.
LC_ALL=C exec awk -v head="$HEAD" -v tailn="$TAIL" -v runmax="$RUN" '
function emit(s) { print s; nout += length(s) + 1; if (nout <= head) fflush(); }

# Print, or ring, one line.  Past the cap nothing is printed at all.
function put(s) {
    if (nout < head) {
        emit(s)
        if (nout >= head)
            emit("[logcap] " head " bytes written; the rest of this log is" \
                 " dropped, and the last " tailn " line(s) follow at the end.")
    } else {
        dropped++
        if (tailn > 0) ring[ri++ % tailn] = s
    }
}

# The run that just ended, if it was long enough to have been summarised.
function flushrun() {
    if (run > runmax) put("[logcap] " (run - runmax) " further line(s) like it")
    run = 0
}

{
    key = $0
    gsub(/[0-9]+/, "#", key)
    if (NR > 1 && key == prevkey) {
        run++
        if (run <= runmax) put($0)
        next
    }
    flushrun()
    prevkey = key
    run = 1
    put($0)
}

END {
    flushrun()
    if (dropped > 0) {
        print "[logcap] " dropped " line(s) dropped after the cap."
        if (tailn > 0) {
            print "[logcap] the last " (ri < tailn ? ri : tailn) " of them:"
            for (i = (ri > tailn ? ri - tailn : 0); i < ri; i++)
                print ring[i % tailn]
        }
    }
    fflush()
}'
