#!/bin/bash
# Pick a dozen unscanned archives, WEIGHTED BY WHERE THE YIELD IS.
#
# Measured after 299 archives: 23 of 27 attributed archives came from
# comm/tcp.  comm/bbs (722), comm/ambos (162) and comm/maxs (288) are BBS and
# door software with no TCP in them at all, so a uniform pick across comm/
# spent three consecutive ticks adding NO_BSDSOCKET_BINARY rows and no
# findings.  8/2/1/1 keeps some breadth without wasting the tick.
set -u
cd /home/turo/anxd-aminet
cut -f1 results.tsv | grep -v '^$\|^archive$' | sort -u > /tmp/done.txt
pick() {   # dir count seed
    grep "^comm/$1/" worklist.txt | awk -F/ '{print $0"\t"$NF}' \
        | grep -vFf /tmp/done.txt | cut -f1 \
        | shuf -n "$2" --random-source=<(yes "$3")
}
# comm/tcp IS EXHAUSTED as of 2026-09-08 -- all 752 archives are in the ledger,
# and it was the highest-yield directory by far (23 of the first 27 attributed
# archives).  Weighting now follows what is left AND where sockets actually
# live: net and mail are protocol clients, irc is clients too, www is mostly
# HTML tooling, misc is a grab bag.
{ pick net 6 "$1"; pick mail 2 "$1"; pick irc 2 "$1"; \
  pick www 1 "$1"; pick misc 1 "$1"; } | sort -u
