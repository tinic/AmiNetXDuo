#!/usr/bin/env bash
#
# The stack's one runtime allocation may not grow without somebody typing the
# new number.
#
#   tools/check-ram-size.sh <build-dir>
#   AMINETXDUO_RAM_ARM=minimal tools/check-ram-size.sh <build-dir>
#
# sizeof(AmiNetStack) IS the resident cost of running the stack: netstack.c
# allocates exactly one of these and holds it for the life of the machine.  On
# a 2 MB A1200 with 1.2 MB free it was 64,624 bytes in the minimal drawer --
# five per cent of everything the machine had -- and NOTHING measured it.
# tools/check-image-size.sh guards the four images; this guards the allocation
# behind them, which the 2026-09-05 campaign cut by 78 per cent with no gate
# in front of it the whole way.
#
# MEASURED, NOT PARSED.  There is no symbol to read: the struct is a compile
# time size, so this compiles a one-line translation unit that declares an
# array of exactly that many bytes and asks m68k-amigaos-size how big the
# object came out.  `nm --size-sort` is useless here for the reason it is
# useless everywhere on this target -- an AmigaOS object is a single hunk.
#
# ONLY THE TWO CONFIGURATIONS THAT SHIP are budgeted, the same two
# tools/check-image-size.sh takes.  Coverage arms carry whatever their options
# imply and holding them to a shipping number would only teach whoever hits it
# to raise it.
#
# SPDX-License-Identifier: MIT

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/build/ci/default}"
ARM="${AMINETXDUO_RAM_ARM:-$(basename "$BUILD")}"

# arm : budget in bytes
#
# Set 2026-09-05 after the resident-RAM campaign, headroom to the next round
# 1 KB.  full is 40,292 and minimal is 14,016; both come from the member table
# in the campaign ledger, and every reduction behind them is measured on the
# rig rather than guessed.
BUDGETS=(
    "default:41000"
    "minimal:15000"
)

budget=""
for row in "${BUDGETS[@]}"; do
    [ "${row%%:*}" = "$ARM" ] && budget="${row##*:}"
done

if [ -z "$budget" ]; then
    echo "ram_size=skipped arm=$ARM reason=not_a_shipping_configuration"
    exit 0
fi

CC="$BUILD/compile_commands.json"
if [ ! -r "$CC" ]; then
    echo "ram_size=skipped arm=$ARM reason=no_compile_commands dir=$BUILD"
    echo "  Configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON to gate this."
    exit 0
fi

# The size binary comes from the compiler compile_commands.json already
# names, not from AMIGA_TOOLCHAIN_ROOT: this has to work when run by hand as
# well as from tools/ci.sh, and the two must never disagree about which
# toolchain measured the number.
SIZE=""
if [ -n "${AMIGA_TOOLCHAIN_ROOT:-}" ] &&
   [ -x "${AMIGA_TOOLCHAIN_ROOT}/bin/m68k-amigaos-size" ]; then
    SIZE="${AMIGA_TOOLCHAIN_ROOT}/bin/m68k-amigaos-size"
else
    _cc=$(python3 -c '
import json,sys
db=json.load(open(sys.argv[1]))
e=[c for c in db if c["file"].endswith("src/netstack/netstack.c")]
print(e[0]["command"].split()[0] if e else "")' "$CC" 2>/dev/null || true)
    case "$_cc" in
        */*-gcc) SIZE="${_cc%-gcc}-size" ;;
    esac
fi

if [ -z "$SIZE" ] || [ ! -x "$SIZE" ]; then
    echo "ram_size=skipped arm=$ARM reason=no_toolchain_size"
    exit 0
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

bytes=$(python3 - "$CC" "$TMP" "$SIZE" <<'PY'
import json, re, subprocess, sys
cc, tmp, size = sys.argv[1], sys.argv[2], sys.argv[3]
db = json.load(open(cc))
ent = [c for c in db if c["file"].endswith("src/netstack/netstack.c")]
if not ent:
    print("NONE"); raise SystemExit
e = ent[0]
flags = " ".join(re.findall(r"-I\s*\S+|-isystem\s*\S+|-D\S+|-include\s*\S+",
                            e["command"]))
gcc = e["command"].split()[0]
open(f"{tmp}/probe.c", "w").write(
    '#include "netstack_internal.h"\n'
    'volatile char probe[sizeof(AmiNetStack)] = {1};\n')
r = subprocess.run(f'{gcc} {flags} -fno-common -c {tmp}/probe.c -o {tmp}/probe.o',
                   shell=True, cwd=e["directory"], capture_output=True, text=True)
if r.returncode:
    print("NONE"); raise SystemExit
out = subprocess.run([size, f"{tmp}/probe.o"], capture_output=True,
                     text=True).stdout.strip().split("\n")[-1].split()
print(int(out[0]) + int(out[1]) + int(out[2]))
PY
)

if [ "$bytes" = "NONE" ] || [ -z "$bytes" ]; then
    echo "ram_size=skipped arm=$ARM reason=probe_would_not_build"
    exit 0
fi

if [ "$bytes" -gt "$budget" ]; then
    echo "ram_size=OVER arm=$ARM bytes=$bytes budget=$budget over=$((bytes - budget))"
    echo "  sizeof(AmiNetStack) is what running the stack costs a machine for"
    echo "  as long as it is up.  Either take the bytes back out, or raise the"
    echo "  budget in tools/check-ram-size.sh in the same commit and say in the"
    echo "  message what bought them."
    exit 1
fi

echo "ram_size=PASS arm=$ARM bytes=$bytes budget=$budget spare=$((budget - bytes))"
