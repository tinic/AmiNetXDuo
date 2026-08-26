#!/usr/bin/env bash
#
# No delay in the netdev cores may be a counted loop over a bus access.
#
#   tools/check-netdev-delays.sh
#
# `ULONG n = us * 4u; while (n-- != 0) (VOID)*attr;` is not a measure of time.
# It measures how many times THIS CPU gets round a loop, which differs by two
# orders of magnitude between a 14 MHz 68020 and an accelerated one.
# netdev_clock.c replaced them with a wait measured against the raster beam;
# the unit test proves the primitive, this proves the call sites use it.
# It looks for the ARITHMETIC, `us * 4`, and not the loop: a spin with a real
# exit condition, or a bus barrier of N real cycles a faster CPU cannot shorten,
# is fine and several are deliberate.  Turning microseconds into a COUNT is
# never right -- a file that does it must route the result through
# netdev_wait_begin(), where the count becomes a floor rather than the wait.
#
# Output is key=value and an exit code: 0 clean, 1 a delay is counted again.
#
# SPDX-License-Identifier: MIT

set -eu

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CORE="$ROOT/src/netdev"

[ -d "$CORE" ] || { echo "netdev_delays=skipped reason=no_src_netdev" >&2; exit 2; }

# The microseconds-to-iterations conversion, in the two spellings the driver
# used: `us * 4` and `us * 4u`, with any amount of space, and the multiply
# either way round.
PATTERN='(\bus\b[[:space:]]*\*[[:space:]]*4u?\b|\b4u?[[:space:]]*\*[[:space:]]*\bus\b)'

bad=0
found=0

while IFS= read -r file; do
    # netdev_clock.c is where a count legitimately stops being a duration, and
    # test/ drives the old shape on purpose to show what it did -- the control
    # arm in test_netdev_clock.c IS the counted loop, and a check that refused
    # it would be a check against demonstrating the defect.
    case "$file" in
        */netdev_clock.c|*/netdev_clock.h|*/test/*) continue ;;
    esac

    while IFS=: read -r line text; do
        [ -n "$line" ] || continue

        # Prose, not code.  These files explain at length what the arithmetic
        # used to be, and quoting it is the opposite of doing it.
        case "$(echo "$text" | sed 's/^[[:space:]]*//')" in
            '*'*|'/*'*|'//'*) continue ;;
        esac

        found=$((found + 1))

        # A conversion is allowed only as an argument to netdev_wait_begin(),
        # where it is the floor and the beam is the duration.  The call is
        # allowed to be on the line before, which is how el3.c and ne2000.c
        # read best, so two lines of context either way are searched.
        from=$((line > 3 ? line - 3 : 1))
        to=$((line + 3))

        if sed -n "${from},${to}p" "$file" | grep -q 'netdev_wait_begin'; then
            continue
        fi

        printf 'netdev_delays=counted file=%s line=%s text=%s\n' \
               "${file#"$ROOT/"}" "$line" "$(echo "$text" | sed 's/^[[:space:]]*//')"
        bad=$((bad + 1))
    done <<EOF
$(grep -nE "$PATTERN" "$file" || true)
EOF
done <<EOF
$(find "$CORE" -name '*.c' -o -name '*.h' | sort)
EOF

# Second rule, same reason from the other end: a POLL bound.  A wait for a
# hardware bit is sized `<NAME>_SPINS`, and every use of one of those names
# outside its own #define must be an argument to netdev_wait_begin(), where the
# count is the floor and the beam is the deadline.  A bare `while (n-- != 0)`
# over a status register expires early on a fast machine and calls a working
# card dead -- the faster the Amiga, the likelier, which is backwards.
SPINS='\b[A-Z][A-Z0-9_]*_SPINS\b'

while IFS= read -r file; do
    case "$file" in
        */netdev_clock.c|*/netdev_clock.h|*/test/*) continue ;;
    esac

    while IFS=: read -r line text; do
        [ -n "$line" ] || continue

        case "$(echo "$text" | sed 's/^[[:space:]]*//')" in
            '*'*|'/*'*|'//'*|'#define'*) continue ;;
        esac

        found=$((found + 1))

        from=$((line > 3 ? line - 3 : 1))
        to=$((line + 3))

        if sed -n "${from},${to}p" "$file" | grep -q 'netdev_wait_begin'; then
            continue
        fi

        printf 'netdev_delays=counted_poll file=%s line=%s text=%s\n' \
               "${file#"$ROOT/"}" "$line" "$(echo "$text" | sed 's/^[[:space:]]*//')"
        bad=$((bad + 1))
    done <<EOF
$(grep -nE "$SPINS" "$file" || true)
EOF
done <<EOF
$(find "$CORE" -name '*.c' -o -name '*.h' | sort)
EOF

if [ "$bad" -ne 0 ]; then
    cat >&2 <<MSG

$bad delay(s) in src/netdev turn microseconds into an iteration count without
routing it through netdev_wait_begin().  A count of bus reads is a measure of
this CPU, not of time: on an accelerated Amiga the same loop finishes in a
fraction of the intended wait, and the waits in these files are the ones that
hold hardware in reset.  See src/netdev/netdev_clock.h.

If the spin really is a bus barrier or a poll rather than a duration -- some
here are, and they are named in netdev_clock.h -- then it should not be sized
in microseconds in the first place.
MSG
    echo "netdev_delays=fail counted=$bad conversions=$found"
    exit 1
fi

echo "netdev_delays=clean conversions=$found gated_by=netdev_wait_begin"
exit 0
