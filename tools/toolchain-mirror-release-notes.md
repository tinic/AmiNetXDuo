**This is a compiler/toolchain package, not an AmiNetXDuo product release.**

Use codewiz/AmigaPorts GCC `amiga16.2` at
`8dc8d8365a3825b74dfba58f8e80545b052e0cd5` and its complete runtime/SDK
baseline. Both compiler and binutils source match codewiz. Earlier GCC fixes
remain through upstream ancestry. No bebbo `devel-cma` changes are included.

## Version policy

`tools/toolchain-version.sh` is the single identity source. The package name
is upstream GCC `BASE-VER` plus our revision: **16.2.1b-anx.2**. The release
tag is **toolchain-m68k-amigaos-gcc-16.2.1b-anx.2**.
Increment `anx.N` for our source/build/SDK/packaging revisions within one
upstream version; restart at `anx.1` when the upstream version changes.
Never replace assets at an existing tag. The original compiler banner and
`.manifest` remain intact; `.aminetxduo-toolchain-version` records our revision.
Use `gcc -dumpfullversion` for `16.2.1b`; `-dumpversion` reports only `16`.

## Assets

| Asset | Host | Size | SHA-256 |
|---|---|---|---|
| `m68k-amigaos-gcc-16.2.1b-anx.2-ndk3.2-linux-x86_64.tar.xz` | Linux x86-64 | 50,047,140 | `8529a742091a28f23c03c4bbfbd9148eaeb15a9c362fb200c79ed818e5390369` |
| `m68k-amigaos-gcc-16.2.1b-anx.2-ndk3.2-darwin-arm64.tar.xz` | macOS arm64 | 36,764,512 | `b3b4bac39a02b57b0d463e064df0780f80b397e5200ae38bb393034054a375b7` |

Each extracts to `opt/m68k-amigaos/`. `tools/fetch-toolchain.sh` selects the
host asset, checks its full hash and verifies GCC plus package identity on
fresh and warm caches. Linux arm64 and macOS x86-64 have no published asset;
use the source recipe on those hosts.

## The 0/-1 correction

`anx.1` is withdrawn and its assets remain unchanged. It carried the
previously withdrawn linker heuristic from
[AmigaPorts/binutils-gdb#37](https://github.com/AmigaPorts/binutils-gdb/pull/37).
HUNK PC8 relocations also describe valid indexed operands, so guessing branch
identity from opcode-like bytes rejects legal code. The earlier assembler
proposal [#36](https://github.com/AmigaPorts/binutils-gdb/pull/36) depended on
sound final validation; this reset carries neither patch. Binutils is the
unmodified codewiz `15f4b0aa` baseline. No new upstream proposal is made.

The installed-tool regression verifies 64 D/A indexed PC8 cases with final
0/-1 displacements and compares their emitted bytes exactly. The withdrawn
linker fails this positive control. The assembler still rejects known local
short branches with reserved displacements. Use local short branches where
the assembler can resolve them and word branches for deferred targets.
The generic HUNK linker does not provide a universal final short-branch check.
AmiNetXDuo's manually encoded Alert branch is also guarded at compile time;
its current instruction remains `6022` (displacement 34).

## Sources and build provenance

These assets are repackaged from fresh native builds for
[tinic/gcc#2](https://github.com/tinic/gcc/pull/2), using codewiz's build
workflow. Repackaging adds our version marker and changes the archive prefix;
it does not change compiler, binutils, runtime or SDK file bytes. The complete
extraction round trip, hard links and symlinks are checked. GNU tar rewrites
member and hard-link paths while preserving symlink targets.

Build driver: AmigaPorts/m68k-amigaos-gcc
`de370a15e783261e82d0ba0ab809eff128356de1`.

```
amiga-netinclude  7d155ca3a3090afe3466f819dd178f2af6f5c176  https://github.com/AmigaPorts/amiga-netinclude
binutils          15f4b0aa860f74022c6aff7ace15bc0a307d5510  https://github.com/AmigaPorts/binutils-gdb
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

NDK 3.2: <https://aminet.net/dev/misc/NDK3.2.lha>, SHA-256
`96cabd4ad683dced632e147bf86dee0f50dcb1254386216c25c362916a6409bb`.
Other archive hashes are pinned by the build driver's `default-archives`.
`tools/build-toolchain.sh --print-pins` shows the standalone source recipe;
it uses the upstream build graph without historical local bebbo patches.
The native CI builds below were executed; the standalone pinned recipe has
not been rebuilt separately.

This baseline uses GCC 16.2.1b, binutils 2.46, current AmigaPorts newlib/libnix
and NDK 3.2. Its eight multilibs cover 68000, baserel and 68020-60 with
baserel/baserel32 and optional 68881. The common `libm020` is built for
68020-60; the old separate `libm060` variants are gone.

## Validation and limits

Compiler CI: [Linux 39349235159](https://github.com/tinic/gcc/actions/runs/39349235159),
[macOS 39349239433](https://github.com/tinic/gcc/actions/runs/39349239433),
[newlib 39349345711](https://github.com/tinic/gcc/actions/runs/39349345711).
All three jobs succeeded. The compiler reports 742 Linux and 697 macOS PASS
records, with the same 607 distinct PASS labels (parallel multilib checks
repeat). Newlib reports 36 passes on each soft-float/FPU board. C++ pthread
tests are explicitly skipped because libstdc++ thread support is disabled.
The Amiga gas/ld gates pass. Broader gas all.exp has 9 passes and 27 existing
non-Amiga failures; those remain failures, not a full gas-suite pass.

The allocator gate verifies the actual custom `libc_a-mallocr.o` in all eight
`libc.a`/`libg.a` variants. Corruption and unexpected-multilib negative controls
are rejected. Startup-object, NDK register ABI and resolver checks remain
active. Micro/default/fat builds retain warnings as errors. This is not the
full GNU C/C++ suite or physical Amiga validation, and makes no performance
claim. Historical Enforcer evidence for the older runtime does not validate
this new runtime.

## Source and licenses

GCC/binutils are GPLv3+; corresponding sources are the exact repositories and
commits above. Runtimes/helpers and SDKs retain their own licenses. Upstream
GNU sources are at <https://ftp.gnu.org/gnu/gcc/> and
<https://ftp.gnu.org/gnu/binutils/>. Included NDK/SDK material is under its
own terms, not this project's license. Older releases retain their original
assets and provenance.
