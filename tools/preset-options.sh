#!/usr/bin/env bash
#
# The -D flags of a shipping preset, from CMakePresets.json.
#
#   tools/preset-options.sh micro
#
# ONE DECLARATION.  The three shipping drawers used to have their option lists
# written out by hand in tools/ci.sh, dist/make-dist.sh and
# .github/workflows/release.yml, and tools/check-shipping-config.sh existed to
# compare two of the three.  The third was never compared, and 0.26.6's release
# failed at the archive step because the micro drawer had never been added to
# it.  CMakePresets.json is the declaration now; this is how a shell caller
# reads it.
#
# Callers that want CMake to do the work should use `cmake --preset <name>`
# instead, and pass -B to put the build where they want it.  This exists for
# the callers that still assemble a command line.
#
# SPDX-License-Identifier: MIT

set -eu

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NAME="${1:?usage: preset-options.sh <preset>}"

exec python3 - "$ROOT/CMakePresets.json" "$NAME" <<'PY'
import json, sys

path, want = sys.argv[1], sys.argv[2]
presets = json.load(open(path))["configurePresets"]
by_name = {p["name"]: p for p in presets}

if want not in by_name:
    sys.stderr.write("preset-options: no preset '%s' in %s\n" % (want, path))
    raise SystemExit(2)

# Inherited cacheVariables first, so a child's value wins.
def collect(name, seen):
    p = by_name[name]
    out = {}
    parents = p.get("inherits", [])
    if isinstance(parents, str):
        parents = [parents]
    for parent in parents:
        out.update(collect(parent, seen))
    out.update(p.get("cacheVariables", {}))
    return out

flags = []
for key, val in sorted(collect(want, set()).items()):
    if key.startswith("CMAKE_"):
        continue                      # the toolchain and build type are the preset's own
    if isinstance(val, dict):
        val = val["value"]
    flags.append("-D%s=%s" % (key, val))

print(" ".join(flags))
PY
