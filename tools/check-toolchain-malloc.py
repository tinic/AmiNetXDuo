#!/usr/bin/env python3
"""Verify the pinned newlib allocator in every installed toolchain multilib.

The 16.2.2 packages shipped a bootstrap libc despite pinning fixed newlib
source. Compare the actual installed allocator member in both libc.a and libg.a,
using a separate map for each pinned runtime. Source, compiler or flag changes
require a reviewed map update and runtime validation; unknown bytes fail closed.
The AmigaPorts profile is covered by its newlib simulator suite, not by the
historical runtime Enforcer evidence for the legacy profile.
"""

import hashlib
import pathlib
import subprocess
import sys


# Paths are relative to <prefix>/m68k-amigaos/lib. The paired libc/libg
# archive members must match for each variant. Eight distinct hashes cover
# eleven multilibs produced by the pinned newlib source's final make step.
# Asset series 16.2.6 to 16.2.9 (GCC AmigaPorts 0edaa619 + PR #82, 1d4ca5d4, f098a8ef): each installed
# malloc.o is the object newlib's final build left in newlib/libc/sys/amigaos/,
# on Linux and macOS.  The newlib source is unchanged from 16.2.5; the bytes
# moved with the compiler.
EXPECTED_MALLOC_SHA256 = {
    "": "8108818fb83a4641aa355979fc53a5f58d784c1139d0067051ba95b50949f23a",
    "libm020": "65ec1680da2fadb21f2a863c334944903e748e930e0c24ee3a5bdc2768456111",
    "libm020/libm881": "65ec1680da2fadb21f2a863c334944903e748e930e0c24ee3a5bdc2768456111",
    "libm060": "7d6109741ec975d7748364c7e288f40553c545dd19db5e6016711845cbf86dab",
    "libb": "18417b53615ecd5bb072b62a41a60530b11a9f60571ff3f35ed5ab57393df0fe",
    "libb/libm020": "8acc5ecfa90b9445eec4b8a2545931492cedd9d3d2d49c101fdf2f9ff50e2ecb",
    "libb/libm020/libm881": "8acc5ecfa90b9445eec4b8a2545931492cedd9d3d2d49c101fdf2f9ff50e2ecb",
    "libb/libm060": "885869098305a7bdc89bd7b7740f92ce728a577352ab57f63092689329c0fe9f",
    "libb32/libm020": "1151be0f4f0d47ca1a0c975523605dc92e3cf7981abfe116e56f9647abc7436f",
    "libb32/libm020/libm881": "1151be0f4f0d47ca1a0c975523605dc92e3cf7981abfe116e56f9647abc7436f",
    "libb32/libm060": "6e6c9d04fbf03e34a15a9e60adf87f9c22d8c0779dfe5701cee48e1fe917c1cf",
}


# AmigaPorts GCC 16.2.1b / newlib e07bea74: eight multilibs. The custom
# allocator moved to mallocr.cpp; libc_a-malloc.o is only a public wrapper
# around _malloc_r and must never stand in for the allocator in this gate.
AMIGAPORTS_MALLOC_SHA256 = {'libb': '8c7f05d2f0797840a85e0558631f4252f43232696be98803b2ce84c3c1d22dfc',
 'libb/libm020': '844d6107c440008eae78c139da5ab2fa747fadd0f414181c394e3fe14c6b2dbb',
 'libb/libm020/libm881': '844d6107c440008eae78c139da5ab2fa747fadd0f414181c394e3fe14c6b2dbb',
 'libb32/libm020': '99a4b73ee31ed218e3b1a9b324ad2f9c97f8c951ea05c2ec6ea58c2a9315ff93',
 'libb32/libm020/libm881': '99a4b73ee31ed218e3b1a9b324ad2f9c97f8c951ea05c2ec6ea58c2a9315ff93',
 '': 'ab3e4f26097acfeca5f630eaf297397d87e0e2bed93338186d998abfb56793a7',
 'libm020': '33fb458d92179450e1da66a2f438531f71530dac93e0a88f3c7cc9bc2d5761cb',
 'libm020/libm881': '33fb458d92179450e1da66a2f438531f71530dac93e0a88f3c7cc9bc2d5761cb'}


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
    expected_malloc = EXPECTED_MALLOC_SHA256
    allocator_member = "malloc.o"
    if compiler_variants == set(AMIGAPORTS_MALLOC_SHA256):
        expected_malloc = AMIGAPORTS_MALLOC_SHA256
        allocator_member = "libc_a-mallocr.o"
    if compiler_variants != set(expected_malloc):
        failures.append(
            "compiler multilib paths differ from pinned allocator map: "
            f"{sorted(compiler_variants)}"
        )

    for archive_name in ("libc.a", "libg.a"):
        expected = {
            str(pathlib.PurePosixPath(variant) / archive_name)
            for variant in expected_malloc
        }
        actual = {
            archive.relative_to(libdir).as_posix()
            for archive in libdir.rglob(archive_name)
        }
        for extra in sorted(actual - expected):
            failures.append(f"m68k-amigaos/lib/{extra}: unexpected unverified multilib")
    for variant, want in expected_malloc.items():
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
                [str(ar), "p", str(archive), allocator_member],
                check=False,
                capture_output=True,
            )
            if result.returncode or not result.stdout:
                failures.append(f"{archive.relative_to(prefix)}: missing {allocator_member}")
                continue
            got = hashlib.sha256(result.stdout).hexdigest()
            if got != want:
                failures.append(
                    f"{archive.relative_to(prefix)}: {allocator_member} {got}, want {want}"
                )

    if failures:
        print("toolchain allocator verification FAILED:", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1
    print(f"toolchain allocator verification passed: {len(expected_malloc)} libc and {len(expected_malloc)} libg multilibs ({allocator_member})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
