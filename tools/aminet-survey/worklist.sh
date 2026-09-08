#!/bin/sh
# Rebuild the corpus list from Aminet's per-directory listings.
#
# NOT the `tree?path=...&count=500` endpoint: it returns 7 KB with zero .lha
# references.  The plain directory listing returns the real thing -- 98 for
# comm/tcp -- and is what the old worklist should have been built from.
set -u
: > /home/turo/anxd-aminet/worklist.txt
for d in "$@"; do
    n=$(curl -fLs --max-time 60 "https://aminet.net/$d/" \
        | grep -oE '[A-Za-z0-9_.+-]+\.lha' | sort -u \
        | sed "s|^|$d/|" | tee -a /home/turo/anxd-aminet/worklist.txt | wc -l)
    echo "$d $n"
done
echo "total $(wc -l < /home/turo/anxd-aminet/worklist.txt)"
