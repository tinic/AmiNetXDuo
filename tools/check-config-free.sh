#!/usr/bin/env bash
#
# Anything that can load the configuration has to link the free for it.
#
#   tools/check-config-free.sh [builddir]
#
# WHAT IT STOPS
#
# ami_config_load() allocates the interface list.  It did not always: until the
# definition ceiling was removed, AmiConfig.interfaces was a fixed array of two
# inside the struct and loading one allocated nothing at all, so no caller owed
# anything and none of the six had a free.  Making the list unlimited made it
# an allocation, and turned all six into leaks in the same commit.
#
# The list is 404 bytes per interface described (m68k), ami_alloc() is
# AllocVec(), and AmigaOS gives back nothing when a process exits.  So a
# command that loads the configuration and never calls ami_config_free() costs
# the machine that much every time it is run, until the next reboot.
# docs/ALLOCATIONS.md.
#
# This is the same debt, and the same shape of rule, as
# tools/check-netdb-free.sh -- read that one first, its header explains why the
# rule cannot be held by hand.  The lesson there was that such a rule is broken
# by ADDING a caller, not by removing a free, and a caller can be added by
# somebody who never reads this file.  A command that starts wanting the
# interface list acquires the debt silently.
#
# HOW
#
# The linker's map, not the source.  --gc-sections means a tool's map lists
# ami_config_load only when something in that tool can actually reach it, so
# this reads what was LINKED rather than what was written, and no spelling,
# macro or indirection hides a caller from it.
#
# ami_config_load_interface() is NOT a debt and is deliberately not listed
# below: it parses one file into a caller-supplied AmiIfConfig and allocates
# nothing that outlives the call.  Several commands use only that one.
#
# bsdsocket.library is not a tool and is not checked here: it is not a process
# that exits.  Its load is in netstack_startup() and its free is in
# ami_ns_destroy(), and tools/alloc-census.sh gates that pair on the real thing
# by provoking an expunge and reading what is still outstanding.
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
    echo "check-config-free: no tool maps under $MAPS; nothing was checked" >&2
    exit 2
fi

# Section names appear in a map whether the section survived or not.  A symbol
# printed with an address did survive, which is the question being asked.
#
# The trailing anchor keeps ami_config_load and ami_config_load_interface
# apart: without it the first would match inside the second and every command
# that reads a single interface file would be told it owes a free it does not.
kept_symbols() {
    grep -oE '^ +0x[0-9a-f]+ +ami_config_(load|free|reserve)$' "$1" 2>/dev/null |
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

    # ami_config_load() is the only one that takes ownership.  Neither
    # ami_config_free nor ami_config_reserve is a debt on its own.
    owes=$(printf '%s\n' "$syms" | grep -x 'ami_config_load' || true)
    [ -n "$owes" ] || continue

    CARRY=$((CARRY + 1))
    if ! printf '%s\n' "$syms" | grep -q '^ami_config_free$'; then
        BAD="$BAD$tool: ami_config_load
"
    fi
done

if [ -n "$BAD" ]; then
    echo "check-config-free: these commands load the configuration and never free it:" >&2
    printf '%s' "$BAD" | sed 's/^/  /' >&2
    echo >&2
    echo "  ami_config_load() allocates the interface list, 404 bytes for" >&2
    echo "  every interface DEVS:NetInterfaces describes, and AmigaOS reclaims" >&2
    echo "  nothing at exit.  Call ami_config_free() on every way out of the" >&2
    echo "  command.  A command that leaves main() from several places wants" >&2
    echo "  the body-function shape src/tools/ping.c uses, not atexit()." >&2
    exit 1
fi

echo "check-config-free: ok, $CARRY of $CHECKED commands load the configuration and all of them free it"
exit 0
