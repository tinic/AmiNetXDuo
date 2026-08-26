#!/usr/bin/env python3
"""Every build option the root CMakeLists declares is compiled in both states.

    tools/check-option-coverage.sh

`option(AMINETXDUO_X ... ON)` means the `default` arm compiles the ON side and
nothing at all compiles the OFF side unless a tools/ci.sh CROSS_CONFIGS arm
names it.  An option no arm names is code that has stopped being compiled, and
the tree has been here before: AMINETXDUO_TCP_SACK=OFF and
AMINETXDUO_RX_VERIFY=OFF each did not build at the moment an arm was first
pointed at them, and AMINETXDUO_RXPROBE=ON was a -Werror=sign-compare failure
found only by building it by hand.  A declared option is a promise to a user
who passes -D; a promise nothing compiles is a promise already broken.

An option is COVERED when some arm sets it to the value opposite its declared
default.  The default side needs no arm: `default` is that arm.

ALLOWLIST below is the explicit set that no cross arm may reach, each with the
reason it cannot.  "Not covered yet" is not a reason.  An allowlist entry that
IS covered fails too, so the list cannot rot.

Output is key=value plus an exit code.

SPDX-License-Identifier: MIT
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# name -> why no CROSS_CONFIGS arm can carry it.  A reason names the mechanism.
ALLOWLIST = {
    "AMINETXDUO_SANITIZE":
        "host tier, not cross: it selects ASan/UBSan on the x86 test build and"
        " the m68k toolchain has no sanitizer runtime.  tools/ci.sh's"
        " `sanitize` stage is its arm",
}


def say(k, v):
    print("%s=%s" % (k, v))


def read(path):
    with open(os.path.join(ROOT, path), errors="replace") as fh:
        return fh.read()


def uncommented(text):
    return "\n".join(l for l in text.splitlines() if not l.lstrip().startswith("#"))


def declared_options():
    """{name: default} for every option() in the ROOT CMakeLists.

    Sub-directory lists declare their own and those arms exist (noasm,
    nonet68kasm); this gate is the root's surface only."""
    text = uncommented(read("CMakeLists.txt"))
    out = {}
    for m in re.finditer(
            r'option\(\s*(AMINETXDUO_[A-Z0-9_]+)\s+"[^"]*"\s+(\S+?)\s*\)',
            text, re.S):
        out[m.group(1)] = m.group(2).strip()
    return out


def cross_configs():
    """{arm: {OPTION: VALUE}} out of tools/ci.sh's CROSS_CONFIGS array."""
    block = re.search(r"CROSS_CONFIGS=\((.*?)\n\)", read("tools/ci.sh"), re.S)
    if not block:
        return None
    out = {}
    for line in uncommented(block.group(1)).splitlines():
        line = line.strip().strip('"')
        if not line or ":" not in line:
            continue
        name, opts = line.split(":", 1)
        out[name] = dict(re.findall(r"-D(AMINETXDUO_[A-Z0-9_]+)=(\w+)", opts))
    return out


def flipped(default, value):
    """True when `value` is the other side of `default`.

    A default that is a ${variable} has no fixed side, so any arm that names
    the option at all compiles a side `default` might not."""
    if default.startswith("${"):
        return True
    on = ("ON", "TRUE", "YES", "1")
    return (default.upper() in on) != (value.upper() in on)


def main():
    opts = declared_options()
    arms = cross_configs()
    if arms is None:
        say("option_coverage", "FAIL")
        say("error", "no_CROSS_CONFIGS_in_tools/ci.sh")
        return 1
    if not opts:
        say("option_coverage", "FAIL")
        say("error", "no_options_in_CMakeLists.txt")
        return 1

    bad = 0
    covered = {}
    for name, default in sorted(opts.items()):
        by = [a for a, o in sorted(arms.items())
              if name in o and flipped(default, o[name])]
        if by:
            covered[name] = by

    for name in sorted(opts):
        if name in covered:
            if name in ALLOWLIST:
                say("stale_allowlist_entry", "%s covered_by=%s"
                    % (name, ",".join(covered[name])))
                bad += 1
            continue
        if name in ALLOWLIST:
            say("option_allowed", "%s why=%s" % (name, ALLOWLIST[name]))
            continue
        say("option_uncompiled", "%s default=%s no_arm_sets_the_other_side"
            % (name, opts[name]))
        bad += 1

    for name in sorted(ALLOWLIST):
        if name not in opts:
            say("stale_allowlist_entry", "%s no_such_option" % name)
            bad += 1

    say("option_total", len(opts))
    say("option_covered", len(covered))
    say("option_allowed_count", sum(1 for n in ALLOWLIST if n in opts
                                    and n not in covered))
    say("option_coverage_errors", bad)
    say("option_coverage", "PASS" if bad == 0 else "FAIL")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
