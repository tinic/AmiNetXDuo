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
if ! curl -fLs --max-time 120 -o "$out/$name" \
     "https://aminet.net/${path}"; then
    echo "FETCH_FAIL $path"
    exit 1
fi
case $(file -b "$out/$name") in
    *HTML*|*ASCII*) echo "FETCH_NOT_ARCHIVE $path"; exit 1 ;;
esac
( cd "$out/$name.d" && lha xq "../$name" >/dev/null 2>&1 ) || {
    echo "UNPACK_FAIL $path"; exit 1; }
echo "OK $path $out/$name.d"
