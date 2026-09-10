#!/usr/bin/env python3
"""Every shipping drawer is declared once, and every consumer uses that one.

    tools/check-shipping-config.sh

The full stack, `minimal` and `micro` are declared in CMakePresets.json and
nowhere else.  tools/ci.sh compiles them with warnings fatal,
.github/workflows/release.yml builds the trees the archive is packed from, and
dist/make-dist.sh packs them.  All three read the preset.

THIS REPLACED A COMPARISON OF HAND-COPIES.  The option lists used to be written
out in each of those three files; this script compared two of them by regex and
had no idea the third existed.  So `micro` was added to ci.sh and make-dist.sh,
release.yml never learned about it, every cross arm passed, and 0.26.6's
release died at the last step with `missing build: build/release-micro`.

What is checked now:

  declared    each shipping drawer is a configure preset
  built       release.yml builds every non-default preset it must pack
  derived     ci.sh and make-dist.sh take their options from the preset
  unique      no consumer carries its own -DAMINETXDUO_ list for a drawer

Output is key=value plus an exit code.

SPDX-License-Identifier: MIT
"""

import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The drawers dist/make-dist.sh packs.  `default` is the top of Libs: and is
# configured by ci.sh's own cross stage, so release.yml builds it as `default`
# rather than as build/release-<name>.
PACKED = ["minimal", "micro"]

errors = 0


def say(key, value):
    print("%s=%s" % (key, value))


def fail(key, value):
    global errors
    errors += 1
    print("%s=%s" % (key, value))


def read(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8") as fh:
        return fh.read()


# ------------------------------------------------------------- declared ----

try:
    presets = json.loads(read("CMakePresets.json"))["configurePresets"]
except (OSError, ValueError, KeyError) as exc:
    print("shipping_config=FAIL no_usable_CMakePresets.json (%s)" % exc)
    raise SystemExit(1)

names = {p["name"] for p in presets if not p.get("hidden")}
for drawer in ["default"] + PACKED:
    if drawer in names:
        say("declared_%s" % drawer, "CMakePresets.json")
    else:
        fail("declared_%s" % drawer, "MISSING_from_CMakePresets.json")

# ---------------------------------------------------------------- built ----

release = read(".github/workflows/release.yml")
for drawer in PACKED:
    if re.search(r"cmake --preset %s\b" % re.escape(drawer), release):
        say("built_%s" % drawer, "release.yml")
    else:
        fail("built_%s" % drawer,
             "release.yml_does_not_build_it -- dist/make-dist.sh will stop at "
             "`missing build`")

# -------------------------------------------------------------- derived ----

ci = read("tools/ci.sh")
dist = read("dist/make-dist.sh")

for drawer in PACKED:
    if re.search(r'"%s:\$\("\$ROOT/tools/preset-options\.sh" %s\)"'
                 % (re.escape(drawer), re.escape(drawer)), ci):
        say("derived_ci_%s" % drawer, "preset-options.sh")
    else:
        fail("derived_ci_%s" % drawer, "tools/ci.sh_does_not_read_the_preset")

    var = "%s_OPTIONS" % drawer.upper()
    if re.search(r'%s="\$\("\$ROOT/tools/preset-options\.sh" %s\)"'
                 % (var, re.escape(drawer)), dist):
        say("derived_dist_%s" % drawer, "preset-options.sh")
    else:
        fail("derived_dist_%s" % drawer, "dist/make-dist.sh_does_not_read_the_preset")

# --------------------------------------------------------------- unique ----
#
# A shipping drawer carrying its own -D list is the shape that rotted.  The
# coverage arms in ci.sh (instr, noinline, tcpextra, pathswap and the rest)
# legitimately own theirs -- they exist to compile one option's other side and
# are not packed into anything -- so only the drawer names are checked.

for drawer in PACKED:
    hand = []

    if re.search(r'"%s:-D' % re.escape(drawer), ci):
        hand.append("tools/ci.sh")
    if re.search(r'%s_OPTIONS="-D' % drawer.upper(), dist):
        hand.append("dist/make-dist.sh")
    if re.search(r'-B build/release-%s\b[^\n]*(?:\\\n[^\n]*)*-DAMINETXDUO_'
                 % re.escape(drawer), release):
        hand.append(".github/workflows/release.yml")

    if hand:
        fail("unique_%s" % drawer,
             "hand_written_option_list_in_" + ",".join(hand))
    else:
        say("unique_%s" % drawer, "declared_only_in_CMakePresets.json")

say("shipping_config_errors", errors)
say("shipping_config", "PASS" if errors == 0 else "FAIL")
raise SystemExit(0 if errors == 0 else 1)
