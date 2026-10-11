This is a cross-toolchain update, not an AmiNetXDuo release. The compiler still reports 16.2.0b.

- GCC is AmigaPorts `amiga16.2` `1d4ca5d4b1592b663af2f00589b245934d872eb3` with no local patches (tinic/gcc `amiga16.2-16.2.7`).
- binutils 2.46 (`amiga-2.46` `a50544a9` + `tools/patches/binutils-2.46/`), newlib, libnix and NDK 3.9 are unchanged from 16.2.6.
- The LTO visibility-warning fix carried as a patch in 16.2.6 is now upstream (AmigaPorts/gcc #82).
- A function argument bound to a5 or sp with `__asm` is an error; an incoming single-register sp parameter is allowed (AmigaPorts/gcc #83).
- A `char` or `short` argument bound to a register with `__asm` is passed in its declared mode, so the caller no longer widens it; callees still extend it, and objects from 16.2.6 or older mix without a rebuild (AmigaPorts/gcc #84).
- `-fpic` and `-fPIC` warn and are ignored instead of being errors.
- `-mno-lra` no longer forms paradoxical DImode subregs in ext-dce.

| Asset | Size | SHA-256 |
|---|---:|---|
| `m68k-amigaos-gcc-16.2.7-ndk3.9-linux-x86_64.tar.xz` | 37,872,184 | `8d04c18e4b04d861848854922b6761651958932af85b4a6a26918e6da4d60e97` |
| `m68k-amigaos-gcc-16.2.7-ndk3.9-darwin-arm64.tar.xz` | 32,754,352 | `59c695e980c9866580e32409dcfa9854e97cc0d4d9dffc5383c0c064da602cb3` |
