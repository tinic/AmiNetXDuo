# Research: an Exec backend for the NetX Duo ThreadX contract

**Status: research only; first wait primitive implemented and native smoke
verified, no NetX replacement or backend conformance validation.**
The human explicitly placed this work on a research branch on 2026-10-07.
It is outside near-term shipping work. It must not change shipping defaults,
release gates or installed machines. The existing ThreadX backend remains the
production implementation and the reference for experiments.

Branch: `research/exec-threadx-compat`.
Baseline: published beta8, `8c93d4a0e7deb7d193682e4e33378b3f0db92e29`.

## Research question

Can a small Exec-backed implementation of the ThreadX services and internals
used by NetX Duo reduce code, static memory or runtime overhead while keeping
future NetX Duo integrations straightforward?

The primary architectural requirement is to preserve NetX Duo's existing source
interface. This experiment must not add OS-specific changes throughout its
protocol implementation or remove existing fork correctness fixes. It is not
an upstream submission or a replacement for the ongoing upstream integration
and audit work.

## Measured starting point

The current TheWire13 archive has a 19,244-byte `bsdsocket.library` and a separate
131,220-byte stack executable: 150,464 bytes combined. Beta8's micro library
contains its stack and is 204,168 bytes. These are uncompressed file sizes,
not resident RAM or equivalent feature coverage. TLS is separate in both.

ThreadX measurements used the pinned native toolchain, GCC 16.2.0b, and rebuilt
the full and micro libraries byte-for-byte identically to the published archive.
See [the machine-readable baseline](exec-threadx-size-baseline.json).

| Code attribution | Micro, bytes | Full, bytes |
|---|---:|---:|
| Surviving ThreadX core function spans, shipping LTO | 9,466 | 9,906 |
| Surviving Amiga port function spans, shipping LTO | 7,262 | 8,536 |
| Combined surviving spans | 16,728 | 18,442 |
| Retained ThreadX core sections, diagnostic without LTO | 10,204 | 10,792 |
| Retained Amiga port sections, diagnostic without LTO | 11,564 | 11,564 |
| Combined retained sections without LTO | 21,768 | 22,356 |

LTO function spans do not uniquely attribute inlined code and can include padding
or literals up to the next named text symbol. The non-LTO map attribution is exact
for that diagnostic build, not the shipping binary. Its combined ThreadX data/BSS
is 116/4,656 bytes; dynamic allocations and relocation metadata are excluded.
The largest surviving port function is the timer task at approximately 2.8 KB.

These numbers are a baseline, not a predicted saving or a strict upper bound.
A replacement has its own implementation cost. The 53,704-byte micro-to-TheWire
file-size gap cannot be assigned to ThreadX from these measurements.

## Proposed compatibility boundary

Initially retain the pinned upstream ThreadX headers and control-block layouts,
including the existing Amiga port extensions. Replace selected implementation
objects behind that contract in an explicit research configuration. Do not invent
smaller lookalike headers before establishing semantic compatibility.

NetX Duo needs more than the public `tx_*` API:

| Contract area | Examples and obligations |
|---|---|
| Public services | Threads, mutexes, event flags, semaphores, timers, sleep and time queries needed by enabled modules and AmiNetXDuo callers |
| Embedded types | `NX_IP` embeds ThreadX thread, mutex, event and timer objects; packet pools and sockets keep `TX_THREAD` suspension lists |
| Internal thread state | `_tx_thread_current_ptr`, `_tx_thread_system_state`, `_tx_thread_preempt_disable`, suspension links, status and cleanup callbacks |
| Wait and timer internals | `_tx_thread_system_suspend`, `_tx_thread_system_resume`, timeout fields and timer deactivation used by NetX cleanup paths |
| Port macros | Critical sections, initialization, tick rate, extensions and build/version-dependent cleanup signatures |
| Existing Amiga integration | Task adoption, per-caller state, nested Exec waits, shutdown and library lifecycle |

Examples are in `common/src/nx_tcp_socket_thread_suspend.c`,
`common/src/nx_tcp_socket_receive.c`, `common/src/nx_tcp_receive_cleanup.c` and
`common/inc/nx_api.h` inside `third_party/netxduo`. DHCP and AutoIP also include
ThreadX internal headers. Inventory the enabled core and add-ons separately from
disabled modules; a source-wide identifier count is not the linked dependency set.

