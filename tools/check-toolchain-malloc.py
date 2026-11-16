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


# AmigaPorts 16.2-rc25 (newlib 5863aa0e): eight multilibs, identical on Linux
# and macOS. The custom allocator is mallocr.cpp; 5863aa0e gave malloc() a
# benaphore of the heap's own in place of a dummy mutex, so these bytes differ
# from rc22 to rc24 in source, not only in the compiler. libc_a-malloc.o is
# only a public wrapper around _malloc_r and must never stand in for the
# allocator in this gate.
AMIGAPORTS_MALLOC_SHA256 = {
    "": "ac56c08a424dce74c6486d2c96dbc9f58c382e1a953ceb1ce06ef7d212cba3c5",
    "libb": "aad9c851f01518fa658a754406005850636bd7a6d22513627b1f37947c2f91bf",
    "libb/libm020": "ca781684cb04a7cc6041c8130ddfa5e226f82e472a7e8c5070b2221150b76e78",
    "libb/libm020/libm881": "ca781684cb04a7cc6041c8130ddfa5e226f82e472a7e8c5070b2221150b76e78",
    "libb32/libm020": "d965a090cabfedcfbbf459c8031396be12acbb3c3edc2ea56ea349dae11bfeb9",
    "libb32/libm020/libm881": "d965a090cabfedcfbbf459c8031396be12acbb3c3edc2ea56ea349dae11bfeb9",
    "libm020": "62c399918b1c2d4eb9622629d5a9741400987b7df98e44f11953d333cfc7645e",
    "libm020/libm881": "62c399918b1c2d4eb9622629d5a9741400987b7df98e44f11953d333cfc7645e",
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
        # newlib's startup is newlib-crt0.o from AmigaPorts 16.2-rc22 on.
        for companion in (("crt0.o", "newlib-crt0.o"), ("libm.a",)):
            path = libdir / variant / companion[-1]
            if not any((libdir / variant / name).is_file() for name in companion):
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
