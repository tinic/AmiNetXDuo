#!/usr/bin/env python3
"""Copy and repair one client crt0.o with the tree-wide semantic gate.

Ported clients link a private crt0.o because they may be built with an
externally supplied toolchain rather than the pinned, already-repaired one.
There must not be a second byte-pattern implementation here: that is how the
upstream issue #8 change fixed the argv call while introducing an unchecked
write through address zero.

The authoritative repair in tools/fix-toolchain-crt0.py understands every
multilib addressing mode in the pinned toolchain and verifies three separate
properties: balanced entry/exit frames, argv passed to main by value, and real
storage behind both startup writes. This wrapper gives it a one-object tree,
runs the repair, and then runs its fail-closed check over the result.

    usage: fix-crt0.py <input crt0.o> <output crt0.o>

SPDX-License-Identifier: MIT
"""

import os
import pathlib
import shutil
import subprocess
import sys


def main(argv):
    if len(argv) != 3:
        sys.stderr.write("usage: fix-crt0.py <in crt0.o> <out crt0.o>\n")
        return 2

    source = pathlib.Path(argv[1]).resolve()
    output = pathlib.Path(argv[2]).resolve()
    if not source.is_file():
        sys.stderr.write(f"fix-crt0: no such input object: {source}\n")
        return 2
    if source == output:
        sys.stderr.write("fix-crt0: input and output must be different files\n")
        return 2

    output.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, output)

    root = pathlib.Path(__file__).resolve().parents[2]
    repair = root / "tools" / "fix-toolchain-crt0.py"
    env = os.environ.copy()

    repaired = subprocess.run([sys.executable, str(repair), str(output.parent)],
                              env=env)
    if repaired.returncode != 0:
        output.unlink(missing_ok=True)
        return repaired.returncode

    checked = subprocess.run([sys.executable, str(repair), str(output.parent),
                              "--check"], env=env)
    if checked.returncode != 0:
        output.unlink(missing_ok=True)
        return checked.returncode
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
