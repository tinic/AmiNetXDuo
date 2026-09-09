#!/bin/bash
# One survey tick: fetch N archives, scan every HUNK executable in each.
# Appends TSV rows; dedups by archive name.
set -u
OUT=${ANXD_SURVEY_OUT:-/tmp/anxd-survey}
LEDGER=/home/turo/anxd-aminet/results.tsv
[ -f "$LEDGER" ] || printf 'archive\tfile\tverdict\tdistinct\tcalls\tlvos\n' > "$LEDGER"

# ONE WRITER AT A TIME.  rescan.sh rewrites the whole ledger -- `awk > tmp &&
# mv tmp ledger` -- so any row a tick appends between the awk and the mv is
# silently dropped.  Nothing would report it: the tick prints its findings and
# exits 0, and the rows are simply gone from a file that still looks healthy.
# Both writers take the same lock; the second waits rather than corrupting.
exec 9>"${LEDGER}.lock"
if ! flock -w 3600 9; then
    echo "$(basename "$0"): another survey writer holds the ledger lock" >&2
    exit 2
fi

# PICKING NOTHING IS NOT SUCCESS.  The caller used to build the list with
# `grep -vFf done.txt`, and `cut -f1 results.tsv` emits the header and an empty
# line -- an empty pattern in -f matches EVERY line, so the exclusion list
# excluded the whole worklist and the tick reported TICK_RC=0 having scanned
# zero archives.  Same shape as every other silent pass found today.
[ "$#" -gt 0 ] || { echo "tick: no archives given, nothing to do" >&2; exit 2; }

