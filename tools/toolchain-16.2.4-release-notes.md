This is a cross-toolchain update, not an AmiNetXDuo release. Compiler, binutils, NDK and newlib pins are unchanged from 16.2.3.

- newlib is compiled at `-Os -fomit-frame-pointer`; libnix, libdebug, libgcc and libpthread stay at `-O2 -fomit-frame-pointer`.
- At `-Os` newlib builds its size-optimised `memmove`, `memcmp`, `memchr`, `strchr`, `strcmp` and `strncmp`; `memcpy` and `memset` remain m68k assembly and `strlen` its m68k inline-assembly loop, byte-identical to 16.2.3.
- `libgcc.a` and `libgcov.a` carry no DWARF sections; a program linking libgcc's 64-bit division is no longer written without relocations.
- `malloc.o` keeps the fresh-page `used->prev = null` store in all 11 libc and 11 libg multilibs; `tools/check-toolchain-malloc.py` pins the hashes as series 16.2.4.
- C:ssh is 332,016 bytes (16.2.3: 369,324); C:scp is 103,400 bytes (16.2.3: 141,064).

| Asset | Size | SHA-256 |
|---|---:|---|
| `m68k-amigaos-gcc-16.2.4-ndk3.9-linux-x86_64.tar.xz` | 37,029,460 | `3ef0bd868984a73097cd506cd5aacfb7d3e6ecb6b650e91ef0743dfe92676414` |
| `m68k-amigaos-gcc-16.2.4-ndk3.9-darwin-arm64.tar.xz` | 31,870,540 | `b334323b1d6af64e8e61407fde8ec20a53f3bf36f8839347788d8df59887f744` |

The package prefix is `opt/m68k-amigaos/`. No macOS x86-64 or Linux aarch64 asset is provided.
