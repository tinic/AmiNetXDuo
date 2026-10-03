#!/usr/bin/env python3
"""Remove DWARF sections from the compiler runtime archives of a toolchain.

    tools/fix-toolchain-dwarf.py <toolchain-root> [--check]

libgcc.a and libgcov.a are built with -g. binutils 2.39's amiga back end
writes a linked member's .debug_* sections as loadable hunks and then emits
no HUNK_RELOC32 at all: an image that pulls _udivdi3.o (any newlib printf
built at -Os does) loads unrelocated and jumps through link-time addresses.
tools/check-hunk-relocs.sh catches that in images this tree builds; this
removes the cause from the toolchain, so a plain program links correctly.

Only .debug_* sections go. Code, data, relocations, symbols and .stab are
untouched, and member order is kept. --check fails on any .debug_* section
in any target archive or object under the prefix, not only these two.
"""

import pathlib
import shutil
import subprocess
import sys
import tempfile

STRIP_ARCHIVES = ("libgcc.a", "libgcov.a")


def tool(prefix, name):
    return str(prefix / "bin" / f"m68k-amigaos-{name}")


def debug_sections(prefix, obj):
    out = subprocess.run(
        [tool(prefix, "objdump"), "-h", str(obj)],
        check=True, capture_output=True, text=True,
    ).stdout
    found = []
    for line in out.splitlines():
        fields = line.split()
        if len(fields) > 1 and fields[0].isdigit() and fields[1].startswith(".debug"):
            found.append(fields[1])
    return found


def members(prefix, archive):
    return subprocess.run(
        [tool(prefix, "ar"), "t", str(archive)],
        check=True, capture_output=True, text=True,
    ).stdout.split()


def archives(prefix, names=None):
    roots = [prefix / "lib/gcc/m68k-amigaos", prefix / "m68k-amigaos/lib",
             prefix / "m68k-amigaos/libnix"]
    for root in roots:
        if not root.is_dir():
            continue
        for path in sorted(root.rglob("*.a")):
            if not path.is_symlink() and (names is None or path.name in names):
                yield path


def strip(prefix):
    total = 0
    for archive in archives(prefix, STRIP_ARCHIVES):
        names = members(prefix, archive)
        if len(set(names)) != len(names):
            print(f"!! {archive}: duplicate member names", file=sys.stderr)
            return 1
        with tempfile.TemporaryDirectory() as tmp:
            subprocess.run([tool(prefix, "ar"), "x", str(archive)], cwd=tmp, check=True)
            changed = 0
            for name in names:
                obj = pathlib.Path(tmp) / name
                secs = debug_sections(prefix, obj)
                if secs:
                    args = [tool(prefix, "objcopy")]
                    for sec in secs:
                        args += ["-R", sec]
                    subprocess.run(args + [str(obj)], check=True)
                    changed += 1
            if changed:
                new = pathlib.Path(tmp) / "new.a"
                subprocess.run([tool(prefix, "ar"), "rcs", str(new)] + names,
                               cwd=tmp, check=True)
                shutil.copyfile(new, archive)
            total += changed
            print(f"  {archive.relative_to(prefix)}: {changed}/{len(names)} members stripped")
    print(f"fix-toolchain-dwarf: {total} member(s) stripped")
    return 0


def archive_debug_members(prefix, archive):
    """Members of one archive carrying .debug_* sections, from one objdump."""
    run = subprocess.run(
        [tool(prefix, "objdump"), "-h", str(archive)],
        check=False, capture_output=True, text=True,
    )
    if run.returncode:
        # objdump refuses some archives whole (libatomic.a); go member by member.
        with tempfile.TemporaryDirectory() as tmp:
            subprocess.run([tool(prefix, "ar"), "x", str(archive)], cwd=tmp, check=True)
            objs = sorted(pathlib.Path(tmp).iterdir())
            return [o.name for o in objs if debug_sections(prefix, o)], len(objs)
    out = run.stdout
    member, bad, count = None, set(), 0
    for line in out.splitlines():
        if line.endswith("file format amiga"):
            member = line.split(":", 1)[0]
            count += 1
            continue
        fields = line.split()
        if member and len(fields) > 1 and fields[0].isdigit() and fields[1].startswith(".debug"):
            bad.add(member)
    return sorted(bad), count


def check(prefix):
    bad = []
    examined = 0
    for archive in archives(prefix):
        found, count = archive_debug_members(prefix, archive)
        examined += count
        bad += [f"{archive.relative_to(prefix)}({m})" for m in found]
    for root in (prefix / "lib/gcc/m68k-amigaos", prefix / "m68k-amigaos/lib"):
        for obj in sorted(root.rglob("*.o")) if root.is_dir() else ():
            examined += 1
            if debug_sections(prefix, obj):
                bad.append(str(obj.relative_to(prefix)))
    if bad:
        print(f"toolchain_dwarf=fail members={len(bad)} examined={examined}")
        for item in bad[:20]:
            print(f"  {item}")
        return 1
    print(f"toolchain_dwarf=clean examined={examined}")
    return 0


def main():
    args = sys.argv[1:]
    want_check = "--check" in args
    args = [a for a in args if a != "--check"]
    if len(args) != 1:
        print(f"usage: {sys.argv[0]} <toolchain-root> [--check]", file=sys.stderr)
        return 2
    prefix = pathlib.Path(args[0]).resolve()
    if not (prefix / "bin/m68k-amigaos-objcopy").is_file():
        print(f"no m68k-amigaos binutils under {prefix}", file=sys.stderr)
        return 2
    return check(prefix) if want_check else strip(prefix)


if __name__ == "__main__":
    sys.exit(main())
