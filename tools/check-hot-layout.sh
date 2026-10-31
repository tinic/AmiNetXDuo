#!/usr/bin/env bash
#
# bsdsocket.library's hot functions sit first in the code hunk, in the order
# of src/bsdsocket/hot-functions.txt, contiguous, each on a 16-byte line.
#
#   tools/check-hot-layout.sh [--complete] <build-dir>
#
# Under -flto the program is cut into size-balanced partitions, so a change in
# code that never runs moved the hot functions across cache lines: A3000 upload
# moved 1-2% between builds for that alone (#396, #398).  The link pins them
# (src/bsdsocket/CMakeLists.txt, hot-order.ld); this reads what the linker
# actually did.
#
# Read from the link map, and the map is tied to the binary: the code hunk
# size in the HUNK_HEADER must equal the map's .text, and with symbols kept
# every hot global's address in the image must equal the map's.
#
# --complete: every listed entry must be in the image.  The default and
# symbols arms are built with every option the list was taken from; other arms
# may lack a function (an option off, -O2 inlining it), and then only the
# entries that are present are checked.
#
# Output is key=value.  Exit 0 pass or skipped, 1 a layout or list defect.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

COMPLETE=0
if [ "${1:-}" = "--complete" ]; then
    COMPLETE=1
    shift
fi
BUILD="${1:?build directory}"

LIST="$ROOT/src/bsdsocket/hot-functions.txt"
LIB="$BUILD/src/bsdsocket/bsdsocket.library"
MAP="$LIB.map"
CACHE="$BUILD/CMakeCache.txt"

if [ ! -r "$CACHE" ]; then
    echo "hot_layout=skipped reason=no_cmake_cache build=$BUILD"
    exit 0
fi
if ! grep -q '^CMAKE_TOOLCHAIN_FILE:.*m68k-amigaos' "$CACHE"; then
    echo "hot_layout=skipped reason=not_cross build=$BUILD"
    exit 0
fi
if [ ! -f "$LIB" ]; then
    echo "hot_layout=skipped reason=no_library build=$BUILD"
    exit 0
fi
# The per-function sections come from the LTRANS units, so an LTO=OFF link
# has none to order.
if grep -q '^AMINETXDUO_LTO:BOOL=OFF' "$CACHE"; then
    echo "hot_layout=skipped reason=lto_off build=$BUILD"
    exit 0
fi
if [ ! -r "$MAP" ]; then
    echo "hot_layout=FAIL reason=no_map map=$MAP"
    exit 1
fi

NM=""
if grep -q '^AMINETXDUO_KEEP_SYMBOLS:BOOL=ON' "$CACHE"; then
    NM=$(sed -n 's/^CMAKE_NM:FILEPATH=//p' "$CACHE")
    if [ -z "$NM" ] || [ ! -x "$NM" ]; then
        echo "hot_layout=FAIL reason=no_nm cache=$CACHE"
        exit 1
    fi
fi

python3 - "$LIST" "$MAP" "$LIB" "$COMPLETE" "$NM" <<'PY'
import re, struct, subprocess, sys

list_path, map_path, lib_path, complete, nm = sys.argv[1:6]
complete = complete == "1"
ALIGN = 16
ENTRY = ".text.bsdsocket_entry"

def fail(reason, **kv):
    extra = "".join(" %s=%s" % (k, v) for k, v in kv.items())
    print("hot_layout=FAIL reason=%s%s" % (reason, extra))
    sys.exit(1)

entries = []
for n, line in enumerate(open(list_path), 1):
    line = line.split("#", 1)[0].strip()
    if not line:
        continue
    if re.fullmatch(r"@[A-Za-z0-9_.+-]+", line):
        entries.append(("file", line[1:]))
    elif re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", line):
        entries.append(("fn", line))
    else:
        fail("bad_list_entry", line=n)
if not entries:
    fail("empty_list")
if len(set(entries)) != len(entries):
    fail("duplicate_list_entry")

def matches(entry, rec):
    kind, name = entry
    sec, _, _, obj = rec
    if kind == "fn":
        return sec == ".text." + name or sec.startswith(".text." + name + ".")
    return sec == ".text" and (obj.endswith("(" + name + ")") or
                               obj.endswith("/" + name) or obj == name)

# The .text output section's input sections, in address order.
recs, syms = [], {}
lines = open(map_path, errors="replace").read().split("\n")
start = next((i for i, l in enumerate(lines) if re.match(r"^\.text\s+0x", l)), None)
if start is None:
    fail("no_text_section", map=map_path)
