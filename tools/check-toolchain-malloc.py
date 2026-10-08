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
# Asset series 16.2.4 and 16.2.5 (GCC a5166db4, 399a27ad): each installed malloc.o is the object
# newlib's final build left in newlib/libc/sys/amigaos/, on Linux and macOS.
EXPECTED_MALLOC_SHA256 = {
    "": "699875ab532e97d004d48b6988b132373e1ecbe085ad7c887013cbb907017550",
    "libm020": "1d39cab4bdfc999c390af941a57b8df2375ca0276e5c8fc0d98d1f41625c4dfb",
    "libm020/libm881": "1d39cab4bdfc999c390af941a57b8df2375ca0276e5c8fc0d98d1f41625c4dfb",
    "libm060": "74bf940fd4378b8fcbae052827e5218c100c00390a8fa22561d467a94637dddc",
    "libb": "e6b12ab7ce9cd66806962e58ab3050e2993c1a6411e2451bf42c145ea84efd12",
    "libb/libm020": "b34cc3e917f80344b5a7271a05e06acfc2f470b203b0abb139c2962f3a198936",
    "libb/libm020/libm881": "b34cc3e917f80344b5a7271a05e06acfc2f470b203b0abb139c2962f3a198936",
    "libb/libm060": "5c7409df3d56d11df7029f66a6acc01e713f900c4e46713d8f853e53f5c66a79",
    "libb32/libm020": "2061efe4d7dd893d288273d0554e94d8ff3bd291fcc0785ef214204614a1b786",
    "libb32/libm020/libm881": "2061efe4d7dd893d288273d0554e94d8ff3bd291fcc0785ef214204614a1b786",
    "libb32/libm060": "89353ba2f35a8e4eace32a4a5d3e2db7643d94341f44d9428bb73171ca8e919b",
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
