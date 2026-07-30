#!/usr/bin/env bash
#
# Fetch the third-party SANA-II drivers whose licences permit it, so the
# emulated network cards have something able to open them.
#
#   tools/fetch-sana2-drivers.sh                    # fetch all fetchable, print the dir
#   tools/fetch-sana2-drivers.sh cnet.device        # just that one
#   tools/fetch-sana2-drivers.sh --list             # every driver and its licence verdict
#   tools/fetch-sana2-drivers.sh --print-path NAME  # where NAME is, or why it is not fetched
#   tools/fetch-sana2-drivers.sh --print-dir        # just say where they would be
#   tools/fetch-sana2-drivers.sh --print-sha        # the pins, for a CI cache key
#   tools/fetch-sana2-drivers.sh --check            # verify the cache, no network
#   tools/fetch-sana2-drivers.sh --force            # re-fetch even if cached
#
# WHY THIS EXISTS
#
#   Nine emulated ethernet cards work under WinUAE and Amiberry.  Eight of them
#   fail at netstack_startup() (0xFFFFFFFE) for one reason: no SANA-II driver is
#   staged, so nothing can open the device.  The boards need no ROM and the
#   emulator's backend is shared across all of them, so the drivers are the
#   whole blocker -- docs/RESEARCH.md 76.3.
#
#   Two of the eight may be fetched.  The rest may not, and this script names
#   the reason rather than pretending they do not exist, because the useful
#   answer to "why is the X-Surf column empty" is the licence, not a shrug.
#   docs/RESEARCH.md 77 has the per-driver terms with quotes.
#
# WHAT IS NOT DONE HERE, DELIBERATELY
#
#   Nothing is redistributed.  Aminet's upload terms let Aminet serve a file;
#   they do not let this repository carry it onward.  So these are fetched from
#   their own publisher on the machine that needs them, and no download ever
#   lands inside the working tree.
#
#   The archives are deleted after extraction.  Only the .device members are
#   kept: the cnet archive is 460 KB of source, docs and eight build variants
#   for one 8 KB driver we actually stage.
#
# A BAD HASH IS NOT A REASON TO PROCEED
#
#   Every download is pinned by sha256, and so is every extracted driver.  A
#   mismatch stops the script and writes nothing.  An unpinned driver fetch
#   fails invisibly: you get a card that does not come up and no reason why,
#   which is indistinguishable from the bug the card is being tested for.
#
# BEING A POLITE CLIENT
#
#   Aminet is volunteer-run.  Downloads are serial, spaced, identified by
#   User-Agent, and do not retry -- a transient failure is reported and the run
#   stops rather than hammering an archive that may be having a bad day.
#
# WHERE IT LANDS
#
#   $AMINETXDUO_SANA2_CACHE, or ~/.cache/aminetxduo/sana2-drivers by default:
#
#       <cache>/<pins12>/<name>.device     the drivers
#       <cache>/current                    symlink to it
#
#   <pins12> is derived from the pins, so changing one re-fetches into a new
#   directory instead of mixing versions, and `current` is the stable path.
#
# STAGING ONE INTO A TEST
#
#   tools/sana2-stage.sh takes a path, so:
#
#       DRV=$(tools/fetch-sana2-drivers.sh --print-path cnet.device)
#       AMINETXDUO_SANA2_DRIVER=$DRV tests/netstack/run-winuae.sh -N ne2000_pcmcia
#
# SPDX-License-Identifier: MIT

set -euo pipefail

# ------------------------------------------------------------------- pins ----

# name | url | archive sha256 | member in archive | driver sha256
#
# cnet.device is public domain, said so by all three of its authors in
# cnetdevice/cnet.guide.  The plain member is the 68020+ build; the archive also
# has .000, .debug, .turboio and 16-bit-access variants, and the 68020+ one is
# what an A1200 with an accelerator wants.
#
# hydra.device comes from the SOURCE archive, not HydraDriver144.lha.  That is
# not an accident: 145src carries "Distribution: Aminet" and the named
# permission of the hardware's creator, the copyright holder's owner and the
# author's estate, and it contains the last released 1.44 binary.  The plain
# 144 binary release carries no Distribution line at all, so the archive that
# looks like the obvious download is the one that cannot be justified.
SANA2_PINS=(
  "cnet.device|https://aminet.net/driver/net/cnetdevice.lha|65580fae37012291c657dc8a594c2219f87f60b5d0f743f689337d4e9b4030f4|cnetdevice/Devs/Networks/cnet.device|4496d1bd34984671b15982e97efec3a2c670a37bc8e8ddd404131082d1091b53"
  "hydra.device|https://aminet.net/driver/net/HydraDriver145src.lha|56495d5f83e3e264bca02959ff50cb84b81b940ae22c89c43e4c34a0275d34e0|HydraDriver145src/hydra.device|97174b8a29657b914fb699cb12a51f1f9b5974a4bfd4c879e797ce753e1ff620"
)

