**This is not a release of AmiNetXDuo.** It is a build artifact: the
m68k-amigaos cross toolchain this project compiles against, built from source
and published here because nothing upstream publishes this pairing.

Which asset is pinned, and its expected hash, is recorded in
`tools/fetch-toolchain.sh`. This file describes what the assets are.

## The assets

| | |
|---|---|
| GCC | 16.2.0b (C/C++/ObjC), tinic/gcc `f098a8ef` (AmigaPorts `amiga16.2` `1d4ca5d4` + 3 m68k commits) |
| binutils | 2.46, tinic/binutils-gdb `a3afdefb` (AmigaPorts `amiga-2.46` `2efe6986` + PRs #35-#37), no local patches |
| NDK | 3.9 (`INCLUDE_VERSION` 45) + Roadshow bsdsocket/SANA-II headers |
| Also | libnix, libgcc, libpthread, vasm, sfdc, fd2sfd, fd2pragma, gprof |
| Prefix | `opt/m68k-amigaos/` |

| Asset | Host | Size | SHA-256 |
|---|---|---|---|
| `m68k-amigaos-gcc-16.2.9-ndk3.9-linux-x86_64.tar.xz` | Linux x86-64 (glibc) | 37,791,756 | `adf6daa38acb5fef21ad692178be21e57ee7cd84d87615876d850e4fd07da431` |
| `m68k-amigaos-gcc-16.2.9-ndk3.9-darwin-arm64.tar.xz` | macOS arm64 | 32,433,400 | `99f2e4ccd38add25a066df18a75ec363ae261708317c54c7263a17efe2ed3e2d` |

**Not covered:** macOS x86-64 and Linux aarch64. There is no asset for either;
`tools/build-toolchain.sh` builds on both.

```sh
sha256sum m68k-amigaos-gcc-16.2.9-ndk3.9-linux-x86_64.tar.xz
tar xJf   m68k-amigaos-gcc-16.2.9-ndk3.9-linux-x86_64.tar.xz
# -> opt/m68k-amigaos/bin/m68k-amigaos-gcc
```

`tools/fetch-toolchain.sh` picks the asset for your `uname -s`/`uname -m`,
checks the hash and unpacks it.

The earlier `toolchain-m68k-amigaos-gcc-15.2.0` release remains published. It is
Linux-only and was repackaged from an AmigaPorts image layer rather than built
here; its own notes describe it.

## binutils 2.46

From asset series 16.2.9 binutils is tinic/binutils-gdb `a3afdefb`: AmigaPorts
`amiga-2.46` `2efe6986` (which includes the same-named-section PC-relative fix,
AmigaPorts/binutils-gdb#32) plus AmigaPorts/binutils-gdb#35, #36 and #37: GAS
byte-branch adjust and 0/-1 fixes, and ld rejects byte displacement 0/-1. It carries the `HUNK_RELOC32` `strip`, slim-LTO
archive symbol index and C++ COMDAT `ar` repairs, so no local diffs are applied.
Series 16.2.8 used `ffe2ad82` (`106606ef` + #32).
Series 16.2.4 to 16.2.7 used bebbo `amiga-2.46` `a50544a9` with the three diffs
in `tools/patches/binutils-2.46/`. Series up to 16.2.3 used 2.39.0
(`ab4e5183f56fd83165356a03c890bf0b681d7535`).

## How these were built

`tools/build-toolchain.sh`, run on each host. It pins every source to an exact
commit rather than a branch, because bebbo's branches move.

```
build driver   https://codeberg.org/bebbo/amiga-gcc            86f8ba62f7a5035e309600c86962681e1cbacccb
binutils/GDB   https://github.com/tinic/binutils-gdb           a3afdefba01d87798425f28f12a567a65e6f2c4a   (fix/amiga-pcrel-byte-linkcheck)
GCC            https://github.com/tinic/gcc                    f098a8ef44ac8d11204a3cb0c39c1e4f5cd63fc8   (amiga16.2)
newlib         https://franke.ms/git/bebbo/newlib-cygwin       0909ae9abc18b38595425143e7a63d9e2fc31174
libnix         https://franke.ms/git/bebbo/libnix              b7268e35510b8b7b4ccdad67fbcbb25e73189aef
sfdc           https://franke.ms/git/bebbo/sfdc                5d4efca359e949547553463f5873778bd85e5506
fd2sfd         https://franke.ms/git/bebbo/fd2sfd              7f14d7f15aac2b8426f577f838069e53bf6008ea
netinclude     https://franke.ms/git/bebbo/amiga-netinclude    b9a8d2cdd410dbb896a93bc9c64b253be436dd89
aros-stuff     https://franke.ms/git/bebbo/aros-stuff          c0a06b4ccd13f52d1518540aa88a450d70581458
fd2pragma      https://github.com/adtools/fd2pragma            8c0f352c348a3252f84170eab737919372562e82
vasm           https://github.com/mheyer32/vasm                bb048d9d3cf54d5e38c643182a0ff55b552f65be
NDK 3.9        http://hp.alinea-computer.de/AmigaOS/NDK39.lha  sha256 ca5d8f923158d69a9c15b59d6e1580555ca6c0a48be21c5226c71f90fc927ca6
```

bebbo's GitHub repositories are gone. Codeberg and his own franke.ms Gitea are
what survive, and they are not interchangeable. Each line above is the remote
verified to serve its pin.

GCC is tinic/gcc `f098a8ef` (AmigaPorts `amiga16.2` `1d4ca5d4` + 3 m68k commits). binutils carries no
local diffs; newlib and aros-stuff carry the diffs under
`tools/patches/newlib/` and `tools/patches/aros-stuff/`. amiga-gcc's own
Makefile gets `--with-system-zlib`, `--disable-gdb --disable-sim` and
`--enable-plugins` on the binutils configure line. The rest is carried as
configure and compiler flags:

* binutils 2.39's `objdump.c:4196` assigns `dummy_fprintf` to
  `memory_error_func` across incompatible types. clang 16+ and GCC 14+ both
  make that an error rather than a warning, so `--disable-werror` does not
  reach it, and the two spell the demoting switch differently. The script
  probes by compiling the offending assignment; an empty-file probe answers
  yes to both spellings on GCC and picks the wrong one.
* binutils 2.39 bundles a zlib whose headers collide with a current macOS SDK.
  `--with-system-zlib` is passed on both hosts.

macOS additionally needs Homebrew's `gmp`/`mpfr`/`texinfo` on the search path,
and GNU `rsync` rather than the `openrsync` macOS ships, which mishandles
libnix's `--exclude` patterns. No container is used on either host.

## Source

These are GCC and GNU binutils binaries, GPLv3+, redistributed here. The
corresponding source is the exact commit list above; `tools/build-toolchain.sh`
clones those commits and reproduces the toolchain. Pristine upstream GCC and
binutils, from which bebbo's branches derive, are at
https://ftp.gnu.org/gnu/gcc/ and https://ftp.gnu.org/gnu/binutils/.

The NDK 3.9 headers under `m68k-amigaos/ndk-include` are Amiga OS SDK material
and are not GPL. They are included exactly as upstream ships them, and their
terms are Hyperion/Amiga's, not this project's.
