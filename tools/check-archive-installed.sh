#!/usr/bin/env bash
#
# Every file in the archive is installed, or is deliberately not installed for
# a recorded reason.  Anything else is a failure.
#
#   tools/check-archive-installed.sh              # the source half alone
#   tools/check-archive-installed.sh <outdir>     # and the staged tree
#
# <outdir> is the directory dist/make-dist.sh packs FROM: the one holding
# `AmiNetXDuo.info` and `AmiNetXDuo/`.  Without it the tree checks are skipped
# and the manifest is still checked against install/Install-AmiNetXDuo, which
# needs no build and is where tools/ci.sh's host stage runs it.
#
# THE HOLE THIS FILLS.  Two lists describe a release -- what dist/make-dist.sh
# packs and what install/Install-AmiNetXDuo copies -- and until now nothing
# compared them.  anxnet.device was in the first for eleven releases and in the
# second for none of them: every machine kept whatever SANA-II driver it
# already had through every reinstall, and no listing, checksum or round-trip
# check could see it, because each list was correct about itself.
#
# Still live when this was written: Docs.info, Examples.info and Terminal.info
# were packed and never copied.  A drawer's icon sits BESIDE the drawer, not
# inside it, and the installer copies each drawer's CONTENTS -- so Examples and
# Terminal arrived on the user's disk as drawers Workbench does not draw.
#
# WHAT IT CHECKS, and the directions matter more than the count
#
#   1. every file in the staged tree matches a row              (nothing new
#                                                                ships
#                                                                undecided)
#   2. every `always` row matches at least one file             (no stale row
#                                                                left behind by
#                                                                something that
#                                                                stopped
#                                                                shipping)
#   3. every `installed` row's token is still in the Installer  (no row
#      script                                                    claiming an
#                                                                install that
#                                                                was deleted)
#   4. the manifest parses: four fields, a known disposition, a
#      known presence, and a reason on every not-installed row
#
# 1 alone would have caught anxnet.device.  2 and 3 are what stop the manifest
# becoming the next thing that drifts: a record nothing checks is a record that
# is wrong within two releases.
#
# WHAT IT DOES NOT CHECK, so nobody mistakes green here for proof.  That a copy
# actually LANDS is a question about a real Workbench, and
# install/test/run-workbench.sh asks it: it mounts the installed volume after
# the installer has run and compares what it finds against the archive copy.
# This script checks that a decision was TAKEN about every file.  The two
# together are the gate; neither is on its own.
#
# Output is key=value plus an exit code: 0 green, 1 a defect, 2 cannot run.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
MANIFEST="$ROOT/install/ARCHIVE-MANIFEST"
INSTALLER="${AMINETXDUO_INSTALLER_SCRIPT:-$ROOT/install/Install-AmiNetXDuo}"
TREE="${1:-}"

[ -f "$MANIFEST" ]  || { echo "no $MANIFEST" >&2; exit 2; }
[ -f "$INSTALLER" ] || { echo "no $INSTALLER" >&2; exit 2; }

bad=0
err() { printf 'archive_error=%s\n' "$*"; bad=$((bad + 1)); }

# ------------------------------------------------------------ the manifest --
#
# Rows in file order, because the first matching row wins and a narrow row has
# to be able to sit above the wide one it lives inside.

G_GLOB=() G_DISP=() G_PRES=() G_WHY=() G_LINE=() G_HIT=()

