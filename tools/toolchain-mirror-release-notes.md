**This is a compiler/toolchain package, not an AmiNetXDuo product release.**

The baseline is codewiz/AmigaPorts GCC `amiga16.2` at
`8dc8d8365a3825b74dfba58f8e80545b052e0cd5`, with its runtime/SDK stack. Our binutils fork retains two existing
byte-branch correctness fixes on top of codewiz's baseline. The GCC sync
retains our earlier GCC fixes through upstream ancestry. No bebbo
`devel-cma` changes are included.

## Version policy

`tools/toolchain-version.sh` is the single package identity source:

* Upstream GCC `BASE-VER`: **16.2.1b** (`gcc -dumpfullversion`).
* Our package revision: **anx.1**.
* Package version: **16.2.1b-anx.1**.
* Release tag: **toolchain-m68k-amigaos-gcc-16.2.1b-anx.1**.

Increment `anx.N` whenever our source, build configuration, SDK or packaging
changes without an upstream GCC version change. Start at `anx.1` when the
upstream GCC version changes. Never replace assets in an existing release.
The upstream build driver's own banner and `.manifest` remain intact; the
additional `.aminetxduo-toolchain-version` records our package revision.
`-dumpversion` reports only `16` with this compiler; use `-dumpfullversion`
when checking the upstream version.

## Assets

| Asset | Host | Size | SHA-256 |
|---|---|---|---|
| `m68k-amigaos-gcc-16.2.1b-anx.1-ndk3.2-linux-x86_64.tar.xz` | Linux x86-64 | 50,719,900 | `032c0c4bc564d36815c42c0ffecc961762ef37c46a0d25a0dbc2046e4c33dd80` |
| `m68k-amigaos-gcc-16.2.1b-anx.1-ndk3.2-darwin-arm64.tar.xz` | macOS arm64 | 37,288,744 | `968c1d73995c60df349dcd82be6786e5c09d0e305dc7a13a057b00f5b4685896` |

Each archive extracts to `opt/m68k-amigaos/`. `tools/fetch-toolchain.sh`
selects the host asset, verifies its full SHA-256 and selects that exact cache.
Linux arm64 and macOS x86-64 have no published asset; use the source build
recipe on those hosts.

This resets the previous mixed bebbo/codewiz build to codewiz's baseline,
with the two retained binutils correctness fixes: GCC 16.2.1b, binutils 2.46, current AmigaPorts newlib/libnix and
NDK 3.2. It includes the SDKs and helper tools built by codewiz's normal
native CI. The new multilib layout has eight variants: 68000, baserel and
68020-60/baserel/baserel32 with optional 68881. There is no separate
`libm060`; 68020 through 68060 use the common `libm020` libraries.

## Build provenance and validation

