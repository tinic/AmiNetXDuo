#!/usr/bin/env python3
"""Check deferred branches and indexed PC8 fields with installed tools."""

import pathlib
import struct
import subprocess
import sys
import tempfile


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: toolchain-branch-selftest.py <toolchain-root>")
    root = pathlib.Path(sys.argv[1]).resolve()
    assembler = root / "bin/m68k-amigaos-as"
    linker = root / "bin/m68k-amigaos-ld"
    objcopy = root / "bin/m68k-amigaos-objcopy"

    def run(args):
        return subprocess.run(args, capture_output=True, text=True, check=False)

    with tempfile.TemporaryDirectory(prefix="anxd-byte-branch-") as tmp:
        work = pathlib.Path(tmp)
        valid = work / "valid.s"
        valid.write_text(""".text
.space 32,0
.globl entry
entry:
 bra.s .Llocal
 nop
.Llocal:
 bra.w target
 bsr.w .Lzero
 bra.w .Lzero
 rts
.weak target
target:
 rts
.section .text.zero,\"ax\"
.Lzero:
 rts
""")
        indexed = work / "indexed.s"
        lines = [".text", ".space 32,0", ".globl entry", "entry:"]
        aliases = []
        expected = bytearray(32)
        for address_register in (False, True):
            for reg in range(8):
                for long_index in (False, True):
                    for displacement in (0, -1):
                        symbol = f"index_{len(aliases)}"
                        offset = len(expected) - 32 + 2 + displacement - 16
                        name = ("a" if address_register else "d") + str(reg)
                        size = "l" if long_index else "w"
                        lines.append(f" lea (({symbol}+16).b,pc,{name}.{size}),a0")
                        aliases.append(f".weak {symbol}\n{symbol} = entry+{offset}")
                        extension = (0x8000 if address_register else 0) | reg << 12
                        extension |= (0x800 if long_index else 0) | (displacement & 0xff)
                        expected.extend(struct.pack(">HH", 0x41fb, extension))
        indexed.write_text("\n".join(lines + [" rts"] + aliases) + "\n")
        expected.extend(b"\x4e\x75")
        for source in (valid, indexed):
            obj = source.with_suffix(".o")
            assembled = run([str(assembler), "-m68020", str(source), "-o", str(obj)])
            if assembled.returncode:
                raise SystemExit("valid PC8 assembly failed: " + assembled.stderr)
            linked = run([str(linker), "-e", "entry", str(obj), "-o", str(source.with_suffix(""))])
            if linked.returncode:
                raise SystemExit("valid PC8 link failed: " + linked.stderr)
        binary = work / "indexed.bin"
        copied = run([str(objcopy), "-O", "binary", "-j", ".text", str(work / "indexed"), str(binary)])
        if copied.returncode:
            raise SystemExit(copied.stderr)
        actual = binary.read_bytes()
        if actual[:len(expected)] != expected or len(actual) - len(expected) not in range(4) or any(actual[len(expected):]):
            raise SystemExit("indexed PC8 fields changed bytes")
        # These displacements are known at assembly time. The assembler has
        # instruction identity; the generic HUNK PC8 linker relocation does not.
        for displacement, operand in [(0, ".+2"), (-1, ".+1")]:
            source = work / f"invalid{displacement}.s"
            source.write_text(f".text\n bra.s {operand}\n rts\n")
            bad = run([str(assembler), "-m68020", str(source), "-o", str(source.with_suffix(".o"))])
            if bad.returncode == 0 or "invalid byte branch offset" not in bad.stderr:
                raise SystemExit(f"resolved invalid branch {displacement} was not rejected: " + bad.stderr)
    print("toolchain_branch_selftest=PASS safe_branch_link=1 indexed_fields=64 byte_exact=yes resolved_invalid=0,-1")
    return 0


if __name__ == "__main__":
    sys.exit(main())
