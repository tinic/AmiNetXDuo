# GCC 16.2.1b-anx.3

The upstream GCC version remains 16.2.1b. `anx.3` identifies this fork's
compiler and package revision independently of upstream releases.

This revision integrates Bebbo's devel-cma cost model and the tested
68000 size improvements from `tinic/gcc` PR #3, merge
`c99b02bd168e1272c98de83e765c438ec28eca0b`. Its source tree is identical to
the tested optimization head `6a861da472737ca6390bf23ce7529d16a236df4b`.
The changes include smaller constant construction, comparison and Boolean
sequences, bounded loop and switch rewrites, address and copy sequences,
and local-call encodings. The narrow volatile byte-packing size regression
is corrected. The boundary, aliasing, condition-code, calling-convention
and LTO regression programs are committed with the optimizations.

The build-system, binutils, runtime-library and NDK 3.2 source pins are
unchanged from `16.2.1b-anx.2`. The packages are built from those pinned
sources with the merged compiler. The allocator hash map is updated for
the resulting objects; installed members match the final newlib build on
both hosts. Target zlib/libpng builds explicitly select the cross ranlib
to prevent macOS ranlib from emptying HUNK archives. They do not add the withdrawn binutils
short-branch heuristic; valid indexed PC8 fields remain covered by the
package's byte-exact negative controls.

The earlier matched library builds were smaller by 1,348 bytes for fat and
5,696 bytes for minimal. Bounded A1200 and A3000 tests passed all compiler
runtime and byte-accounted transfer checks and restored the installed
stacks. A1200 minimal throughput averaged about 2% lower; A3000 throughput
was broadly unchanged. These results do not establish a general speedup.

Both source-built host packages pass 755 native C and 74 native C++ target
checks, plus 36 newlib checks on each of the non-FPU and FPU boards. Eight
additional default/regparm3, plain/LTO programs pass strict 68000 emulation
with memory checking. The allocator gate passes its corruption and unexpected
multilib controls; the branch gate passes all 64 indexed-PC8 byte controls.

Product CI and the full Kickstart, release-archive and shipped-CLI emulator
tiers are separate validation of the product pin. This compiler release
does not itself assert completion of those product tests.

## Assets

| Asset | Bytes | SHA-256 |
|---|---:|---|
| `m68k-amigaos-gcc-16.2.1b-anx.3-ndk3.2-darwin-arm64.tar.xz` | 36431264 | `f6d318f2dd88a91efd4951d375085b1ac1c21a69aa8706b93a22306ff0e8bcf9` |
| `m68k-amigaos-gcc-16.2.1b-anx.3-ndk3.2-linux-x86_64.tar.xz` | 49985248 | `fe7a5ee684429635ed0baa2e64d5e1656bc0c61a71b174fd286aa78ded90462a` |

Extracts to `opt/m68k-amigaos/`. Full source revisions are recorded in each
package’s `.manifest`; the package marker is `16.2.1b-anx.3`. Archives were
round-trip checked including hard links, symlinks and installed file bytes.