The existing port already runs ThreadX threads on Exec tasks. Its baton, adoption,
wait and timer handling should be examined for reuse. Exec preemption alone does
not preserve NetX's global current-thread and critical-section assumptions.

## Behaviour that must be preserved

- Atomically publish a wait and release its protection, without losing a wakeup.
- Resolve arrival, timeout, cancellation and object deletion races exactly once;
  unlink waiters and preserve status, cleanup ownership and packet ownership.
- Preserve mutex recursion and any priority/preemption behaviour required by the
  actual consumers. Do not substitute success stubs for unsupported semantics.
- Preserve event consumption, multiple waiters, bounded and indefinite waits,
  timer units, callback context and timer cancellation.
- Support concurrent Exec callers and the stack's worker tasks without a stale
  global current-thread pointer, deadlocks or blocking under `Forbid()`.
- Preserve task adoption, nested waits, repeated start/stop, opener isolation,
  thread teardown and library expunge behaviour.

## Upstream update workflow

Maintain a checked dependency manifest of required symbols, accessed fields,
macros and semantic requirements for each enabled profile. Compile/link against
the real pinned headers and fail visibly when an update introduces an unsupported
dependency. Text scanning helps review but cannot prove semantic compatibility.

For each future NetX Duo update, compare the contract, compile the research
backend and run its regressions. New dependencies should be implemented and
tested in the compatibility backend. If a proposed design requires additional
OS-specific protocol edits, reconsider its boundary before proceeding.

## Experimental stages

1. Produce the dependency manifest and classify reusable existing port code.
   Establish which behaviours are exercised by the enabled profiles and tests.
2. Design the Exec primitives, current-thread model, wait ownership and timer
   lifecycle. Independently review the contract before replacing scheduler code.
3. Build an explicit experimental backend with an unchanged production backend
   available as the control. Keep it outside default presets and release packing.
4. Validate primitive races and lifecycle, then run NetX and socket regressions
   actually linked against the replacement. Passing tests against the original
   ThreadX implementation is not evidence for the replacement.
5. Compare code, relocation bytes, BSS, dynamic allocations, task/stack counts,
   throughput and latency using matching compiler, profiles and workloads.
6. Exercise an upstream update to assess integration cost. Report actual savings,
   correctness gaps and maintenance costs before any shipping proposal.

The initial experiments belong on host models and emulators. Live hardware or
release deployment is not part of this research authorization. A future shipping
decision requires a separate scope decision, independent review and complete
affected regression evidence; branch existence is not approval to integrate.

## Current evidence and open work

- Size baseline: measured; no source or hardware changes during measurement.
- Compiled-source full/micro dependency manifests and initial wait primitive:
  implemented in [research/exec-threadx](../../research/exec-threadx/README.md).
  The host model, m68k compilation and A1200 emulator smoke (four primitive
  checks, source `80e28dfd2`) pass. These are initial evidence, not a backend pass.
- Complete replacement design and minimum-profile manifest: pending.
- Replacement implementation, runtime correctness and net saving: unmeasured.
- AgentNet architectural and bounded implementation assessment: received from
  deepseek-v4; wait/adapter judged sound within task-level scope, no backend GO.
  Suspension/preemption, deferred cleanup and current-thread obligations are
  recorded in the research README. Its NetX pin differs from beta8, so it is
  architectural input. The subsequent implementation review covers `f0187a288`
  and `80e28dfd2`; extraction fixes and compiler regressions address its concrete
  header/member/marker/delta findings. Backend layout goldens, link validation
  and cleanup/scheduler binding remain open.

Primary references:

- [NetX Duo ThreadX type dependency](https://github.com/eclipse-threadx/netxduo/blob/master/common/inc/nx_api.h)
- [NetX TCP suspension implementation](https://github.com/eclipse-threadx/netxduo/blob/master/common/src/nx_tcp_socket_thread_suspend.c)
- [TheWire13 package and author README](https://aminet.net/package/comm/tcp/TheWire13)

## Spike 2 implementation checkpoint

The isolated research project now links actual pinned NetX TCP suspension and
cleanup/deferred-check code with actual ThreadX timeout/wait-abort, using the
bounded Exec bridge described in `research/exec-threadx/README.md`. Host schedules
cover publication, early wake, cancellation/timeout ownership, deferred lists,
stale expiry and identity restoration. The m68k second-task smoke compiles; its
native verdict and independent exact-commit review are pending. This remains
research only: no shipping backend selection, vendor edits, full scheduler
conformance or measured net savings.