trim() { # -> $REPLY, with the leading and trailing blanks off
    local s="$1"
    s="${s#"${s%%[![:space:]]*}"}"
    REPLY="${s%"${s##*[![:space:]]}"}"
}

lineno=0
while IFS= read -r raw || [ -n "$raw" ]; do
    lineno=$((lineno + 1))
    trim "$raw"; row="$REPLY"
    [ -n "$row" ] || continue
    case "$row" in '#'*) continue ;; esac

    # Four fields, so exactly three separators.  A reason with a pipe in it is
    # a malformed row and is reported as one rather than silently truncated.
    IFS='|' read -r f1 f2 f3 f4 f5 <<< "$row"
    if [ -n "${f5+x}" ] && [ -n "$f5" ]; then
        err "line $lineno: more than four fields (a '|' inside a reason?)"
        continue
    fi
    if [ -z "${f4+x}" ]; then
        err "line $lineno: fewer than four fields: $row"
        continue
    fi

    trim "$f1"; glob="$REPLY"
    trim "$f2"; disp="$REPLY"
    trim "$f3"; pres="$REPLY"
    trim "$f4"; why="$REPLY"

    case "$disp" in
        installed|not-installed) ;;
        *) err "line $lineno: unknown disposition '$disp'\
 (installed or not-installed)"; continue ;;
    esac
    case "$pres" in
        always|sometimes) ;;
        *) err "line $lineno: unknown presence '$pres' (always or sometimes)"
           continue ;;
    esac
    if [ -z "$glob" ]; then
        err "line $lineno: empty glob"; continue
    fi
    if [ -z "$why" ]; then
        if [ "$disp" = installed ]; then
            err "line $lineno: $glob is installed and names nothing in the\
 Installer script"
        else
            err "line $lineno: $glob is not installed and no reason is\
 recorded.  An unexplained 'not installed' is what shipped three drawer\
 icons for eleven releases."
        fi
        continue
    fi

    G_GLOB+=("$glob"); G_DISP+=("$disp"); G_PRES+=("$pres")
    G_WHY+=("$why");   G_LINE+=("$lineno"); G_HIT+=(0)
done < "$MANIFEST"

if [ "${#G_GLOB[@]}" = 0 ]; then
    err "no usable rows in $MANIFEST"
fi

# --------------------------------------------------------- glob to a regex --
#
# `*` must not cross a slash and `**` must, which no single shell facility
# gives: bash's globstar applies to pathname expansion, not to `[[ x == pat ]]`,
# and extglob has no "any depth" of its own.  One pass over the characters,
# longest token first.
glob_re() { # glob -> $REPLY, an anchored ERE
    local g="$1" out="" i=0 c n
    while [ "$i" -lt "${#g}" ]; do
        c="${g:$i:1}"
        n="${g:$((i + 1)):1}"
        case "$c" in
            '*') if [ "$n" = '*' ]; then out="$out.*"; i=$((i + 2))
                 else out="${out}[^/]*"; i=$((i + 1)); fi ;;
            '?') out="${out}[^/]"; i=$((i + 1)) ;;
            '.'|'+'|'('|')'|'['|']'|'{'|'}'|'^'|'$'|'|'|'\\')
                 out="$out\\$c"; i=$((i + 1)) ;;
            *)   out="$out$c"; i=$((i + 1)) ;;
        esac
    done
    REPLY="^$out\$"
}

RE=()
for g in "${G_GLOB[@]}"; do glob_re "$g"; RE+=("$REPLY"); done

# ------------------------------------------- the installer still says so --
#
# Direction 3.  A row that claims an install has to name something the script
# still contains, so deleting a copyfiles breaks the row rather than leaving it
# describing a release that no longer happens.
#
# COMMENTS COME OUT FIRST, and that is most of the value here.  Every `; ...`
# in that file is prose, and the prose mentions S_LIBS, S_TERM and every drawer
# by name; a grep that counted a comment would pass on an installer whose
# copies had all been deleted and whose commentary still described them.
#
# Quote-aware, because a `;` inside a string is not a comment and there are
# several -- help texts end sentences with semicolons.  Blanking the strings
# instead was tried and is WRONG in the other direction: the useful token for
# the trust store is the literal `"certificates"`, and blanking strings deleted
# exactly the thing the row names.  So: walk the line, toggle on `"`, cut at
# the first `;` that is outside one.
INSTALLER_CODE=$(awk '{
    q = 0
    for (i = 1; i <= length($0); i++) {
        c = substr($0, i, 1)
        if (c == "\"") q = !q
        else if (c == ";" && !q) { $0 = substr($0, 1, i - 1); break }
    }
    print
}' "$INSTALLER")

installed_rows=0 notinstalled_rows=0
for i in "${!G_GLOB[@]}"; do
    [ "${G_DISP[$i]}" = installed ] || { notinstalled_rows=$((notinstalled_rows + 1)); continue; }
    installed_rows=$((installed_rows + 1))
    tok="${G_WHY[$i]}"
    # Verbatim, quotes and parentheses and all.  Naming the whole `(source ...)`
    # rather than the bare variable is what makes the check bite: S_EXAM is a
    # substring of S_EXAMICON, so a row that named the variable alone would
    # still pass with its own copyfiles deleted and the icon's left behind.
    #
    # A HERE-STRING AND NOT A PIPE, under `set -o pipefail`.  `printf ... |
    # grep -q` reports 141, not 0, whenever the match is EARLY enough that grep
    # stops reading and printf takes a SIGPIPE -- so the check inverted itself
    # on exactly the rows whose token appears near the top of the file, and
    # passed on the ones near the bottom.  Caught here by
    # (tackon S_LIBS "bsdsocket.library"), which first appears at line 519 of
    # 1700 and was reported missing while the identical usergroup row passed.
    if ! grep -qF -- "$tok" <<< "$INSTALLER_CODE"; then
        err "line ${G_LINE[$i]}: ${G_GLOB[$i]} is recorded as installed via\
 '$tok', and no such token is in $(basename "$INSTALLER").  Either the copy\
 was deleted -- in which case the archive now ships a file nothing installs --\
 or the row names the wrong thing."
    fi
