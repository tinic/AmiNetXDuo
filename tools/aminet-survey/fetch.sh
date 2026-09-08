#!/bin/sh
# Fetch one Aminet archive and unpack it.  `curl -fL`, and that is the point:
# the first harness omitted -f, so a 404 HTML page was written to a.lha, `lha
# xq` exited 0 on it, and the archive reported NO_AMIGA_EXE.  Six of six
# requested archives failed that way and every one looked like a result.
set -eu
path="$1"                       # e.g. comm/tcp/AmiFTP.lha
out="${2:-/tmp/anxd-survey}"
name=$(basename "$path")
mkdir -p "$out/$name.d"
# THE HTTP CODE, AND A PAUSE.  Both matter, and neither was here.
#
# A retry pass re-fetched 20 archives back to back and 18 came back FETCH_FAIL.
# They are not gone: GetAllHTML.lha and FTPMount.lha answer 200 on a single
# request seconds later.  aminet.net throttles a burst, so the survey was
# manufacturing its own failures -- and because a FETCH_FAIL row keys the
# archive as scanned, each one permanently retired an archive that exists.
#
# Charon.lha really is 404.  Recording the code is what tells the two apart:
# retry.sh skips 404 and re-attempts the rest, instead of hammering dead paths
# and giving up on live ones.  --retry rides out the transient case in place.
code=$(curl -fLs --max-time 120 --retry 3 --retry-delay 2 --retry-connrefused \
       -w '%{http_code}' -o "$out/$name" "https://aminet.net/${path}") || {
    echo "FETCH_FAIL $path http=${code:-000}"
    sleep "${ANXD_FETCH_PAUSE:-2}"
    exit 1
}
# Between successful fetches too: the burst is what draws the throttle, and a
# tick that walks a dozen archives is a burst.
sleep "${ANXD_FETCH_PAUSE:-2}"
# A POSITIVE TEST, because the negative one matched the wrong thing.  It was
#
#     case $(file -b "$out/$name") in *HTML*|*ASCII*) not an archive ;;
#
# and file(1) prints the first member's name after the type:
#
#     LHa (2.x) archive data [lh5], with "GetAllHTML.doc"
#
# so `*HTML*` matched GetAllHTML.doc INSIDE a perfectly good archive.  Every
# archive whose name carries HTML or ASCII was thrown away as a 404 page -- 8
# of the 18 FETCH_FAIL rows, and 24 archives in the worklist are named that
# way.  Matching what a thing IS beats enumerating what it must not be.
# file(1) prints LEADING WHITESPACE for these -- "  LHa (2.x) archive data" --
# so an anchored LHa* pattern silently matches nothing and rejects everything.
# Trimmed first, and `*archive data*` carries the general case regardless of
# which packer aminet used.
kind=$(file -b "$out/$name" | sed 's/^[[:space:]]*//')
case "$kind" in
    LHa*|LZX*|Zip*|gzip*|bzip2*|XZ*|*"archive data"*|*"compress'd"*) ;;
    *) echo "FETCH_NOT_ARCHIVE $path kind=${kind%%,*}"; exit 1 ;;
esac
( cd "$out/$name.d" && lha xq "../$name" >/dev/null 2>&1 ) || {
    echo "UNPACK_FAIL $path"; exit 1; }
echo "OK $path $out/$name.d"
