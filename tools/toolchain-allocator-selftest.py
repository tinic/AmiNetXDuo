#!/usr/bin/env python3
"""Exercise the allocator gate with real archives and deliberately bad copies."""

import pathlib
import shutil
import subprocess
import sys
import tempfile


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: toolchain-allocator-selftest.py <toolchain-root>")
    root = pathlib.Path(sys.argv[1]).resolve()
    gate = pathlib.Path(__file__).with_name("check-toolchain-malloc.py")

    def check(prefix):
        return subprocess.run(
            [sys.executable, str(gate), str(prefix)],
            capture_output=True, text=True, check=False,
        )

    good = check(root)
    if good.returncode:
        raise SystemExit(good.stdout + good.stderr)
    libraries = root / "m68k-amigaos/lib"
    ar = root / "bin/m68k-amigaos-ar"
    archive = libraries / "libc.a"
    members = subprocess.check_output([str(ar), "t", str(archive)], text=True).splitlines()
    member = "libc_a-mallocr.o" if "libc_a-mallocr.o" in members else "malloc.o"
    original = subprocess.check_output([str(ar), "p", str(archive), member])
    if not original:
        raise SystemExit("allocator control is empty")

    with tempfile.TemporaryDirectory(prefix="anxd-allocator-") as tmp:
        fixture = pathlib.Path(tmp)
        (fixture / "bin").mkdir()
        for tool in ("m68k-amigaos-gcc", "m68k-amigaos-ar"):
            (fixture / "bin" / tool).symlink_to(root / "bin" / tool)
        for name in ("libc.a", "libg.a", "crt0.o", "libm.a"):
            for source in libraries.rglob(name):
                target = fixture / source.relative_to(root)
                target.parent.mkdir(parents=True, exist_ok=True)
                target.symlink_to(source)
        # The unmodified borrowed fixture must pass before either negative control.
        borrowed = check(fixture)
        if borrowed.returncode:
            raise SystemExit(borrowed.stdout + borrowed.stderr)

        bad_archive = fixture / "m68k-amigaos/lib/libc.a"
        bad_archive.unlink()
        shutil.copyfile(archive, bad_archive)
        bad_member = fixture / member
        corrupted = bytearray(original)
        corrupted[len(corrupted) // 2] ^= 1
        bad_member.write_bytes(corrupted)
        subprocess.run([str(ar), "r", str(bad_archive), str(bad_member)], check=True,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        bad = check(fixture)
        if bad.returncode == 0 or member not in bad.stdout + bad.stderr:
            raise SystemExit("corrupted allocator was not rejected with its member identity")

        bad_archive.unlink()
        bad_archive.symlink_to(archive)
        unexpected = fixture / "m68k-amigaos/lib/unverified/libc.a"
        unexpected.parent.mkdir()
        unexpected.symlink_to(archive)
        bad = check(fixture)
        if bad.returncode == 0 or "unexpected unverified multilib" not in bad.stdout + bad.stderr:
            raise SystemExit("unexpected archive was not rejected")

    print("allocator_gate_selftest=PASS positive=2 negative=2 member=" + member)
    return 0


if __name__ == "__main__":
    sys.exit(main())