# name | one-line verdict | what to do instead
# Asked for any of these by name, the script explains instead of failing with
# "unknown driver".  Quotes and sources are in docs/RESEARCH.md 77.4.
SANA2_REFUSED=(
  "x-surf.device|Individual Computers publish it, but wiki.icomp.de's copyright page reserves reproduction elsewhere without written permission, so a CI fetch is not covered.|Download X-surf-1.16.lha by hand: https://wiki.icomp.de/wiki/X-Surf-100"
  "x-surf-100.device|Individual Computers publish it, but wiki.icomp.de's copyright page reserves reproduction elsewhere without written permission, so a CI fetch is not covered.|Download X-surf-1.16.lha by hand: https://wiki.icomp.de/wiki/X-Surf-100"
  "ariadne.device|Village Tronic's own readme says 'Copyright (C) 1994-1996 Village Tronic Marketing GmbH, All rights reserved'. That is an explicit reservation, and the company is gone, so nobody can grant it now.|No fetchable source exists. Aminet has no Ariadne driver, only a review."
  "ariadne_ii.device|AriadneII_43_12.lha holds two binaries and no licence text of any kind. Terms that cannot be established are a no.|No fetchable source exists."
  "eb920.device|The Aminet upload is ASDG's 1996 install disk posted by a third party, with no Distribution field and no licence text in the archive. ASDG granted nothing.|No fetchable source exists."
  "a2065.device|Commodore's, and not redistributable -- see docs/DEVELOPMENT.md and tools/ci.sh. The Aminet a2065v216a.lha is a third party's patch of Commodore's source, which inherits the problem.|Bring your own copy and set AMINETXDUO_A2065=<path>."
)

# ---------------------------------------------------------------- options ----

MODE="fetch"
FORCE=0
WANT_NAME=""
SELECTED=()

while [ $# -gt 0 ]; do
    case "$1" in
        --list)       MODE="list" ;;
        --print-dir)  MODE="printdir" ;;
        --print-sha)  MODE="printsha" ;;
        --check)      MODE="check" ;;
        --print-path) MODE="printpath"; WANT_NAME="${2:-}"; shift ;;
        --force)      FORCE=1 ;;
        -h|--help)    sed -n '2,79p' "$0"; exit 0 ;;
        -*) echo "usage: $0 [--list|--print-dir|--print-sha|--check|--print-path NAME] [--force] [driver...]" >&2; exit 2 ;;
        *)  SELECTED+=("$1") ;;
    esac
    shift
done

# --print-path is the machine-readable one, so its diagnostics go to stderr and
# stdout carries a path or nothing.
say() { [ "$MODE" = "printpath" ] && echo "$*" >&2 || echo "$*"; }

CACHE="${AMINETXDUO_SANA2_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/aminetxduo/sana2-drivers}"

pin_field() { printf '%s' "$1" | cut -d'|' -f"$2"; }

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    else
        shasum -a 256 "$1" | cut -d' ' -f1
    fi
}

sha256_of_stdin() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum | cut -d' ' -f1
    else
        shasum -a 256 | cut -d' ' -f1
    fi
}

# One directory per set of pins, so bumping a driver never mixes versions into a
# directory CI has already cached under the old key.
pins_digest() {
    local p out=""
    for p in "${SANA2_PINS[@]}"; do
        out="$out$(pin_field "$p" 1):$(pin_field "$p" 5)
"
    done
    printf '%s' "$out" | sha256_of_stdin | cut -c1-12
}

PINS12=$(pins_digest)
DIR="$CACHE/$PINS12"

# Report a refused driver by name.  Returns 0 if it was one, so callers can
# tell "explained" from "not a driver we know".
explain_refusal() {
    local want="$1" r
    for r in "${SANA2_REFUSED[@]}"; do
        [ "$(pin_field "$r" 1)" = "$want" ] || continue
        echo "" >&2
        echo "!! $want is not fetched automatically." >&2
        printf '   %s\n' "$(pin_field "$r" 2)" >&2
        printf '   %s\n' "$(pin_field "$r" 3)" >&2
        echo "   docs/RESEARCH.md 77 has the licence terms in full." >&2
        return 0
    done
    return 1
}