# FETCH EVERYTHING FIRST, IN PARALLEL.  fetch.sh slept 2 s between archives,
# added after aminet throttled a burst of twenty back-to-back requests and
# then applied serially to every archive after that.  Measured on 8 archives:
# 19.65 s sequential against 1.52 s at -P6, same 7 of 8 retrieved.
#
# THE PAUSE IS NOT GONE, IT IS OVERLAPPED.  Six sockets open at once is not
# the shape that drew the throttle -- twenty requests in a row is -- and curl
# still carries --retry.  ANXD_FETCH_JOBS=1 with ANXD_FETCH_PAUSE=2 is the way
# back if aminet objects.
#
# The outcome per archive goes in a status file rather than being inferred: a
# fetch that failed and a fetch never attempted are different rows, and an
# earlier half-applied version of this patch left the READ of these files in
# place with nothing writing them -- so 3,627 archives were recorded
# FETCH_FAIL without a single request being made.
# The scanner version belongs on EVERY row, not only the ones that named a
# vector.  These three writes carried six fields, so 3,583 NO_BSDSOCKET_BINARY
# rows landed with no scanner= at all and check-derived.sh refused the whole
# ledger -- correctly: "this archive holds no bsdsocket binary" is a claim, and
# a claim with no record of which scanner made it cannot be re-judged when the
# scanner changes.
SCANNER=$(python3 -c "
import sys; sys.path.insert(0, '/home/turo/anxd-aminet'); import scan
print(scan.SCANNER_VERSION)")

JOBS=${ANXD_FETCH_JOBS:-6}
mkdir -p "$OUT/status"
todo=""
for path in "$@"; do
    name=$(basename "$path")
    if cut -f1 "$LEDGER" | grep -qx "$name"; then
        echo "SKIP already scanned: $name"; continue
    fi
    todo="$todo $path"
    rm -f "$OUT/status/$name"
done
[ -n "$todo" ] || { echo "tick: nothing new to fetch"; }
# shellcheck disable=SC2086
[ -n "$todo" ] && printf '%s\n' $todo \
    | ANXD_FETCH_PAUSE=${ANXD_FETCH_PAUSE:-0} xargs -P "$JOBS" -I{} \
      sh -c 'n=$(basename "$1")
             if out=$(/home/turo/anxd-aminet/fetch.sh "$1" "$2" 2>&1); then
                 printf "OK\n"      > "$2/status/$n"
             else
                 printf "%s\n" "$out" > "$2/status/$n"
             fi' _ {} "$OUT"

for path in $todo; do
    name=$(basename "$path")
    # Keep the reason.  A bare FETCH_FAIL cannot be triaged: 404 is permanent
    # and worth retiring, 000/429 is this survey's own request rate and worth
    # re-attempting.  18 of 20 rows turned out to be the second kind.
    fout=$(cat "$OUT/status/$name" 2>/dev/null || echo "FETCH_FAIL no status file")
    if [ "$fout" != OK ]; then
        # The first word of fetch.sh's message is the reason class --
        # FETCH_FAIL, FETCH_NOT_ARCHIVE, UNPACK_FAIL -- and they are not the
        # same event.  Collapsing all three into a bare FETCH_FAIL is how a
        # glob bug that discarded 24 live archives looked like a dead mirror.
        why=$(printf '%s' "$fout" | tail -1 | cut -d' ' -f1)
        det=$(printf '%s' "$fout" | sed -n 's/.*\(http=[0-9]*\).*/\1/p' | tail -1)
        why="${why#FETCH_}${det:+ $det}"
        printf '%s\t-\tFETCH_%s\t0\t0\t\tscanner=%s\t%s\t\n' \\
            "$name" "${why:-FAIL http=000}" "$SCANNER" "$path" >> "$LEDGER"
        echo "FETCH_${why:-FAIL} $name"; continue
    fi
    found=0
    # -print0 / read -r, NOT `for f in $(find ...)`.  Word splitting broke every
    # path with a space in it -- "Update STFax.info" became two nonexistent
    # arguments, scan.py printed nothing, and the EMPTY output was written to
    # the ledger as a row.  36 blank rows accumulated that way, and a blank row
    # reads as data.  Files with spaces were also never actually scanned.
    # An archive that only partly extracted is recorded as such, alongside its
    # binary rows: those rows are real, but the set of them is not known to be
    # complete.  Not an OK-family verdict, so it never counts as attribution.
    case "$fout" in
        *UNPACK_PARTIAL*)
            printf '%s\t-\tUNPACK_PARTIAL\t0\t0\t\n' "$name" >> "$LEDGER"
            echo "UNPACK_PARTIAL $name -- rows from it may be short" ;;
    esac
    sha=$(sha256sum "$OUT/$name" 2>/dev/null | cut -d' ' -f1)
    errs=0
    while IFS= read -r -d '' f; do
        row=$(python3 scan.py "$f" 2>"$OUT/scan.err")
        # A CRASHED SCANNER IS NOT A FINDING.  This counted the error and fell
        # through to the found=0 line below, which writes NO_BSDSOCKET_BINARY
        # -- a positive claim that the archive holds no bsdsocket program,
        # manufactured out of the scanner failing to look.  Those rows then
        # count as surveyed and are never revisited.
        if [ -z "$row" ]; then
            errs=$((errs+1))
            echo "SCAN_ERROR $f: $(sed -n '$p' "$OUT/scan.err")" >&2
            continue
        fi
        case "$row" in
            *NO_BSDSOCKET_STRING*|*NO_HUNK*) continue ;;
        esac
        # The path RELATIVE to the archive, not the basename.  AmiVNC ships
        # AmiVNC.060 twice -- Executables/Planar/ and "Executables/RTG &
        # Planar/" -- and two different binaries recorded under one name read
        # as a duplicated row.  (That directory also has a space and an
        # ampersand in it, which is why the null-delimited find matters.)
        rel=${f#"$OUT/$name.d/"}
        row=${row#*$'\t'}
        # THE ARCHIVE'S FULL PATH AND ITS HASH, which is codex point 1.  A
        # basename does not identify an Aminet archive -- samba appears under
        # comm/net and comm/tcp -- and without a hash there is no way to tell
        # whether a row describes the file that is on the mirror today.  A
        # finding nobody can re-derive from the same bytes is an anecdote.
        printf '%s\t%s\t%s\t%s\t%s\n' "$name" "$rel" "$row" "$path" "$sha" >> "$LEDGER"
        echo "$name | $row" | cut -c1-150
        found=$((found+1))
    done < <(find "$OUT/$name.d" -type f -size +1k -print0 \
             | xargs -0 -r grep -lZ "bsdsocket.library" 2>/dev/null)
    if [ "$found" = 0 ]; then
        if [ "$errs" -gt 0 ]; then
            # Distinguishable, and deliberately NOT an OK-family verdict: it
            # is "we did not manage to look", which is a row to come back to
            # rather than a fact about the archive.
            printf '%s\t-\tSCAN_ERROR files=%s\t0\t0\t\tscanner=%s\t%s\t%s\n' \\
                "$name" "$errs" "$SCANNER" "$path" "$sha" >> "$LEDGER"
            echo "SCAN_ERROR $name ($errs files) -- NOT recorded as having no binary"
        else
            printf '%s\t-\tNO_BSDSOCKET_BINARY\t0\t0\t\tscanner=%s\t%s\t%s\n' \\
                "$name" "$SCANNER" "$path" "$sha" >> "$LEDGER"
        fi
    fi
done

# EVERY DERIVED TABLE IS REGENERATED HERE, AND A GENERATOR THAT DIES MUST FAIL
# THE TICK.  It did the opposite: each one was `python3 gen.py > table.tsv
# 2>/dev/null`, so the shell truncated the published table to zero bytes before
# python started, the traceback went to /dev/null, and the tick printed
#
#     lvo-usage: 0 of 143 vectors have a caller
#     TICK_RC=0
#
# A tick that scanned nothing and emptied three tables reported success.  The
# cause was one byte: Spitfire2.lha ships a drawer named `Spitfire<b2> Install`
# and 0xb2 is not valid UTF-8, so every generator raised UnicodeDecodeError
# (survey_io.py now decodes the ledger as latin-1, which round trips it).
#
# gen() fixes the shape rather than that one byte -- generate to a temporary,
# keep the previous good table if it fails, and remember the failure so the
# tick exits non-zero.  A summary that cannot see has to fail.
GEN_FAIL=0
gen() {   # destination, then the command
    dest=$1; shift
    if "$@" > "$dest.new" 2>"$dest.err"; then
        mv "$dest.new" "$dest"; rm -f "$dest.err"
    else
        GEN_FAIL=1
        echo "GEN_FAIL $(basename "$dest") -- table left at its previous contents" >&2
        sed -n '$p' "$dest.err" >&2
        rm -f "$dest.new"
    fi
}

# RUNNING TALLY OF HARNESS CANDIDATES, regenerated every tick so it cannot go
# stale.  tests/HARNESSES drives our own tools, LhA and a Workbench install and
# contains NO third-party network application, so everything here is net-new
# coverage.  Greedy set cover over the 143 vectors: the question a harness asks
# is "which handful covers the most API", not "which program is biggest".
gen /home/turo/anxd-aminet/candidates.tsv \
    python3 /home/turo/anxd-aminet/candidates.py /home/turo/anxd-aminet
echo "candidates: $(tail -1 /home/turo/anxd-aminet/candidates.tsv)"

# Per-LVO usage counts, regenerated every tick alongside the candidates.
gen /home/turo/anxd-aminet/lvo-usage.tsv \
    python3 /home/turo/anxd-aminet/usage.py /home/turo/anxd-aminet
echo "lvo-usage: $(awk -F'\t' 'NR>1 && $3>0' /home/turo/anxd-aminet/lvo-usage.tsv | wc -l) of 143 vectors have a caller"

# Vectors with fewer than 10 callers, WITH the callers named -- a count alone
# cannot decide those, and the distinct-program column separates "three
# daemons in three distributions" from "nine unrelated programs".
gen /home/turo/anxd-aminet/lvo-rare.tsv \
    python3 /home/turo/anxd-aminet/rare.py 10 /home/turo/anxd-aminet
echo "lvo-rare: $(awk -F'\t' 'NR>1 && !s[$2]++' /home/turo/anxd-aminet/lvo-rare.tsv | wc -l) vectors under 10 callers"

# The named callers of the vector codex is implementing.  Same generator, any
# vector: the names are what make a count decidable, and they should not have
# to be reconstructed by hand for the next one.
gen /home/turo/anxd-aminet/vsyslog-callers.tsv \
    python3 /home/turo/anxd-aminet/callers.py vsyslog /home/turo/anxd-aminet
echo "vsyslog-callers: $(wc -l < /home/turo/anxd-aminet/vsyslog-callers.tsv) rows"

[ "$GEN_FAIL" -eq 0 ] || { echo "tick: a derived table failed to regenerate" >&2; exit 1; }
