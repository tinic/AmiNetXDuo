This is a cross-toolchain update, not an AmiNetXDuo release. It changes one
thing from 16.2.4: GCC gains tinic/gcc `399a27ad06e3e978774af5c556bcf66d81ba799f`
("m68k-amigaos: warn about visibility only when the user asked for it") on
top of `a5166db4` (bebbo `amiga16.2` `134541b3` plus the sibcall/a0 fix).
binutils (`amiga-2.46` `a50544a9` + `tools/patches/binutils-2.46/`), newlib,
libnix and the NDK are unchanged. The compiler still reports 16.2.0b.

16.2.4 printed "visibility attribute not supported in this configuration;
ignored" for every static that LTO partitioning promoted to hidden
visibility, thousands per build. The AmigaOS target now warns only for a
`visibility` attribute the user wrote. Nothing was emitted for visibility
before and nothing is now; code generation is unchanged.

| Asset | Size | SHA-256 |
|---|---:|---|
| `m68k-amigaos-gcc-16.2.5-ndk3.9-linux-x86_64.tar.xz` | 38,153,828 | `d135e713133934aa0eb278c833de5b817fad0a272ec609a4083249eb7433bc45` |
| `m68k-amigaos-gcc-16.2.5-ndk3.9-darwin-arm64.tar.xz` | 33,057,872 | `f671ab36a56a530176a88f7d91cca188373ba24622aa98d92ccba5113823cc4e` |

Both packages passed archive round-trip verification, the crt0 gate (11 of 11
startup objects), the NDK inline-register gate and the installed-allocator
check across 11 libc and 11 libg multilibs; the allocator objects are
unchanged from 16.2.4. A seven-partition LTO link prints no visibility
warning, and a user `visibility ("hidden")` attribute still does.

`tools/build-toolchain.sh` documents the exact source pins and rebuild
process. The package prefix is `opt/m68k-amigaos/`. No macOS x86-64 or Linux
aarch64 asset is provided.
