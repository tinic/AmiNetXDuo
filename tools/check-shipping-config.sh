#!/usr/bin/env python3
"""Every library in the archive is built in a configuration CI compiles.

    tools/check-shipping-config.sh

dist/make-dist.sh packs four drawers -- 68000, 68000-minimal, 68020-40 and
68060 -- and .github/workflows/release.yml is what builds them.  tools/ci.sh's
CROSS_CONFIGS is what compiles configurations with warnings fatal, and its own
comment says of the minimal arm: "It must stay byte-for-byte the options
.github/workflows/release.yml gives build/release-68000-minimal."

Nothing checked that, and it is not true.  A drawer built with options no CI
arm compiles is a binary that ships having been compiled exactly once, on the
release runner, with nothing watching.

This reads the cmake invocations out of release.yml, emulator.yml's archive
step and ci.sh, and compares the option sets.  Divergences that are known and
deliberate are listed in KNOWN below with a reason; anything else fails.  A
KNOWN entry that no longer diverges also fails, so the list cannot rot.

Output is key=value plus an exit code.

SPDX-License-Identifier: MIT
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Divergences that are recorded rather than fixed, because which side is right
# is a decision about what ships and not about the test.  Key is the drawer;
# value is (the pair that disagrees, why it is here).
KNOWN = {
    "68000-minimal": (
        "release.yml vs ci.sh",
        "release.yml gives five OFF flags and ci.sh's minimal68000 arm gives "
        "seven; AMINETXDUO_AREXX and AMINETXDUO_TCPDEVICE default ON "
        "(CMakeLists.txt:150,190), so the drawer that ships carries the ARexx "
        "host and the TCP: handler and the arm that compiles it does not. "
        "make-dist.sh's own note names five options and its 225 KB figure was "
        "measured on five. emulator.yml:243 builds the archive its e2e "
        "installs with seven, so the drawer tested on a real Workbench is not "
        "the drawer that ships. Pick one.",
    ),
}

OPT = re.compile(r"-D(AMINETXDUO_[A-Z0-9_]+)=(\w+)")


def say(k, v):
    print("%s=%s" % (k, v))


def read(path):
    with open(os.path.join(ROOT, path), errors="replace") as fh:
        return fh.read()


def uncommented(text):
    """Drop whole-line comments; both YAML and shell use '#'."""
    return "\n".join(l for l in text.splitlines() if not l.lstrip().startswith("#"))


def cmake_configs(text):
    """{build-dir: {OPTION: VALUE}} for every `cmake -S . -B <dir> ...` run.

    A cmake invocation continues over backslash-joined lines, so they are
    rejoined before the options are read; reading line by line found only the
    flags that happened to be on the first line."""
    joined = re.sub(r"\\\s*\n", " ", uncommented(text))
    out = {}
    for m in re.finditer(r"cmake\s+-S\s+\.\s+-B\s+(\S+)([^\n]*)", joined):
        d, rest = m.group(1), m.group(2)
        out[d] = dict(OPT.findall(rest))
    return out


def ci_cross_configs():
    """{name: {OPTION: VALUE}} out of tools/ci.sh's CROSS_CONFIGS array."""
    text = read("tools/ci.sh")
    block = re.search(r"CROSS_CONFIGS=\((.*?)\n\)", text, re.S)
    if not block:
        return None
    out = {}
    for line in uncommented(block.group(1)).splitlines():
        line = line.strip().strip('"')
        if not line or ":" not in line:
            continue
        name, opts = line.split(":", 1)
        out[name] = dict(OPT.findall(opts))
    return out


# Which release.yml build directory feeds which archive drawer, and which
# ci.sh arm is supposed to be the same options.  From dist/make-dist.sh's
# CPU_DIRS and CPU_BUILD, whose default build root is build/cm; the release
# workflow passes -b build/release, so the suffixes are the same.
DRAWERS = [
    #  drawer            release.yml dir              ci.sh arm
    ("68020-40",        "build/release",              "default"),
    ("68000",           "build/release-68000",        "m68000"),
    ("68060",           "build/release-68060",        "m68060"),
    ("68000-minimal",   "build/release-68000-minimal", "minimal68000"),
]


def main():
    rel = cmake_configs(read(".github/workflows/release.yml"))
    ci = ci_cross_configs()
    if ci is None:
        say("shipping_config", "FAIL")
        say("error", "no_CROSS_CONFIGS_in_tools/ci.sh")
        return 1

    bad = 0
    seen_known = set()

    for drawer, reldir, arm in DRAWERS:
        # release.yml builds the three CPU drawers in a shell `for` loop whose
        # -B is "$dir", so only the minimal one appears literally.  A loop is
        # matched by its case arms instead.
        if reldir in rel:
            got = rel[reldir]
        elif drawer == "68000-minimal":
            say("drawer_%s" % drawer, "NOT_BUILT_BY_release.yml")
            bad += 1
            continue
        else:
            # The loop form. Its only per-drawer option is the CPU, which the
            # case arm sets; everything else is default, so the expected set
            # is just the CPU.
            got = {"AMINETXDUO_CPU": drawer.split("-")[0]}

        want = dict(ci.get(arm, {}))
        if arm not in ci:
            say("drawer_%s" % drawer, "no_ci.sh_arm_named_%s" % arm)
            bad += 1
            continue

        # The default arm carries no -D at all; the 68020 drawer is the same.
        if arm == "default":
            want = {}
            got = {k: v for k, v in got.items() if k != "AMINETXDUO_CPU"}

        if got == want:
            say("drawer_%s" % drawer, "matches_ci_arm_%s" % arm)
            continue

        only_rel = sorted(k for k in got if got[k] != want.get(k))
        only_ci = sorted(k for k in want if want[k] != got.get(k))
        detail = "release_only=%s ci_only=%s" % (
            ",".join(only_rel) or "-", ",".join(only_ci) or "-")

        if drawer in KNOWN:
            seen_known.add(drawer)
            say("drawer_%s" % drawer, "KNOWN_DIVERGENCE %s" % detail)
            say("drawer_%s_why" % drawer, KNOWN[drawer][1])
        else:
            say("drawer_%s" % drawer, "DIVERGES %s" % detail)
            bad += 1

    for drawer in KNOWN:
        if drawer not in seen_known:
            say("stale_known_entry", drawer)
            bad += 1

    say("shipping_config_errors", bad)
    say("shipping_config", "PASS" if bad == 0 else "FAIL")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