done

# --------------------------------------------------------- the staged tree --

files_seen=0 unaccounted=0
if [ -n "$TREE" ]; then
    if [ ! -d "$TREE" ]; then
        echo "no such directory: $TREE" >&2
        exit 2
    fi
    TREE=$(cd "$TREE" && pwd)

    # WHAT IS PACKED, not what is in the directory.  dist/make-dist.sh's
    # working directory also holds the .lha it just wrote and the .verify tree
    # it unpacked to compare against, and neither is a member of the archive.
    # The two names below are the two arguments its archiver call takes:
    #
    #     "$ARCHIVER" a "$ARCHIVE_NAME" AmiNetXDuo.info AmiNetXDuo
    #
    # Hardcoded and then checked, so a third member added there cannot slip
    # past this by being somewhere this script never looks -- which would be
    # the same blind spot one level up.
    if [ -f "$ROOT/dist/make-dist.sh" ] &&
       [ "$(grep -c 'AmiNetXDuo\.info AmiNetXDuo' "$ROOT/dist/make-dist.sh")" -lt 2 ]; then
        err "dist/make-dist.sh no longer packs exactly 'AmiNetXDuo.info\
 AmiNetXDuo'.  This script walks those two names; anything else it packs would\
 not be checked at all."
    fi

    # Regular files only.  A drawer carries no content of its own and is not a
    # thing the installer copies; it appears here through the files in it, and
    # an empty one that reached the archive would be caught by the round-trip
    # check in dist/make-dist.sh rather than by this.
    while IFS= read -r rel; do
        files_seen=$((files_seen + 1))
        hit=-1
        for i in "${!RE[@]}"; do
            if [[ "$rel" =~ ${RE[$i]} ]]; then hit=$i; break; fi
        done
        if [ "$hit" -lt 0 ]; then
            unaccounted=$((unaccounted + 1))
            err "UNACCOUNTED: $rel is in the archive and no row in\
 install/ARCHIVE-MANIFEST says what becomes of it.  Add one: 'installed' with\
 the Installer token that copies it, or 'not-installed' with the reason."
        else
            G_HIT[$hit]=$(( G_HIT[hit] + 1 ))
        fi
    done < <(cd "$TREE" && find AmiNetXDuo.info AmiNetXDuo -type f 2>/dev/null | sort)

    # Direction 2.  A row nothing matched, on a build that always has the
    # thing, is a row describing a file that stopped shipping -- and a stale
    # row is how a manifest becomes decoration.
    for i in "${!G_GLOB[@]}"; do
        [ "${G_PRES[$i]}" = always ] || continue
        [ "${G_HIT[$i]}" = 0 ] || continue
        err "line ${G_LINE[$i]}: ${G_GLOB[$i]} matched nothing in $TREE.\
  Either it stopped being packed -- delete the row -- or a wider row above it\
 is shadowing it, since the first matching row wins."
    done
fi

# ------------------------------------------------------------------ verdict --

printf 'archive_rows=%d installed=%d not_installed=%d\n' \
       "${#G_GLOB[@]}" "$installed_rows" "$notinstalled_rows"
if [ -n "$TREE" ]; then
    printf 'archive_files=%d unaccounted=%d\n' "$files_seen" "$unaccounted"
else
    printf 'archive_files=skipped (no tree given)\n'
fi

if [ "$bad" = 0 ]; then
    if [ -n "$TREE" ]; then
        echo "archive_installed=ok every packed file is installed or\
 deliberately not, and every claim is still in the Installer"
    else
        echo "archive_installed=ok every installed row is still in the\
 Installer (the tree was not checked)"
    fi
    exit 0
fi
printf 'archive_installed=%d problems\n' "$bad"
exit 1
