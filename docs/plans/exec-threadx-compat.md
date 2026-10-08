# Research: an Exec backend for the NetX Duo ThreadX contract

**Status: research only; wait primitive and bounded real-NetX TCP/UDP suspension bridge
implemented and native smokes verified. Full backend/conformance remains open.**
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
  header/member/marker/delta findings. Backend layout goldens, full replacement link validation
  and complete cleanup/scheduler binding remain open. The bounded spike 2
  implementation and review are recorded below.

Primary references:

- [NetX Duo ThreadX type dependency](https://github.com/eclipse-threadx/netxduo/blob/master/common/inc/nx_api.h)
- [NetX TCP suspension implementation](https://github.com/eclipse-threadx/netxduo/blob/master/common/src/nx_tcp_socket_thread_suspend.c)
- [TheWire13 package and author README](https://aminet.net/package/comm/tcp/TheWire13)

## Spike 2 implementation checkpoint

The isolated research project now links actual pinned NetX TCP suspension and
cleanup/deferred-check code with actual ThreadX timeout/wait-abort, using the
bounded Exec bridge described in `research/exec-threadx/README.md`. Host schedules
cover publication, early wake, cancellation/timeout ownership, deferred lists,
stale expiry and identity restoration. The m68k second-task smoke compiles and
passed all five native cases on one boardless A1200 run (38ff2cb0d, binary
71c27079, exit 0, stacks 8192). deepseek-v4 independently reviewed exact
38ff2cb0d + 9af3a1965: sound within bounded scope, no concrete blocker and no
full backend GO. This remains research only: no shipping backend selection, vendor edits, full scheduler
conformance or measured net savings.

The cleanup-guard follow-up 9af3a1965 passed the same five native cases (binary
e0404172, exit 0). Host additionally covers deferred-expiry then wait-abort:
the IP actor drains before releasing its boundary. The unresolved ordering
without that drain is rejected by a separate host guard probe. A general
non-IP deferred-abort dispatch policy remains open; the research backend must
not be enabled in production.

Independent review is complete for this checkpoint: deepseek-v4 found no
bounded bridge blocker at 38ff2cb0d + 9af3a1965, and claudecode found no blocker
in the host negative-test/CMake delta 9af3a1965..cd8d6a80a. Host CTest PASS 4/4
includes eleven bridge schedules and a separate rejection test, not twelve
successful cleanup schedules. The cd8d6a80 rebuilt native binary was identical
to the guarded binary actually run above. Task build/staging outputs were
removed after retaining small logs, hashes, binary and map evidence.

Review follow-up: claudecode identified that returning unsupported from blocking
mutex-get can be ignored by NetX. Research commit 8b724ee53 changes blocking
contention to fail closed, retains NO_WAIT semantics, adds a rejection probe,
and acquires/releases the real IP mutex in host/native producer fixtures.
Host CTest PASS 5/5 and native PASS 5/5 (ac40bcee, exit 0) at that commit.
The subsequent `6d9ff19d8` guard rejects mutex-get without registered thread
identity, with a third rejection probe. Final host CTest PASS 6/6, eleven bridge
schedules plus three rejection probes; final native PASS 5/5 at `6d9ff19d8`,
binary d7e229de (41,720 bytes), exit 0 after 14 seconds, same A1200/KS3.1/stacks.
Claudecode independently reviewed both follow-up deltas: no bounded blocker.
Source, binary hashes, stdout/exit and reviews are retained in
`/Users/turo/ai/evidence/exec-threadx-bridge-spike2.json` and adjacent evidence.
Research task build/cache and remote staging outputs are removed. Full backend,
general deferred-abort dispatch, UDP and measured savings remain open.

## Spike 3 checkpoint

Generic cleanup-gated wakeups are implemented without NetX edits. A non-IP abort
returns success, while physical dispatch of the ready target waits for actual IP
cleanup under a later boundary. Each wake carries a generation, detach rejects
pending gates, and a one-second research grace fails closed if cleanup stalls.
This is a scheduling adaptation; general ThreadX state/priority compatibility,
object deletion and packet-arrival races require further work.

Actual UDP receive/cleanup, checksum/packet-release helpers and ThreadX sleep are
linked. Host CTest PASS 8/8 includes 13 TCP schedules, 8 UDP schedules, 4 sleep
schedules and rejection probes; packet delivery is a fixture and checksum paths
are not exercised. The three-task native smoke passed 10/10 cases, exit 0 after 14 seconds on one
boardless A1200/KS3.1 run at `2ba6f0d6e`, binary a524f38b (47,160 bytes), stacks
8192. Both workers removed themselves before the parent returned. Exact-commit
independent implementation review of 2ba6f0d6e + f366e2361 is complete: bounded
sound/no blocker, with no full lifetime/scheduler GO. The small suspend-entry
invariant follow-up 96325636c was independently reviewed as correct/minimal,
with no bounded blocker, and passed the same ten native cases, exit 0 after
15 seconds, binary 4beada45 (47,228 bytes). All task build/cache and emulator
staging outputs were cleaned; small useful evidence is retained under
`/Users/turo/ai/evidence/exec-threadx-spike3.json` and adjacent logs/hashes.

Review confirmed the OPEN packet-arrival interaction: if an aborted-but-linked
thread is selected before deferred cleanup, NetX resume overwrites its status
with NX_SUCCESS. A full backend requires a policy for both status and packet
ownership; the cleanup gate alone does not solve it. One-second grace is a
fatal research diagnostic, never a safe total-drain/lifetime implementation.

## Spike 4 implementation checkpoint

The G4 receive-abort race is now reproduced by a fixture: the generic gated
baseline can return NX_SUCCESS and a packet after the abort caller returned
TX_SUCCESS. An explicit research NetX integration hook finishes actual receive
cleanup under the real IP mutex before backend READY/resume. It uses the saved
callback/control/sequence from the current suspension, leaves actual ThreadX
abort and vendor protocol sources unchanged, and preserves abort result and
fixture packet ownership in eight host schedules. Four rejection probes reject
stale/missing cleanup captures, invalid retained sockets and foreign mutex
contention. Host CTest PASS 13/13. Exact `e4a4db11a` passed the expanded native smoke
12/12, workers_reaped=2, exit 0 after 14 seconds on one boardless A1200/KS3.1
run, all stacks 8192, binary `190c9f27` (50,068 bytes). Actual evidence is in
`/Users/turo/ai/evidence/exec-threadx-spike4-native`; remote staging is removed.
deepseek-v4's exact `e4a4db11a` review confirms the implementation meets P1-P6,
removes G4 for opted-in receive aborts, and has no bounded fixture blocker.
No full backend/scheduler/wire/lifetime verdict. Local research build outputs
and remote staging are cleaned; useful exact-SHA evidence is retained.

This establishes a possible bounded integration boundary, not production
blocking/scheduler compatibility. Potentially blocking mutex acquisition cannot
remain inside resume/Forbid in a complete backend. Non-receive TCP waits, wire
processing, object lifetime, full replacement linking and size savings remain
open. Existing non-opted-in generic gating retains its known packet-arrival race.

## Spike 5 implementation checkpoint

Blocking NO_INHERIT mutexes now publish real ThreadX wait fields and use pinned
mutex cleanup for timeout/abort. FIFO handoff, recursion and owned-mutex lists
are implemented. An opt-in receive-abort wrapper acquires IP protection before
raw ThreadX abort mutates the target; the resume cleanup hook never parks.
Counted target pins prevent detach while an abort caller waits, and a separate
operation epoch detects a new suspension without treating cleanup-grace rearm
as a different NetX operation. Vendor and production sources remain unchanged;
only the standalone research compiler renames the raw abort body symbol.

Host CTest PASS 14/14 includes eight genuinely concurrent mutex schedules and
five contended NetX abort schedules plus retained models/guards. The expanded
17-case native smoke compiles. Exact independent review and actual emulator
verdict are pending. Priorities/inheritance, mutex deletion/owner termination,
safe socket storage lifetime, full wire/event/backend linking and size savings
remain open.