is_fetchable() {
    local want="$1" p
    for p in "${SANA2_PINS[@]}"; do
        [ "$(pin_field "$p" 1)" = "$want" ] && return 0
    done
    return 1
}

# ------------------------------------------------------------- read-only ----

if [ "$MODE" = "printdir" ]; then
    printf '%s\n' "$DIR"
    exit 0
fi

# The CI cache key wants the pins without having to parse this file.
if [ "$MODE" = "printsha" ]; then
    for p in "${SANA2_PINS[@]}"; do
        printf '%s %s %s\n' "$(pin_field "$p" 1)" "$(pin_field "$p" 3)" "$(pin_field "$p" 5)"
    done
    exit 0
fi

if [ "$MODE" = "list" ]; then
    echo "fetchable -- licences permit downloading these:"
    for p in "${SANA2_PINS[@]}"; do
        name=$(pin_field "$p" 1)
        state="not fetched"
        [ -f "$DIR/$name" ] && state="cached"
        printf '  %-20s %-12s %s\n' "$name" "$state" "$(pin_field "$p" 2)"
    done
    echo ""
    echo "not fetched -- terms do not permit it (docs/RESEARCH.md 77.4):"
    for r in "${SANA2_REFUSED[@]}"; do
        printf '  %-20s %s\n' "$(pin_field "$r" 1)" "$(pin_field "$r" 2)"
    done
    echo ""
    echo "Any of the refused ones can still be staged from a local copy:"
    echo "  AMINETXDUO_SANA2_DRIVER=<path> tests/netstack/run-winuae.sh -N <board>"
    exit 0
fi

if [ "$MODE" = "printpath" ]; then
    [ -n "$WANT_NAME" ] || { echo "--print-path needs a driver name" >&2; exit 2; }
    if explain_refusal "$WANT_NAME"; then
        exit 1
    fi
    is_fetchable "$WANT_NAME" || {
        echo "!! $WANT_NAME is not a driver this script knows about." >&2
        echo "   tools/fetch-sana2-drivers.sh --list shows all of them." >&2
        exit 2
    }
    [ -f "$DIR/$WANT_NAME" ] || {
        echo "!! $WANT_NAME is not fetched yet; run tools/fetch-sana2-drivers.sh" >&2
        exit 1
    }
    printf '%s\n' "$DIR/$WANT_NAME"
    exit 0
fi

# --check verifies what is cached against the pins and touches no network.  A
# driver that was never fetched is not a failure; one whose bytes have changed
# under us is.
if [ "$MODE" = "check" ]; then
    rc=0
    for p in "${SANA2_PINS[@]}"; do
        name=$(pin_field "$p" 1)
        want=$(pin_field "$p" 5)
        if [ ! -f "$DIR/$name" ]; then
            printf '  %-20s not fetched\n' "$name"
            continue
        fi
        got=$(sha256_of "$DIR/$name")
        if [ "$got" = "$want" ]; then
            printf '  %-20s ok\n' "$name"
        else
            printf '  %-20s MISMATCH (want %s, got %s)\n' "$name" "$want" "$got"
            rc=1
        fi
    done
    exit "$rc"
fi

# ------------------------------------------------------------------ fetch ----

# A name given on the command line may be a refused one; explain and stop
# rather than quietly fetching only the others.
if [ "${#SELECTED[@]}" -gt 0 ]; then
    for want in "${SELECTED[@]}"; do
        if explain_refusal "$want"; then
            exit 1
        fi
        is_fetchable "$want" || {
            echo "!! $want is not a driver this script knows about." >&2
            echo "   tools/fetch-sana2-drivers.sh --list shows all of them." >&2
            exit 2
        }
    done
fi

wanted() {
    [ "${#SELECTED[@]}" -eq 0 ] && return 0
    local w
    for w in "${SELECTED[@]}"; do
        [ "$w" = "$1" ] && return 0
    done
    return 1
}

need() { command -v "$1" >/dev/null 2>&1 || { echo "missing required tool: $1" >&2; exit 2; }; }
need curl

# lha reads both variants here; libarchive reads the generic-header one but not
# the Amiga-header one, so it cannot be the only extractor.  Either is fine and
# both give identical bytes, which the driver pin then proves.
EXTRACTOR=""
for cand in lha bsdtar 7z 7zz; do
    command -v "$cand" >/dev/null 2>&1 && { EXTRACTOR="$cand"; break; }
