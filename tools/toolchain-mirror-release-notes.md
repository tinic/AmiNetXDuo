**This is not a release of AmiNetXDuo.** It is a build artifact: a convenience
mirror of the prebuilt m68k-amigaos cross toolchain that this project's CI
compiles against. AmiNetXDuo itself has no releases yet.

## What the asset is

| | |
|---|---|
| Asset | `m68k-amigaos-gcc-15.2.0-ndk3.9-linux-x86_64.tar.xz` |
| Size | 35,875,464 bytes (34.2 MiB) |
| SHA-256 | `fca243597aac34400251c0cc9cede9d74434e43dca1ed5c18d3a82b7bd1ab977` |
| Contents | GCC 15.2.0 (C/C++/ObjC) + binutils for `m68k-amigaos`, newlib, libnix, clib2, ixemul, vasm/vbcc/vlink, and the NDK 3.9 + Roadshow headers |
| Host | **linux/x86-64 ELF only.** These binaries do not run on macOS, Windows or arm64. |
| Unpacked | 265 MB, 5,009 entries, under the prefix `opt/m68k-amigaos/` |

The tarball unpacks to `opt/m68k-amigaos/`, matching the path layout of the
upstream image layer it was made from, so the same extraction logic reads
either source.

Verify before use:

```sh
sha256sum m68k-amigaos-gcc-15.2.0-ndk3.9-linux-x86_64.tar.xz
# fca243597aac34400251c0cc9cede9d74434e43dca1ed5c18d3a82b7bd1ab977
tar xJf m68k-amigaos-gcc-15.2.0-ndk3.9-linux-x86_64.tar.xz
# -> opt/m68k-amigaos/bin/m68k-amigaos-gcc
```

`tools/fetch-toolchain.sh` in this repository does all of that for you, and
falls back to the upstream registry if this release is unreachable.

The upstream tag says `gcc10` and contains **GCC 15.2.0**: AmigaPorts repointed
that branch at the `amiga15.2` GCC in October 2025 and did not rename the tag.
This asset is named for what it actually contains.

## Provenance

Repackaged, byte-for-byte and without recompilation, from a single layer of a
public image:

```
image        docker.io/amigadev/crosstools:m68k-amigaos-gcc10
manifest     sha256:c4e68cf502b4764b810a0d2ec7a2f33adee28630aad0a37d1f3c322da36a7a03  (index, as observed 2026-07-25)
             sha256:a9958137357cb094ffd9c17900cca3678b38caa03b82c3974ba39356ed40ab17  (linux/amd64)
layer        sha256:c63033fd447383b09ab739299075f11d482c79182ff99b55959dd7b970f7b12d  (the COPY /opt/m68k-amigaos step, 93,097,963 bytes gzipped)
image built  2025-11-09
```

The layer digest is the pin. It is the SHA-256 of the layer's bytes, so it is
simultaneously the version identifier and the integrity check, and it cannot
drift if upstream re-pushes the tag.

Only the compression changed: gzip → xz, 93 MB → 35 MB. The extracted tree from
this asset is identical to the extracted tree from that layer, verified with
`diff -r --no-dereference` over all 5,009 entries, including the 46 hard links
and 14 symlinks.

Configure line recorded in the binaries:

```
--prefix=/opt/m68k-amigaos --target=m68k-amigaos --host=x86_64-linux-gnu
--enable-languages=c,c++,objc --enable-version-specific-runtime-libs
--disable-libssp --disable-nls --disable-shared --disable-werror
--enable-threads=no
--with-headers=.../newlib-cygwin/newlib/libc/sys/amigaos/include/
```

## Licensing, and where to get the source

This asset contains **GCC and GNU binutils binaries, which are GPLv3+**, and
this repository is redistributing them. GPLv3 §6 requires that the
corresponding source be made available to anyone who receives the binaries.
These are the directions required by GPLv3 §6(d).

Nothing here was compiled by this project. The corresponding source is the
upstream AmigaPorts tree that produced the image above:

| Component | Source repository | Branch | Commit as of the 2025-11-09 build |
|---|---|---|---|
| GCC 15.2.0 | https://github.com/AmigaPorts/gcc | `amiga15.2` | `5561a85ead7c` (2025-09-22) |
| binutils/GDB | https://github.com/AmigaPorts/binutils-gdb | `amiga` | `19983bc2dc79` (2025-10-31) |
| newlib | https://github.com/AmigaPorts/newlib-cygwin | `amiga` | `4c8cbd88df31` (2025-11-01) |
| build driver | https://github.com/AmigaPorts/m68k-amigaos-gcc | `gcc10` | see `default-repos` for every other component |

The full component list, libnix, clib2, ixemul, fd2sfd, fd2pragma, sfdc, vasm,
vbcc, vlink, ira, lha, libdebug, aros-stuff, amiga-netinclude, with each
repository URL and branch, is the `default-repos` file on the `gcc10` branch of
`AmigaPorts/m68k-amigaos-gcc`. `make` in that repository clones those
repositories and reproduces this toolchain.

Pristine upstream GCC 15.2.0, from which the `amiga15.2` branch derives, is at
https://ftp.gnu.org/gnu/gcc/gcc-15.2.0/ (and its mirrors).

If any of the above becomes unreachable, open an issue on this repository and
the corresponding source will be provided by any means the requester
designates, at no charge beyond the cost of the medium. That offer is valid for
as long as this asset is hosted here, and in any case for at least three years
from publication.

The NDK 3.9 headers under `m68k-amigaos/ndk-include` are Amiga OS SDK material
and are not GPL; they are included exactly as upstream ships them, and their
terms are Hyperion/Amiga's, not this project's.
