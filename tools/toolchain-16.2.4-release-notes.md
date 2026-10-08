This is a cross-toolchain update, not an AmiNetXDuo release. It moves both
GCC and binutils; newlib (`0909ae9abc18b38595425143e7a63d9e2fc31174`),
libnix and the NDK are unchanged from 16.2.3.

* GCC: tinic/gcc `a5166db4a6ec6c20e0dfc79593742a88fa632335`, which is bebbo
  `amiga16.2` at `134541b36861c551aa9b37ec80a3e57ac1382328` plus the sibcall/a0
  fix (previously 243a0096 on 60f21496). The compiler still reports 16.2.0b.
* binutils: 2.39.0 to 2.46, bebbo `amiga-2.46` at
  `a50544a917284847c99e97d41c69ece9d5cb2fef`, with the three diffs in
  `tools/patches/binutils-2.46/`. One keeps `HUNK_RELOC32` through `strip` and
  `objcopy --strip-all`; one restores the archive symbol index for slim LTO
  members; one backports bebbo `6fa839d0`, without which `ar` dies with SIGFPE
  indexing a C++ COMDAT object.
* The build runs under `LC_ALL=C`, so `libc.a` member order no longer follows
  the build host's locale.

| Asset | Size | SHA-256 |
|---|---:|---|
| `m68k-amigaos-gcc-16.2.4-ndk3.9-linux-x86_64.tar.xz` | 38,143,060 | `9690e9ee36f68cec26e3ec2330550fd527204b61df367aa62fa74d1f254e28b2` |
| `m68k-amigaos-gcc-16.2.4-ndk3.9-darwin-arm64.tar.xz` | 33,062,340 | `01c8cc2ab3458a68c2c1eff623f2807efed1c63f21502a739582a49b0dd88f4f` |

Both packages passed archive round-trip verification, the crt0 gate (11 of 11
startup objects), the NDK inline-register gate and the installed-allocator
check across 11 libc and 11 libg multilibs. The allocator hashes changed with
the compiler: the pinned newlib `malloc.cpp` built by the 16.2.3 compiler
reproduces the old pinned object byte for byte, and built by this compiler
reproduces the new one; both hosts install identical objects. The seven
sibcall/a0 tests pass on both hosts.

`tools/build-toolchain.sh` documents the exact source pins and rebuild
process. The package prefix is `opt/m68k-amigaos/`. No macOS x86-64 or Linux
aarch64 asset is provided.
