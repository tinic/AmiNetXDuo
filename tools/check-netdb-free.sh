#!/usr/bin/env bash
#
# Anything that can load the netdb has to link the free for it.
#
#   tools/check-netdb-free.sh [builddir]
#
# WHAT IT STOPS
#
# ami_netdb_load() builds four tables out of DEVS:Internet and hangs them off
# file-scope statics in src/config/netdb.c: 12,616 bytes on a stock install.
# ami_alloc() is AllocVec(), and AmigaOS gives back nothing when a process
# exits, so a command that loads them and never calls ami_netdb_free() costs
# the machine 12,616 bytes every time it is run, until the next reboot.
#
# That was one rule held by hand in eight places, and `hostname` broke it a
# week after it was written.  It broke it the way every such rule is broken:
# not by removing a free, but by adding a caller.  ami_config_load() used to
# load the netdb as a side effect, so `hostname` acquired the debt by asking
# for the interface list.  That side effect is gone (src/config/config_file.c);
# this is the other half, and it is the half that survives somebody adding a
# lookup to a command that has none today.
#
# HOW
#
# The linker's map, not the source.  --gc-sections means a tool's map lists
# ami_netdb_load only when something in that tool can actually reach it, so
# this reads what was LINKED rather than what was written, and no spelling,
# macro or indirection hides a caller from it.  A tool that names any
# allocating netdb entry point and does not name ami_netdb_free fails the
# build.
#
# bsdsocket.library is not a tool and is not checked here: it is not a process
# that exits, its load is bsd_lib_open() and its free is bsd_lib_expunge(), and
# tools/alloc-census.sh gates that pair on the real thing by provoking an
# expunge and reading what is still outstanding.
#
# SPDX-License-Identifier: MIT

set -eu

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD="${1:-${AMINETXDUO_BUILD:-$ROOT/build/cm}}"
case "$BUILD" in
    /*) ;;
    *)  BUILD="$ROOT/$BUILD" ;;
esac

MAPS="$BUILD/src/tools"

# A map that is not there is not a pass.  This runs after the link step, so an
# empty set means the wiring is wrong, not that every tool is clean.
set -- "$MAPS"/*.map
if [ ! -e "$1" ]; then
    echo "check-netdb-free: no tool maps under $MAPS; nothing was checked" >&2
    exit 2
fi

# Section names appear in a map whether the section survived or not.  A symbol
# printed with an address did survive, which is the question being asked.
kept_symbols() {
    grep -oE '^ +0x[0-9a-f]+ +ami_netdb_[a-z_]+$' "$1" 2>/dev/null |
        awk '{print $2}' | sort -u
}

BAD=""
CHECKED=0
CARRY=0

for map in "$MAPS"/*.map; do
    tool=$(basename "$map" .map)
    CHECKED=$((CHECKED + 1))

    syms=$(kept_symbols "$map")
    [ -n "$syms" ] || continue

    # Every entry point below reaches netdb_table(), which loads on demand, so
    # any one of them is enough to owe the free.  ami_netdb_free itself is not
    # a debt, so it is taken out before the question is asked.
    owes=$(printf '%s\n' "$syms" | grep -v '^ami_netdb_free$' || true)
    [ -n "$owes" ] || continue

    CARRY=$((CARRY + 1))
    if ! printf '%s\n' "$syms" | grep -q '^ami_netdb_free$'; then
        BAD="$BAD$tool: $(printf '%s' "$owes" | tr '\n' ' ')
"
    fi
done

if [ -n "$BAD" ]; then
    echo "check-netdb-free: these commands can load the netdb and never free it:" >&2
    printf '%s' "$BAD" | sed 's/^/  /' >&2
    echo >&2
    echo "  ami_netdb_load() allocates 12,616 bytes into statics that outlive" >&2
    echo "  the process, and AmigaOS reclaims nothing at exit.  Call" >&2
    echo "  ami_netdb_free() on every way out of the command, or stop reaching" >&2
    echo "  a netdb lookup from it." >&2
    exit 1
fi

echo "check-netdb-free: ok, $CARRY of $CHECKED commands reach the netdb and all of them free it"
exit 0
