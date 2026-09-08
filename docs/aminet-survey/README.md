# Aminet bsdsocket.library survey

What real Amiga network software actually calls, measured by resolving
`SocketBase` and attributing every `jsr d16(a6)` through it.  Drives what the
micro profile may stub.

| file | content |
|---|---|
| `lvo-usage.tsv` | every one of the 143 vectors, one row, with the number of attributed binaries calling it |
| `unused-vectors.tsv` | the same set as USED / NO_CALLER / RESERVED |
| `candidates.tsv` | applications ranked by marginal API coverage, for the test harness |
| `lvomap.tsv` | offset -> vector -> implementing symbol, generated from `src/bsdsocket/bsdsocket_vectors.c` |
| `worklist.txt` | the corpus: every archive path the survey draws from |
| `lvo-collisions.tsv` | every vector against the 76 NDK `_lib.i` tables |
| `lvo-rare.tsv` | every vector with FEWER THAN 10 callers, with the callers named |
| `results.tsv` | the ledger: one row per scanned binary |

## Raw and derived

Everything here can be rebuilt from three files, so a later reassessment does
not depend on trusting the summaries:

| raw input | what it is |
|---|---|
| `worklist.txt` | the corpus, 5,923 archive paths |
| `results.tsv` | one row per scanned binary: archive, path, verdict, distinct, calls, and the LVO NAMES it calls |
| `lvomap.tsv` | the 143 vectors, pinned against the source that defines them |

| derived | rebuild with |
|---|---|
| `lvo-usage.tsv` | counted from `results.tsv` column 6 |
| `unused-vectors.tsv` | `lvo-usage.tsv` against `lvomap.tsv` |
| `lvo-rare.tsv` | `tools/aminet-survey/rare.py 10` |
| `candidates.tsv` | `tools/aminet-survey/candidates.py` |
| `lvo-collisions.tsv` | `lvomap.tsv` against the NDK `_lib.i` tables |

`results.tsv` keeps the LVO names per binary rather than a count, which is why
a different threshold, a different grouping, or a per-application question can
be answered later without rescanning 1,700 archives.

## Method

| step | what |
|---|---|
| corpus | `aminet.net/INDEX`, `comm/` plus networked archives elsewhere |
| attribute | find `OpenLibrary("bsdsocket.library")`, read the store, count only `movea.l SocketBase,a6` + `jsr/jmp d16(a6)` |
| `SOCK_RAW` | argument-pattern, not a vector: `moveq #3,d1` (or `pea 3.w`) before `socket()` |

`tools/aminet-survey/` holds the harness.  `./pick.sh <seed> > /tmp/n12.txt`
then `./tick.sh $(cat /tmp/n12.txt)`.

## Why a displacement scan is not enough

**143 of 143 vectors share their offset with at least one other library**
(`lvo-collisions.tsv`): `-366` is `bpf_open` AND exec `PutMsg`; `-144` is
`ObtainSocket` AND dos `Exit` AND AS225 `gethostbyaddr`.  There is no
unambiguous offset, so `SocketBase` provenance is the method, not an
improvement to it.  The first harness scanned displacements and ran ~80% false
positives -- AmiFTP scored 89 against a ground truth of 18.

## Limits that bind any conclusion drawn from this

| limit | consequence |
|---|---|
| attribution ~55% | "no observed caller" means no caller in the attributed half |
| argument-selected features are invisible | `SOCK_RAW` has 36 users and would read as 0 if it had a vector; the same hides `setsockopt` options, `IoctlSocket` commands, `SBTM_*` tags, `AF_INET6` |
| AS225 dual-stack excluded | those binaries mix two LVO tables; flagged, not counted |
| era | Aminet spans decades; a `getaddrinfo`/`getnameinfo` hit on a MODERN archive is real, not a scan error |

## What the unused vectors cost

Measured two ways, because the first was wrong.

| set | impls | LTO build | non-LTO build |
|---|---|---|---|
| zero-caller | 52 | 29,634 | **14,290** |
| used | 55 | 23,836 | 21,098 |

Sizes come from text-symbol address gaps, and under LTO a symbol absorbs
inlined neighbours: `FreeRouteInfo` measured 6,710 bytes with LTO and does not
appear in the non-LTO top 14 at all.  **Quote the aggregate, never a per-vector
figure** -- individual entries move by hundreds of bytes in both directions
(`if_freenameindex` 10 -> 970, `In_CanForward` 320 -> 834).

The non-LTO numbers are the ones to plan with: the unused half of the API is
about two thirds the size of the used half, not larger than it.

Reproduce:

    cmake -S . -B build/noltosym \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-m68k-amigaos.cmake \
      -DCMAKE_BUILD_TYPE=Release -DAMINETXDUO_LTO=OFF \
      -DAMINETXDUO_KEEP_SYMBOLS=ON -DAMINETXDUO_TESTS=OFF
    cmake --build build/noltosym --target bsdsocket_library

`tools/ci.sh` does NOT honour `AMINETXDUO_EXTRA_CMAKE`; passing it yields a
byte-identical LTO library and a validation that silently did nothing.

## Known accepted losses: what stops working if a vector is cut

A vector with a handful of callers may be droppable -- but the cost has to be
NAMED, not left as a number.  `lvo-rare.tsv` carries everything under 10
callers with the callers attached; this is the same data as a support
statement.

| vector | offset | programs that stop working |
|---|---|---|
| `AbortInterfaceConfig` | -492 | `miamisecureshell.library` |
| `bpf_open` | -366 | `miamiipnat.library` |
| `getnameinfo` | -822 | `binkd` |
| `GetSocketEvents` | -300 | `GiFTMui`, `lanclip` |
| `recvmsg` | -276 | `MiamiDx`, `MiamiIPNatD` |
| `ObtainInterfaceList` | -462 | `AmigaTTextOS3_WARPPPC`, `NetMon.68k`, `RNOXfer`, `SonosController` |
| `QueryInterfaceTagList` | -468 | `AmigaTTextOS3_WARPPPC`, `NetMon.68k`, `RNOXfer`, `SonosController` |
| `ReleaseInterfaceList` | -456 | `AmigaTTextOS3_WARPPPC`, `NetMon.68k`, `RNOXfer`, `SonosController` |
| `getprotobynumber` | -252 | `MiamiHost`, `MiamiIPFW`, `MiamiNSLookup`, `MiamiNetStat` |
| `bpf_write` | -384 | `AmiHomeassist`, `AmiHomeassistCLI`, `AmiMatters` |
| `bpf_close` | -372 | `AmiHomeassist`, `AmiHomeassistCLI`, `AmiMatters`, `miamiipnat.library` |
| `getnetbyaddr` | -228 | `MiamiNetStat`, `MiamiRoute`, `NNTPd`, `route` |
| `sendmsg` | -270 | `MiamiDx`, `MiamiIPNatD`, `MiamiNSLookup`, `ch_nfsc`, `u9fs` |
| `ReleaseCopyOfSocket` | -156 | `inetd`, `letnet`, `rsh` |

Read `callers` next to `programs` in `lvo-rare.tsv`: a low count is often ONE
program shipped in several distributions, which the raw number overstates as
diversity.  `ReleaseCopyOfSocket` is 9 callers but only 3 programs -- `inetd`,
`letnet`, `rsh` -- and it is the same handoff contract as `ObtainSocket` (91),
so it is not the marginal vector its count suggests.  `sendmsg` is `ch_nfsc` in
four distributions plus `u9fs`: the NFS and 9P filesystem layer.

Counts move as the survey runs; regenerate with
`tools/aminet-survey/rare.py 10`.
