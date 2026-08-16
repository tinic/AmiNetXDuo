#!/usr/bin/env bash
#
# Bound a log that a long-running process writes without limit.  Reads stdin,
# writes stdout.
#
#   tools/logcap.sh [-h HEAD_BYTES] [-t TAIL_LINES] < noisy > bounded
#
# WHY IT EXISTS
#
#   Amiberry is started with --log, which it needs -- tools/amiberry-run.sh
#   reads the backend and the illegal-instruction lines back out of that log,
#   and without --log they go to a file that is off by default.  Nothing capped
#   it.  Measured on an RTG guest: 87 MB per 30 s, about 3.3 GB an hour.  It
#   has filled playhouse3 three times; the last sweep found 30 GB in five stale
#   logs and 27.6 GB of that was one file.
#
#   The disk is the smaller cost.  The failure mode is the bad one: an arm
#   stalls, its serial log is 0 bytes, and Amiberry is writing millions of
#   `Denise queue without lock!` lines into a log nobody tails.  That reads
#   exactly like a driver hang in the code under test, and it is not one.
#
# WHAT IT DOES, in one pass, with no buffering of the whole stream
#
#   * COLLAPSES CONSECUTIVE REPEATS.  A run of lines that are the same APART
#     FROM THEIR DIGITS becomes the first RUN of them, then `[logcap] N
#     further line(s) like it`.  Modulo digits because Amiberry stamps every
#     line with a time and two counters --
#
#       05-693 [223 022-199/195]: Denise queue without lock! id=0
#       05-693 [223 022-200/196]: Denise queue without lock! id=0
#
#     -- so no two lines of the flood are byte-identical and a plain repeat
#     collapse catches none of it.  Measured on a 40 s SLIRP boot: 72,797 of
#     73,158 lines are that one message.
#
#     The first RUN of a group is always printed in full, so a short run of
#     lines whose NUMBERS are the content -- addresses, sizes, masks -- is
#     kept as it was.  Only a genuine flood is summarised.  Nothing about the
#     emulator's messages is written down here; the rule is generic.
#
#     RUN is 4, measured rather than picked: on a bridged iperf arm the flood
#     arrives interleaved with other lines in short bursts, so a RUN of 20 let
#     20,973 of them through and spent 1.2 MB of a 4 MB budget on one message.
#     Four is enough to see the shape of a repeat, and the count says the rest.
#   * CAPS THE HEAD.  Once HEAD bytes have been written, printing stops.  The
#     head is where everything anything greps for lives -- the board and MAC
#     lines, `UAENET: '<iface>' open successful`, the first illegal
#     instruction -- because they are all printed while the machine boots.
#   * KEEPS A TAIL RING.  The last TAIL lines are held and printed at EOF
#     under a banner saying how many lines were dropped between.  `tail -20`
#     on a run that exited early is a real diagnostic and a head-only cap
#     would have thrown it away.
#
#   So the output is at most HEAD + (TAIL lines) + two banners, whatever the
#   input is, and it never blocks the writer: everything past the cap is read
#   and discarded rather than left in the pipe.
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
