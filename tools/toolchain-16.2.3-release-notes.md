This is a cross-toolchain update, not an AmiNetXDuo release. The compiler is
the pinned tinic/gcc fork at `243a0096237cc382c075142c80acadad4e07b9e5`
(reporting GCC 16.2.0b); newlib is pinned at
`0909ae9abc18b38595425143e7a63d9e2fc31174`.

Version 16.2.3 rebuilds and installs pinned newlib after libnix, before the
C++ runtime. The 16.2.2 assets accidentally retained a bootstrap libc whose
small-page `malloc` path omits `used->prev = null`; the pinned newlib source
already contained that fix, but the packaging build did not run its target.
Do not use 16.2.2 for allocator or memory-safety testing.

| Asset | Size | SHA-256 |
|---|---:|---|
| `m68k-amigaos-gcc-16.2.3-ndk3.9-linux-x86_64.tar.xz` | 38,203,836 | `b24ce5d006514b2f471daac564838ac0aa2adff8f21aa54d2afc6d6b256cd1f3` |
| `m68k-amigaos-gcc-16.2.3-ndk3.9-darwin-arm64.tar.xz` | 32,996,580 | `09d61276d2e1684d1040f9e06f8d927758fc8d554f255e843822e9e6ce233a4f` |

Both packages passed archive round-trip verification and installed-object
checks across 11 libc and 11 libg multilibs. A default-newlib `-m68020`
allocator stress binary built from each host toolchain was byte-identical.
Under MungWall and Enforcer, the new binary completed eight cycles of 512
allocations without a new violation; the 16.2.2 binary produced 40 invalid
writes in the same diagnostic guest.

The rebuilt libraries also populate GCC's `libm060` variant. `-m68060`
builds now select that variant's libc and crt0 rather than falling back to
the top-level 68000 copies; its crt0 passed the existing startup-object gate.

`tools/build-toolchain.sh` documents the remaining exact source pins and
rebuild process. The package prefix is `opt/m68k-amigaos/`. No macOS x86-64 or
Linux aarch64 asset is provided.