done
[ -n "$EXTRACTOR" ] || {
    echo "!! no LHA extractor found.  Install one of:" >&2
    echo "     lha        Debian/Ubuntu: lhasa      macOS: brew install lha" >&2
    echo "     bsdtar     Debian/Ubuntu: libarchive-tools  (generic headers only)" >&2
    echo "     7z         Debian/Ubuntu: p7zip-full" >&2
    exit 2
}

# Refuses rather than returns non-zero: a hash mismatch is never something to
# route around.
verify_sha256() {
    local file="$1" want="$2" what="$3" got
    got=$(sha256_of "$file")
    if [ "$got" != "$want" ]; then
        echo "" >&2
        echo "!! $what does not match its pin -- refusing to install it." >&2
        echo "   want $want" >&2
        echo "   got  $got" >&2
        echo "   Nothing was written to $CACHE." >&2
        exit 1
    fi
}

extract_member() {
    local archive="$1" member="$2" into="$3"
    case "$EXTRACTOR" in
        lha)     ( cd "$into" && lha xq2 "$archive" "$member" ) >/dev/null 2>&1 ;;
        bsdtar)  bsdtar xf "$archive" -C "$into" "$member" >/dev/null 2>&1 ;;
        7z|7zz)  "$EXTRACTOR" x -y -o"$into" "$archive" "$member" >/dev/null 2>&1 ;;
    esac
    [ -f "$into/$member" ]
}

TMP=$(mktemp -d "${TMPDIR:-/tmp}/aminetxduo-sana2.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

UA="AmiNetXDuo/1.0 (+https://github.com/tinic/AmiNetXDuo) sana2-driver-fetch"

FETCHED=0
FIRST=1

for p in "${SANA2_PINS[@]}"; do
    name=$(pin_field "$p" 1)
    url=$(pin_field  "$p" 2)
    asha=$(pin_field "$p" 3)
    member=$(pin_field "$p" 4)
    dsha=$(pin_field "$p" 5)

    wanted "$name" || continue

    if [ "$FORCE" = "0" ] && [ -f "$DIR/$name" ] &&
       [ "$(sha256_of "$DIR/$name")" = "$dsha" ]; then
        say "==> $name already at $DIR"
        continue
    fi

    # Spaced out, and only between actual downloads, so a fully cached run
    # costs nothing.
    [ "$FIRST" = "1" ] || sleep 2
    FIRST=0

    say "==> fetching $name"
    say "    $url"

    ARCHIVE="$TMP/$(basename "$url")"
    # No --retry on purpose: these are volunteer archives, and a failure worth
    # reporting is worth stopping for.
    if ! curl -fsSL -A "$UA" --connect-timeout 20 --max-time 300 \
              "$url" -o "$ARCHIVE" 2>"$TMP/curl.err"; then
        echo "!! could not fetch $url" >&2
        echo "   $(tail -1 "$TMP/curl.err" 2>/dev/null)" >&2
        exit 1
    fi

    verify_sha256 "$ARCHIVE" "$asha" "the $name archive"
    say "    archive sha256 verified"

    rm -rf "$TMP/x"
    mkdir -p "$TMP/x"
    extract_member "$ARCHIVE" "$member" "$TMP/x" || {
        echo "!! $EXTRACTOR could not extract $member from $(basename "$ARCHIVE")" >&2
        exit 1
    }

    # The archive hash already passed, so a wrong driver here would mean the
    # extractor mangled it -- which is exactly what this second pin catches.
    verify_sha256 "$TMP/x/$member" "$dsha" "$name"
    say "    driver sha256 verified"

    # Created only once something has passed both pins, so a failed run leaves
    # no directory behind to make "nothing was written" a lie.
    mkdir -p "$DIR"
    mv "$TMP/x/$member" "$DIR/$name.tmp"
    mv "$DIR/$name.tmp" "$DIR/$name"
    rm -f "$ARCHIVE"
    FETCHED=$((FETCHED + 1))
done

# `current` is the stable path for anything that would rather not compute the
# pins digest, and only points at a directory that exists.
if [ -d "$DIR" ]; then
    ln -sfn "$DIR" "$CACHE/current.tmp"
    mv -f "$CACHE/current.tmp" "$CACHE/current"
fi

[ "$FETCHED" = "0" ] || say "==> installed $FETCHED driver(s)"
say "==> $DIR"

if [ "$MODE" != "printpath" ]; then
    printf '%s\n' "$DIR"
fi