text_size = int(lines[start].split()[2], 16)
pending = None
for l in lines[start + 1:]:
    if l and not l[0].isspace():
        break
    m = re.match(r"^ \*fill\*\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)", l)
    if m:
        recs.append(("*fill*", int(m.group(1), 16), int(m.group(2), 16), ""))
        pending = None
        continue
    m = re.match(r"^ (\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$", l)
    if m and not m.group(1).startswith("*("):
        recs.append((m.group(1), int(m.group(2), 16), int(m.group(3), 16),
                     m.group(4).strip()))
        pending = None
        continue
    m = re.match(r"^ (\.text\S*)\s*$", l)
    if m:
        pending = m.group(1)
        continue
    m = re.match(r"^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$", l)
    if m and pending:
        recs.append((pending, int(m.group(1), 16), int(m.group(2), 16),
                     m.group(3).strip()))
        pending = None
        continue
    m = re.match(r"^\s+0x([0-9a-f]+)\s+([A-Za-z_.$][\w.$]*)\s*$", l)
    if m and recs:
        syms.setdefault(len(recs) - 1, []).append((m.group(2), int(m.group(1), 16)))
    pending = None

fills = [r for r in recs if r[0] == "*fill*"]
code = [(i, r) for i, r in enumerate(recs) if r[0] != "*fill*" and r[2] > 0]
if not code:
    fail("no_input_sections", map=map_path)

# The map belongs to this binary: code hunk size from the HUNK_HEADER.
d = open(lib_path, "rb").read()
if struct.unpack_from(">I", d, 0)[0] != 0x3F3:
    fail("not_a_hunk_file", lib=lib_path)
p = 4
while True:
    n = struct.unpack_from(">I", d, p)[0]; p += 4
    if n == 0:
        break
    p += 4 * n
_, first, last = struct.unpack_from(">III", d, p); p += 12
hunk0 = (struct.unpack_from(">I", d, p)[0] & 0x3FFFFFFF) * 4
if hunk0 != (text_size + 3) & ~3:
    fail("map_not_this_binary", hunk0=hunk0, map_text=text_size)

i0, r0 = code[0]
if r0[0] != ENTRY or r0[1] != 0:
    fail("entry_not_first", first=r0[0], addr="0x%x" % r0[1])

pos = 1
placed, absent, prev_end = 0, [], r0[1] + r0[2]
block = []
for e in entries:
    got = []
    while pos < len(code) and matches(e, code[pos][1]):
        got.append(code[pos]); pos += 1
    if not got:
        elsewhere = [r for _, r in code if matches(e, r)]
        if elsewhere:
            fail("out_of_order", entry=e[1], found=elsewhere[0][0],
                 addr="0x%x" % elsewhere[0][1],
                 expected_after=(block[-1][1][0] if block else ENTRY))
        absent.append(e[1])
        continue
    for k, (idx, r) in enumerate(got):
        gap = r[1] - prev_end
        if gap < 0 or gap >= ALIGN:
            fail("not_contiguous", entry=e[1], section=r[0], addr="0x%x" % r[1],
                 gap=gap)
        if k == 0 and r[1] % ALIGN:
            fail("misaligned", entry=e[1], section=r[0], addr="0x%x" % r[1])
        prev_end = r[1] + r[2]
        block.append((idx, r))
    placed += 1

for _, r in code[pos:]:
    for e in entries:
        if matches(e, r):
            fail("listed_section_outside_block", entry=e[1], section=r[0],
                 addr="0x%x" % r[1])

if complete and absent:
    fail("listed_but_absent", entries=",".join(absent),
         hint="inlined_or_renamed_update_src/bsdsocket/hot-functions.txt")

checked = 0
if nm:
    out = subprocess.run([nm, lib_path], capture_output=True, text=True)
    if out.returncode != 0:
        fail("nm_failed", nm=nm)
    image = {}
    for l in out.stdout.splitlines():
        f = l.split()
        if len(f) == 3 and f[1] in "tT":
            image[f[2]] = int(f[0], 16)
    for idx, r in block:
        for name, addr in syms.get(idx, []):
            sym = "_" + name        # the map drops the C prefix underscore
            if sym not in image:
                fail("symbol_not_in_image", symbol=sym)
            if image[sym] != addr:
                fail("image_differs_from_map", symbol=sym,
                     image="0x%x" % image[sym], map="0x%x" % addr)
            checked += 1
    if checked == 0:
        fail("no_symbols_checked", nm=nm)

pad = sum(r[2] for r in fills if r[1] < prev_end)
print("hot_layout=PASS entries=%d placed=%d absent=%d block_bytes=%d "
      "pad_bytes=%d symbols_checked=%d" %
      (len(entries), placed, len(absent), prev_end, pad, checked))
PY