These assets are repackaged from fresh native Linux and macOS builds for
[tinic/gcc#2](https://github.com/tinic/gcc/pull/2), using codewiz's reusable
build workflow. Binutils is then rebuilt natively from
[tinic/binutils-gdb#1](https://github.com/tinic/binutils-gdb/pull/1), which
merges codewiz's current baseline with our existing deferred byte-branch and
invalid-encoding guards. The original CI manifest is retained separately;
the overlay record identifies the rebuilt binutils. The original CI `.manifest` and the accompanying
`.aminetxduo-toolchain-overlay.json` identify all installed source versions.
`tools/package-toolchain-mirror.sh` adds our version marker, changes the
archive prefix and verifies the full extraction round trip, links and entry
count. The packaging step does not recompile or change compiler/runtime objects.

Build driver: AmigaPorts/m68k-amigaos-gcc
`de370a15e783261e82d0ba0ab809eff128356de1`.

```
amiga-netinclude  7d155ca3a3090afe3466f819dd178f2af6f5c176  https://github.com/AmigaPorts/amiga-netinclude
binutils          6f53cbde391ed1fa95a24a61ac2adb6edf229df8  https://github.com/tinic/binutils-gdb
clib2             78e05e255495382bbf880d0d37d20763d129f01e  https://github.com/AmigaPorts/clib2
fd2pragma         bcffac5c89295baf1db033addc984312b4b4a722  https://github.com/AmigaPorts/fd2pragma
fd2sfd            a0ae60d140fdfd30f889810faea07cc6968d1fd0  https://github.com/AmigaPorts/fd2sfd
gcc               8dc8d8365a3825b74dfba58f8e80545b052e0cd5  https://github.com/tinic/gcc
ira               bb7c11384a32c409341fdb0d16d715b7959a0290  https://github.com/AmigaPorts/ira
ixemul            f0dd997994263c6ac783b563222a9f53a00c013c  https://github.com/AmigaPorts/ixemul
lha               979815dc8699a9c5018de067946737c83ec6f697  https://github.com/AmigaPorts/lha
libdebug          76c7dbfeaac79516117dc810e3ba9b34d5f54a69  https://github.com/AmigaPorts/libdebug
libnix            df91c92564490bae1a5abd9e302f466aa7684bac  https://github.com/AmigaPorts/libnix
newlib-cygwin     e07bea74897b6ee7b645602d66fcff6c6792d1a9  https://github.com/AmigaPorts/newlib-cygwin
sfdc              4e7afc044c1ff129574fcf3b839c3351edcfe2fb  https://github.com/AmigaPorts/sfdc
vasm              de93f7dd067001532cff663730133601edb274cb  https://github.com/AmigaPorts/vasm
vbcc              55a487d500dccccad806cf3916e90bcd95189ec9  https://github.com/AmigaPorts/vbcc
vlink             ce46ccfda0e30eda5b12b9a5d0259d8eaefdefe9  https://github.com/AmigaPorts/vlink
aros-stuff        9e09fb0f9f5150058c788fb42787927907b28a46  https://github.com/AmigaPorts/aros-stuff
```

NDK: `https://aminet.net/dev/misc/NDK3.2.lha`, SHA-256
`96cabd4ad683dced632e147bf86dee0f50dcb1254386216c25c362916a6409bb`.
Other downloaded archives are pinned by the build driver's `default-archives`.
`tools/build-toolchain.sh --print-pins` records the source recipe. Unlike the
old recipe it uses the AmigaPorts build graph without the historical local
bebbo patches. The independently executed builds for this release are the
native CI jobs plus the native binutils rebuilds; the standalone pinned recipe has not been rebuilt separately.

Compiler CI: [Linux 39349235159](https://github.com/tinic/gcc/actions/runs/39349235159),
[macOS 39349239433](https://github.com/tinic/gcc/actions/runs/39349239433),
[newlib 39349345711](https://github.com/tinic/gcc/actions/runs/39349345711).
The compiler summaries report 742 Linux and 697 macOS PASS records; both
contain the same 607 distinct PASS labels (parallel multilib checks repeat).
Newlib reports 36 passes on each soft-float/FPU board. C++ pthread tests are
explicitly skipped because libstdc++ thread support is disabled.
The rebuilt binutils passes all 19 Amiga linker cases on both hosts. Its
Amiga gas case passes; broader gas reports 9 passes and 27 existing non-Amiga
failures, with identical outcome rows against unmodified codewiz tools on
both hosts. Those failures remain recorded as failures.
The installed-tool branch selftest also proves valid deferred fields assemble
and link, while final 0/-1 branch displacements are rejected. The
allocator gate checks the actual custom allocator `libc_a-mallocr.o` in all
eight `libc.a` and `libg.a` variants, rather than the public malloc wrapper.
Negative controls prove that a corrupted allocator and an extra unverified
multilib are rejected. Existing startup-object and NDK register-ABI checks
remain enabled. Newlib tests use simulator soft-float and FPU boards.
AmiNetXDuo micro/default/fat builds retain warnings as errors.
This is not the full GNU C/C++ testsuite or physical Amiga validation, and
it makes no performance claim. Historical Enforcer evidence for the older
runtime is not evidence for this new runtime.

## Source and licenses

GCC and binutils are GPLv3+; exact corresponding sources are the repositories
and commits above. The build driver and each runtime/helper retain their own
licenses. Upstream GNU sources are at <https://ftp.gnu.org/gnu/gcc/> and
<https://ftp.gnu.org/gnu/binutils/>. Included NDK/SDK files are Amiga SDK
material under their own terms, not this project's license.

Older `16.2.9`, earlier `16.2.x` and `15.2.0` releases and their asset hashes
remain unchanged; their own release notes describe those historical packages.
