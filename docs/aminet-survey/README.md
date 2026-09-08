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
| `lvo-collisions.tsv` | every vector against the 76 NDK `_lib.i` tables |
| `results.tsv` | the ledger: one row per scanned binary |

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
