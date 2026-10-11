This is a cross-toolchain update, not an AmiNetXDuo release. The compiler still reports 16.2.0b.

- GCC is AmigaPorts `amiga16.2` v16.2-rc15 (`0edaa6190a0f6289a293153b7538b45da6787be9`) plus AmigaPorts/gcc PR #82, tinic/gcc `amiga16.2-rc15-anx` (`fb3ead730d3a3682ab299eb8168f37153497ea19`).
- binutils 2.46 (`amiga-2.46` `a50544a9` + `tools/patches/binutils-2.46/`), newlib, libnix and NDK 3.9 are unchanged from 16.2.5.
- `-Wcallconv-mismatch` is on by default and also covers C++ conversions, calls and conditionals.
- An indirect sibling call with an argument in a0 jumps through a1.
- Under `-fbaserel`, a read-only symbol plus an offset and `.list_*` sections stay absolute.
- `-mhard-float` selects the 68881 multilib, and a 68060 with `-m68881` or `-mhard-float` links the 020/68881 libraries.
- An explicit `-mregparm=0` keeps stack arguments for function types with an unrelated attribute.
- `-Wall` no longer reports array bounds on the SysBase read at address 4.
- Constructor and destructor priorities are honoured.
- `-fpic` and `-fPIC` are errors.
- A `char` or `short` argument bound to a register with `__asm` is widened to 32 bits by the caller.
- LTO-promoted statics draw no visibility warning; a user-written `visibility` attribute still does.

| Asset | Size | SHA-256 |
|---|---:|---|
| `m68k-amigaos-gcc-16.2.6-ndk3.9-linux-x86_64.tar.xz` | 37,878,896 | `479316a7737cd883fe95426d507df1b3a6e484592b85413f96909d423b4c5de3` |
| `m68k-amigaos-gcc-16.2.6-ndk3.9-darwin-arm64.tar.xz` | 32,744,696 | `e07be49191bb73120fd9db2377fd4d0f4d466387070ad771cdb666d8e1a951ba` |
