#!/usr/bin/env python3
"""Check deferred byte branches using the installed assembler and linker."""

import pathlib
import subprocess
import sys
import tempfile


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: toolchain-branch-selftest.py <toolchain-root>")
    root = pathlib.Path(sys.argv[1]).resolve()
    assembler = root / "bin/m68k-amigaos-as"
    linker = root / "bin/m68k-amigaos-ld"
    with tempfile.TemporaryDirectory(prefix="anxd-byte-branch-") as tmp:
        work = pathlib.Path(tmp)
        valid = work / "valid.s"
        valid.write_text(""".text
.space 12,0
.globl entry
entry:
 bra.s target
 bsr.s .Lzero
 bra.s .Lzero
 rts
.space 12,0
.weak target
target:
 rts
.section .text.zero,\"ax\"
.Lzero:
 rts
""")
        invalid = work / "invalid.s"
        invalid.write_text(""".text
.space 12,0
.globl entry
entry:
 bra.s next_insn
.weak next_insn
next_insn:
 bra.s own_field+1
.weak own_field
own_field = next_insn
 rts
""")
        for source in (valid, invalid):
            result = subprocess.run(
                [str(assembler), "-m68020", str(source), "-o", str(source.with_suffix(".o"))],
                capture_output=True, text=True, check=False,
            )
            if result.returncode:
                raise SystemExit("deferred branch assembly failed: " + result.stderr)
        good = subprocess.run(
            [str(linker), "-e", "entry", str(valid.with_suffix(".o")), "-o", str(work / "valid")],
            capture_output=True, text=True, check=False,
        )
        if good.returncode:
            raise SystemExit("valid deferred branch link failed: " + good.stderr)
        bad = subprocess.run(
            [str(linker), "-e", "entry", str(invalid.with_suffix(".o")), "-o", str(work / "invalid")],
            capture_output=True, text=True, check=False,
        )
        if bad.returncode == 0 or any(
            f"byte branch displacement {value} cannot be encoded" not in bad.stderr
            for value in (0, -1)
        ):
            raise SystemExit("invalid final branch encodings were not both rejected: " + bad.stderr)
    print("toolchain_branch_selftest=PASS deferred_assembly=2 valid_link=1 rejected_displacements=0,-1")
    return 0


if __name__ == "__main__":
    sys.exit(main())
