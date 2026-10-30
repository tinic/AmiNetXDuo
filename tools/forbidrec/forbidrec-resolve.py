#!/usr/bin/env python3
"""forbidrec's log, with our own PCs named down to the source line.

  tools/forbidrec/forbidrec-resolve.py LOG --lib name=bsdsocket.library,exe=FILE \\
      [--lib ...] [--addr2line m68k-amigaos-addr2line]

forbidrec names a PC by module, hunk and offset (or nearest LVO).  For a module
given here by --lib, the offset is looked up with addr2line in that file, which
must be a build of the same source with -g and its symbols kept: the run's hunk
sizes are not in the log, so the caller vouches for the match (compare the
code hunk against the file the machine loaded first).

The PC is a return address, so the line is looked up at PC - 2: inside the
jsr, not at the instruction after it.

Adds `<key>_func=` and `<key>_src=` to each hold, max and unmatched line, and
prints one `unseen` line per sampler forbid run that no recorded hold covers.

SPDX-License-Identifier: MIT
"""

import argparse
import re
import subprocess

PC = re.compile(r' (pc[01]?)_in="([^"]*)" \1_hunk=(\d+) \1_off=0x([0-9a-f]+)')
FIELD = re.compile(r'(\w+)=("[^"]*"|\S+)')


def addr2line(tool, exe, offs):
    if not offs:
        return {}
    out = subprocess.run([tool, "-f", "-C", "-e", exe] + ["0x%x" % o for o in offs],
                         capture_output=True, text=True, check=False).stdout.splitlines()
    res = {}
    for i, o in enumerate(offs):
        func = out[2 * i] if 2 * i < len(out) else "?"
        src = out[2 * i + 1] if 2 * i + 1 < len(out) else "?"
        res[o] = (func, re.sub(r".*/(src|port|third_party)/", r"\1/", src))
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--lib", action="append", default=[])
    ap.add_argument("--addr2line", default="m68k-amigaos-addr2line")
    a = ap.parse_args()

    libs = {}
    for spec in a.lib:
        kv = dict(f.split("=", 1) for f in spec.split(","))
        libs[kv["name"]] = kv["exe"]

    lines = open(a.log, errors="replace").read().splitlines()
    want = {}
    for ln in lines:
        for m in PC.finditer(ln):
            if m.group(2) in libs and m.group(3) == "0":
                want.setdefault(m.group(2), set()).add(max(int(m.group(4), 16) - 2, 0))
    names = {}
    for lib, offs in want.items():
        for o, v in addr2line(a.addr2line, libs[lib], sorted(offs)).items():
            names[(lib, o)] = v

    for ln in lines:
        def add(m):
            key = (m.group(2), max(int(m.group(4), 16) - 2, 0))
            if key in names and m.group(3) == "0":
                f, s = names[key]
                return m.group(0) + ' %s_func=%s %s_src=%s' % (m.group(1), f, m.group(1), s)
            return m.group(0)
        out = PC.sub(add, ln)
        print(out)

    # Sampler runs that no recorded hold of the same task overlaps.  A
    # phase's sample lines follow its hold lines, and t_ms counts from the
    # start of the phase.
    phase = None
    rec = {}
    for ln in lines:
        f = dict(FIELD.findall(ln))
        if ln.startswith("phase="):
            phase = f["phase"]
        elif ln.startswith("hold ") and f.get("kind") == "forbid":
            t0 = int(f["t_ms"])
            rec.setdefault((phase, f["task"]), []).append(
                (t0, t0 + int(f["dur_us"]) // 1000 + 1))
        elif ln.startswith("sample ") and f.get("kind") == "forbid_run":
            t0 = int(f["t_ms"])
            t1 = t0 + int(f["dur_us"]) // 1000 + 1
            spans = rec.get((phase, f["task"]), [])
            if not any(s0 <= t1 and e0 >= t0 for s0, e0 in spans):
                print("unseen phase=%s %s" % (phase, ln[len("sample "):]))


main()
