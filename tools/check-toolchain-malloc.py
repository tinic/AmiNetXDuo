#!/usr/bin/env python3
"""Verify the pinned newlib allocator in every installed toolchain multilib.

The 16.2.2 packages shipped a bootstrap libc despite pinning fixed newlib
source. For this immutable asset series, compare installed malloc.o bytes
against the known-good newlib build, in both libc.a and libg.a. A future
source, compiler, or flag change must deliberately update this map and its
runtime Enforcer proof; unknown bytes fail closed.
"""

import hashlib
import pathlib
import subprocess
import sys


# Paths are relative to <prefix>/m68k-amigaos/lib. The paired libc/libg
# archive members must match for each variant. Eight distinct hashes cover
# eleven multilibs produced by the pinned newlib source's final make step.
EXPECTED_MALLOC_SHA256 = {
    "": "0f2522d271d7094be4c9dd974e24634d13b6af150fd48fc8de89caf1ab4f2c22",
    "libm020": "1531c3f1489f4d1a19b53d69e3fcb3a17abbce21c49216151c323dbdb7e00144",
    "libm020/libm881": "1531c3f1489f4d1a19b53d69e3fcb3a17abbce21c49216151c323dbdb7e00144",
    "libm060": "a60a7c7e936c87f661d7b23430d7f908d5b902c1131dbf5eae792a54a94c4e91",
    "libb": "0085a784fa1bbe184f322df4ae1b48413a0c421ca1daf570d491c4fcc8798316",
    "libb/libm020": "af379f9fbe7da3bab6a5476c094147415e55ea7e9c491d4d94676f17b145e1b1",
    "libb/libm020/libm881": "af379f9fbe7da3bab6a5476c094147415e55ea7e9c491d4d94676f17b145e1b1",
    "libb/libm060": "c486aa7719ae6bedd46d82a37609951019df6d29ad135558c10a5c2285347a3b",
    "libb32/libm020": "07c5767e7ec8e7dd31c3649cab559b9cb5a498743cb3976ff9b5adda352352a2",
    "libb32/libm020/libm881": "07c5767e7ec8e7dd31c3649cab559b9cb5a498743cb3976ff9b5adda352352a2",
    "libb32/libm060": "f7f63dad7cb7ff955678a50a2f778ce46a8988ee2c9ed683e17cf6dd98b5f188",
}


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <toolchain-prefix>", file=sys.stderr)
        return 2
    prefix = pathlib.Path(sys.argv[1]).resolve()
    ar = prefix / "bin/m68k-amigaos-ar"
    gcc = prefix / "bin/m68k-amigaos-gcc"
    libdir = prefix / "m68k-amigaos/lib"
    if not ar.is_file() or not gcc.is_file() or not libdir.is_dir():
        print(f"missing cross-gcc, cross-ar or library directory under {prefix}", file=sys.stderr)
        return 1

    failures = []
    multilibs = subprocess.run(
        [str(gcc), "-print-multi-lib"], check=False, capture_output=True, text=True
    )
    if multilibs.returncode:
        print("cross-gcc could not enumerate multilibs", file=sys.stderr)
        return 1
    compiler_variants = set()
    for line in multilibs.stdout.splitlines():
        if line.strip():
            variant = line.split(";", 1)[0]
            compiler_variants.add("" if variant == "." else variant)
    if compiler_variants != set(EXPECTED_MALLOC_SHA256):
        failures.append(
            "compiler multilib paths differ from pinned allocator map: "
            f"{sorted(compiler_variants)}"
        )

    for archive_name in ("libc.a", "libg.a"):
        expected = {
            str(pathlib.PurePosixPath(variant) / archive_name)
            for variant in EXPECTED_MALLOC_SHA256
        }
        actual = {
            archive.relative_to(libdir).as_posix()
            for archive in libdir.rglob(archive_name)
        }
        for extra in sorted(actual - expected):
            failures.append(f"m68k-amigaos/lib/{extra}: unexpected unverified multilib")
    for variant, want in EXPECTED_MALLOC_SHA256.items():
        for companion in ("crt0.o", "libm.a"):
            path = libdir / variant / companion
            if not path.is_file():
                failures.append(f"{path.relative_to(prefix)}: missing multilib companion")
        for archive_name in ("libc.a", "libg.a"):
            archive = libdir / variant / archive_name
            if not archive.is_file():
                failures.append(f"{archive.relative_to(prefix)}: missing archive")
                continue
            result = subprocess.run(
                [str(ar), "p", str(archive), "malloc.o"],
                check=False,
                capture_output=True,
            )
            if result.returncode or not result.stdout:
                failures.append(f"{archive.relative_to(prefix)}: missing malloc.o")
                continue
            got = hashlib.sha256(result.stdout).hexdigest()
            if got != want:
                failures.append(
                    f"{archive.relative_to(prefix)}: malloc.o {got}, want {want}"
                )

    if failures:
        print("toolchain allocator verification FAILED:", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1
    print("toolchain allocator verification passed: 11 libc and 11 libg multilibs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
