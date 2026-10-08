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

Host CTest PASS 15/15 includes eight genuinely concurrent mutex schedules and
five contended NetX abort schedules plus retained models/guards. The expanded
17-case native smoke passed at exact `9cec1234d`, exit 0 after 15 seconds,
workers_reaped=2, one boardless A1200/KS3.1 run, stacks8192, binary `8b1a45f7`
(55,012 bytes). Host-only `0c48a9bb9` verifies policy fallthrough for actual
UDP/sleep/mutex waits. Control compilation confirms unchanged raw abort .text
and relocations after symbol rename. deepseek-v4's exact `9cec1234d` + `0c48a9bb9` + `56878cb89` review is complete:
correct/no blocker in bounded scope; no full backend/scheduler/lifetime verdict. Priorities/inheritance, mutex deletion/owner termination,
safe socket storage lifetime, full wire/event/backend linking and size savings
remain open.

The small foreign-release guard `56878cb89` rejects the unsupported upstream
thread-release retry path before mutation. A host negative case verifies actual
helper rejection with retained ownership; this is not successful termination.
The same 17 native cases pass at that commit, exit 0 after 15 seconds, binary
`dc73e1c5` (55,080 bytes), separate exact evidence retained and staging removed.

A separate exact-56878cb89 read-only review freshly verified host 15/15 and
identified integration priorities, now recorded in the research README: real
DHCPv6 delayed-suspend/abort/threshold stop-start, foreign Exec IO boundaries,
two simultaneous abort pins, actual NX_IP event/periodic timer/packet pool slice
with original-backend comparison, and resident/signal/priority-load economics.
The saved full manifest's wait-abort consumer is DHCPv6, not DTLS; the factual
README correction is applied. These are future obligations, not claims that the
unsupported APIs are already exposed. Owned builds and native staging are cleaned.


## Spike 6 event/timer checkpoint

The research branch now links unchanged pinned ThreadX event get/set/cleanup,
with prepublication call-boundary guards, and actual application timer
create/activate/change/delete bodies. A flat application countdown dispatches
actual NetX periodic callbacks from an explicit marked task timer context,
independently of private thread wait deadlines. Deactivation preserves remaining
ticks. Event snapshots/clear, single/multiple waiters, timeout/abort and callback
self/other cancellation are covered. Callback create/activate/change/delete and
nested ticks fail closed; only event set and deactivation are supported.

Exact a7d6196cb passes host 20/20 and expanded native 21/21, workers_reaped=2,
exit 0 after 15s, one A1200/KS3.1 r40.68 run with 8192-byte parent/worker stacks,
binary 008689e7 (60,880 bytes including fixtures/runtime). Actual stdout, startup,
runner exit, binary hashes and native zero staging files remaining are retained separately.
deepseek-v4's independent exact a7d6196cb review is complete: no blocker in
bounded scope, all 18/18 parts read, no full backend GO. Owned local build/edit
scripts and remote native staging are removed; small useful evidence retained. This is a fixture of IP helper boundary ordering plus the actual
periodic callback, not the full helper loop. No automatic common timer task,
elapsed-time catch-up, callback lifecycle/drain, full wire/packet ownership,
replacement link or net code size savings are established. Production/vendor
sources remain unchanged. Next: real packet pool/receive ownership and IP helper
integration compared with the original backend; then lifecycle and economics.

## Spike 7 packet ownership checkpoint

Exact research 586278ae3 links unchanged pinned packet pool lifecycle/allocation/
cleanup/release, copy/append and UDP delivery/receive sources. Host 25/25 includes
seven concurrent allocation schedules, copy/chain recovery across two pools,
actual queued/direct UDP delivery, and four deliberate prepublication/quiescence
rejections. Quiescent deletion is supported; deletion with waiters or outstanding
packets is deliberately rejected, not counted as a successful lifecycle case.

Native 28/28 passed at that commit, workers_reaped=2, exit 0 after 15s, one boardless
A1200/KS3.1 r40.68 run, all stacks 8192 bytes, binary bf8b5e86 (70,284 bytes including
runtime and fixtures). Seven new native cases include actual IPv4 UDP valid/bad
checksum handling and bad-checksum packet release waking a pool allocator while
receive continues. The independent byte-wise fixture creates packet bytes;
NetX performs checksum validation and ownership transfer. Input starts at UDP
dispatch after synthetic IP decoding; socket binding is manually initialized.
There is no link-driver/full IP/wire/socket-lifecycle verdict. LP64 host checksum
is disabled; target evidence is kept separately. deepseek-v4 independently
reviewed exact 586278ae3: no blocker in bounded scope, all 20/20 parts read,
no full backend GO. Owned local build and native staging are removed; useful
small evidence is retained. Two terminal ICMP transmit sentinels cover absent routing
and transmit, never return success, and must be rejected by the future full
replacement link gate. Shipping/vendor inputs remain unchanged.

## Current progress and next runtime milestone

| Area | Current evidence | Remaining work |
|---|---|---|
| Dependency boundary | Saved full/micro pinned compiler inventories and real-header m68k probe | Full replacement link gate and ABI/layout goldens |
| Waiting mechanisms | Bounded host/native TCP, UDP, sleep, mutex, event and periodic callback cases | Delayed suspend, priority/threshold and foreign Exec IO compatibility |
| Packet ownership | Real pool allocation/release and UDP transfer; native IPv4 checksum handling | Complete IP/helper path, TCP ownership, IPv6/chained checksum and socket lifecycle |
| Runtime lifecycle | Owned Exec startup/normal-return/reap with retained stacks; native 11/11, 15 tasks reaped | Public ThreadX create/terminate/delete, mutex/event deletion, independent common timer task and drain |
| Economic result | Shipping component spans and standalone fixture sizes recorded separately | Comparable finished library link, resident resources and throughput |

Next executable milestone: provide owned Exec task creation and teardown, plus
object deletion needed by actual NetX IP create/delete. At the pinned source,
`nx_ip_create.c` calls thread create with AUTO_START after creating mutex/events,
creates the periodic timer, and changes a registered creator's threshold.
`nx_ip_delete.c` stops timers, terminates the helper, deletes mutex/events, then
deletes the thread. Retain source/control storage until producers and the task
are truly reaped; no silent priority or forced-termination success. Reuse the
existing port's task publication/stack-retirement guarantees without accidentally
retaining its ThreadX scheduler. Then boot a real NX_IP helper with a disposable
fake link driver, compare traces with the original backend, exercise repeated
shutdown/restart, and measure complete replacement/library/resource cost.

## Spike 8 owned task checkpoint

Exact `101d52195ac467d1b6c5f0e8c35ae57b2cb7c33a` adds a research Exec launcher,
separate from public ThreadX services. It publishes caller-owned Task/stack and
entry descriptors before AddTask, then waits outside every bridge boundary for
owner-side IO/registration startup ACK. Entry runs inside one outer bridge
context. Normal return checks bridge quiescence, detaches and closes private IO;
FINISHED publication and RemTask occur under one uninterrupted Forbid interval.
Creator-only reap releases the ACK signal and permits storage reuse.

The retained host models pass 25/25, including idle-query assertions across
nested boundaries. They do not execute native lifecycle. The new m68k smoke
passes 11/11 on one boardless A1200/KS3.1 r40.68 run: 15 tasks reaped, 12 restarts,
exit 0 after 14s, all stacks 8192 bytes. Actual guest output, startup, exit and
hashes were read/verified under `/Users/turo/ai/evidence/exec-threadx-spike8-native`;
claudecode confirms zero staging left. Binary `b2930fd6`, 50,344 bytes including
fixtures/runtime, is not a library size result. deepseek-v4's independent exact
source review is complete: all 19/19 parts read, no blocker in bounded scope,
no tests run and no full backend GO. Host and native execution remain separately
attributed to root and claudecode. Owned local/native build staging is removed.

This proves the owned task mechanism only: fixed Exec priority zero, no public
thread create/terminate/delete contract, no TX_COMPLETED state, no forced
termination, no full NX_IP. Private IO startup failure rollback is implemented
but not injected in this run; ACK allocation failure is exercised. Producers
must already be quiescent when entry returns, and storage is retained until reap.

The public integration needs a different publication contract: actual
`nx_ip_create` calls AUTO_START thread creation from inside its existing boundary,
then publishes extensions/timer/IP ID and created-list membership. This launcher
deliberately rejects blocking startup there. Do not route that call straight to
the synchronous research start or silently return success on deferred IO failure.
Preserve already initialized public TX_THREAD fields when binding the new owner,
provide a real publication/start gate and explicit allocation rollback, and handle
the creator's threshold contract. On shutdown, actual `nx_ip_delete` terminates
the helper before deleting its mutex/events/thread; helper event waiting must be
cleaned and retired without returning into its endless loop or releasing storage
while a producer still has it. Complete those service/lifetime boundaries before
claiming real IP create/delete or producing an economic comparison.

## Spike 9 public creation boundary checkpoint

Exact `a87f95f58f4ef1032e899cd43797796f3f2aec53` adds a native-only reserved-worker
implementation of the pinned public thread create/resume/delete signatures.
Preparation opens private IO in the real owner and parks it on SIGF_SINGLE while
leaving the public control block untouched. Creation consumes that reservation,
initializes/preserves the actual TX_THREAD fields, binds its runtime owner, updates
the circular created list and signals AUTO_START inside the caller's boundary.
The owner cannot enter until the outer boundary releases. DONT_START remains
suspended until explicit initial resume. No constructor call waits for startup.

Normal completion removes the runtime binding and retains the public ID with
TX_COMPLETED. Private IO close and native no-gap FINISHED/RemTask follow; only
then may creator-side public delete remove the ID/list/native pointers, free ACK
and allow caller-owned storage reuse. Unbound reservation cancellation reaps
without public control mutation. Domain holds reject runtime reset with retained
reservations, including completed but undeleted records.

Host 27/27 passes: retained models plus field-preserving binding, duplicate/
foreign/active/owned-mutex/pinned retirement guards, completed ID and reserved
reset rejection. Native 11/11 passes, 9 tasks reaped including six public restarts
and one unbound cancellation, exit 0 after 14s, one boardless A1200/KS3.1 r40.68
run, parent/child stacks 8192 bytes. Actual output/startup/runner/hashes were read
and verified under `/Users/turo/ai/evidence/exec-threadx-spike9-native`.
Binary `4bbcf5a4`, 57,132 bytes including fixtures/runtime; native staging is
removed and local owned build is cleaned. Independent deepseek-v4 read-only
source review is complete (all 19 parts): no blocker in this bounded scope.
The reviewer ran no tests; host execution is root's, native execution claudecode's.

Final code `d3720c81826960b046e44c4aecfa6ad4ff49ceb3` corrects the initial
31-minus-priority mapping, which would place logical IP priority 2 at Exec 29.
Logical 0..15 now map to Exec 1, 16..31 to Exec 0, capped at the production
port's priority; ties are deliberate and logical control fields are preserved.
Static profile guards require review if the pinned priority configuration changes.
Host 27/27 and the m68k cross/startup gate pass on this final code. Its separate
native run passes 12/12, 10 tasks reaped, six restarts, exit 0 after 14s, including
actual logical-priority-2 creation/entry/completion/deletion at Exec priority 1.
Machine, stacks and harness match the baseline. Root verified actual stdout,
startup, runner and all evidence hashes in
`/Users/turo/ai/evidence/exec-threadx-spike9b-native`; binary `62efc96f`, 58,048
bytes including fixtures/runtime. Independent source review of the small delta
is complete (all eight parts): no blocker in the bounded scope. Both source
verdicts and both exact native receipts are retained separately. Final owned
local build and native staging are removed.

The partial non-LTO standalone map attributes 11,520 bytes of backend .text,
2,528 bytes of retained pinned ThreadX bodies, plus 3,300 bytes of clock division
helpers. `/Users/turo/ai/evidence/exec-threadx-spike9-code-cost.json` retains exact
per-object attribution and limitations. This excludes fixtures/startup, other
runtime sections/allocations and missing backend/NetX components. It cannot be
subtracted from shipping LTO spans or treated as finished net library savings.

Strict ThreadX scheduling conformance is not established by the capped Exec
priority bands. Unequal thresholds/nonzero slices are rejected
with TX_FEATURE_NOT_ENABLED before public mutation. Actual NetX IP creation's
hardcoded one-tick slice remains unsupported. Unreserved allocation, general
delayed suspend/resume, threshold changes, forced termination, mutex/event
retirement, common timer ownership/drain and full IP/helper/link remain open.
Child IO-open and AddTask failure rollback exist but are not injected here.
Bound but never resumed DONT_START threads cannot yet be reclaimed; forced
termination remains open. The research gate uses Exec's reserved SINGLE bit;
its ownership must be reconciled with future full integration. Defining the
created-thread globals here also requires excluding conflicting ThreadX core
initialization from a future complete replacement link.
Next: resolve the scheduling policy for the actual NetX creation arguments and
creator threshold window; implement helper stop/drain and object retirement,
then validate unchanged pinned IP create/helper/delete with a disposable driver
and compare actual library/resource cost with the original backend. Shipping
and vendor inputs remain unchanged; the research branch stays unmerged.

Before enabling the real IP constructor, guard its entire supported backend
contract. The pinned `_nx_ip_create` does not check the return of
`tx_thread_create`; merely returning TX_NO_MEMORY/TX_FEATURE_NOT_ENABLED from
the backend can leave it publishing an NX_IP ID with no valid helper. A future
research integration wrapper must reject unsupported/missing reservations
before calling the unchanged raw constructor, and verify actual helper/task/timer
creation and entry evidence afterwards. An IP ID or NX_SUCCESS alone is not a
successful runtime verdict. This source observation is a pending integration
obligation, not a vendor fix or a shipping change made by spike 9.

## Spike 10 scheduling policy checkpoint

Exact `37776d2651c6c85f3e414c759de70e2c8436c72e` accepts NetX's nonzero
helper slice as logical metadata, with no ThreadX tick countdown or global Exec
quantum change. Initial threshold still must equal priority. Public
preemption_change supports only the current registered owner under an unmarked
serialized call, returns the old user threshold before successful mutation,
validates against user priority and rejects inheritance. Foreign threshold changes
remain unsupported. time_slice_change stores both logical slice fields and
returns the prior configured slice under the same serialized boundary.

The outer context_end rejects a raised threshold before releasing Forbid.
Nested contexts retain the outer lock. Actual event suspension may release it
with the threshold retained, then reenters before the waiting caller continues;
that owner must restore before returning from its outer call. This bounded policy
provides serialization, not a ready-list scheduler or dispatch-race preference.
No claim is made that a woken sender outruns the producer or other Exec tasks.

Root host 31/31 passes, including actual pinned event suspension and three
separate fatal guards (outside-context service, raised-threshold outer exit,
unsupported inheritance), with retained earlier models. m68k cross/startup
gates pass. Claudecode's single boardless A1200/KS3.1 r40.68 run passes 14/14,
10 tasks reaped, six restarts, exit 0 after 14s; parent/child stacks 8192,
harness `300b22e8`. Root read actual stdout/startup/runner and verified all hashes
under `/Users/turo/ai/evidence/exec-threadx-spike10-native`; binary `4cba8af3`,
60,168 bytes including fixtures/runtime. Native cases check priority-2/slice-1
creation, creator publication and threshold restoration before entry, worker slice
changes, raised-threshold event blocking and protected restoration after resume.
Independent deepseek-v4 read-only source review is complete (all 19 parts):
no blocker in this bounded scope. The reviewer ran no tests; execution is
attributed separately to root and claudecode. Review confirms all pinned NetX
threshold callers target their current thread, matching this restriction.
Owned local build and native staging are removed.

The exact standalone non-LTO map now attributes 11,880 backend .text bytes,
2,528 retained ThreadX bytes (14,408 subtotal) plus 3,300 clock division helper
bytes; per-object evidence is `exec-threadx-spike10-code-cost.json` in the evidence
directory. This is still a partial fixture link, excluding missing integration,
runtime sections/resources and unrelated helpers. It establishes no finished
library savings or performance improvement.

Next integration obligations remain: validate all unchecked raw constructor
service contracts before publication; run actual pinned IP helper/driver code;
implement safe owner stop/drain and mutex/event retirement; supply common clock
ownership and a complete replacement link. Pinned `_nx_ip_delete` calls terminate,
object delete and thread delete while its outer serialized boundary and a
preemption-disable increment are retained. Waiting there for another Exec owner
to close private IO/remove itself is unsafe. A separately reviewed integration
protocol must establish owner retirement before that unchanged raw deletion path
and preserve real cleanup/created-list/packet/resource semantics. Source inspection
identifies this obligation; no full IP create/delete verdict is claimed here.

AgentNet's read-only proposal is retained as
`/Users/turo/ai/evidence/exec-threadx-ip-delete-proposal.txt` (including the
accepted root corrections). An alternative is a wrapper with preflight checks,
real logical upstream deletion under one boundary, then private owner retirement
and ACK outside it; the wrapper returns only after actual native removal.
That alternative is unimplemented and unreviewed. Before adopting it, complete
the private wait generation and reap IO (a raw signal cannot end a pending
anx_wait_run), unlink all producer/runtime references before logical control
deletion, and provide an owner terminal path before any post-wait dereference
of the deleted control block. Keep ordinary public thread-delete's proven
FINISHED-before-clear contract; any deferred retirement needs a narrowly gated
integration path. Refuse raw deletion bypass before mutation, retain IP/control/
stack/private storage through ACK, and prove no late touch by repeated poisoned
storage reuse. No real driver IO or hardware testing is included in this proposal.

## Spike 11 event-worker pre-stop checkpoint

Exact `972c5f418254d0f64b80c907b10e349a2def1797` implements a bounded
creator stop command for an entered worker genuinely parked on the exact event
object. Preflight requires actual cleanup/sequence/list membership and pending
generation, no owned mutex, abort pin or pending wake, and one normal outer
serialized context. It can remove the real event waiter and timer while a
preemption-disable increment is held. READY, wrong-object, self/foreign creator,
owned, pinned, pending-wake and nested requests refuse before mutation.

The accepted stop marks TX_TERMINATED, invokes unchanged event cleanup, removes
the runtime binding and clears its private public-control pointer before
completing the private wait DELETED. After wait returns, the suspend loop checks
only private terminal state under protection; it releases that protection and
enters the no-return owner callback before restoring any ThreadX frame/identity
or reading the public control. The callback closes IO as its real owner and
publishes FINISHED/ACK/RemTask without a gap. Public ID and caller-owned storage
remain retained until native ACK plus ordinary creator delete. There is no native
longjmp and no deferred logical deletion or arbitrary forced task removal.

Root host 33/33 and m68k cross/startup gates pass. The host terminal escape uses
setjmp/longjmp only as a fixture, not Exec lifecycle/ABI proof. Native stop 9/9
passes, four tasks reaped, two genuinely submitted timer IO requests reaped and
three restarts, with poisoned control/stack reuse only after ACK/delete. The
retained public-thread 14/14 fixture also passes on this code (10 tasks reaped,
six restarts) because all workers now install the terminal hook. Each was one
boardless A1200/KS3.1 r40.68 run by claudecode, exit 0 after 14s, parent/child
stacks 8192, harness `300b22e8`. Root read actual stdout/startup/runner and verified
all binary/map/evidence hashes in the separate spike11 stop/thread native evidence
directories. Stop binary `c008e803`, 54,128 bytes; thread binary `bb61fc1f`, 61,644
bytes, both including fixtures/runtime. Independent deepseek-v4 read-only source
review is complete (all 22 parts): no blocker in this bounded pre-stop scope.
The reviewer ran no tests; root host execution and claudecode native execution
are separate evidence. General termination and actual full IP remain open.
Owner-written timer sends/reaps counters establish lifecycle
balance and block close with an unreaped request; they are not performance data.

The partial non-LTO stop-fixture map attributes 13,140 backend .text bytes,
2,528 retained ThreadX bytes (15,668 subtotal) plus 3,300 clock helper bytes;
per-object evidence is `/Users/turo/ai/evidence/exec-threadx-spike11-code-cost.json`.
These costs exclude missing integration, fixtures/runtime sections/resources and
other helpers, and cannot establish finished library savings against shipping
LTO spans. The owned local build and both native staging paths are removed after
retaining exact artifacts/logs.

This pre-stop mechanism keeps the already tested FINISHED-before-public-delete
contract and can support a future IP wrapper that stops/drains its helper before
entering raw deletion. That wrapper still needs a lifetime gate across its wait
and all external packet/driver/API/timer producers quiesced. At spike 11, object retirement
was still missing: research mutex/event creation did not maintain upstream
created lists, so raw deletion could not simply be linked. Preserve real
object IDs, list/count and owned/suspended invariants when adding it. Raw mutex
creation also installs `_tx_mutex_thread_release`; linking that body needs care
to avoid accidentally pulling a full ThreadX scheduler/forced-release path.
The unchanged real IP constructor/helper/delete and driver remain untested.

## Spike 12: quiescent object retirement

Code 05cd6c7592c29862d3dc63b863c378c8722c3c75 adds real mutex/event created
rings/counts, duplicate/membership validation and guarded unchanged pinned raw
deletes. Busy/owned/suspended or active captured wait references refuse deletion
unchanged; normal serialized context is required. Runtime reset now rejects live
objects after thread detach. Existing fixtures explicitly delete their objects.
The shared object probe checks head/non-head/singleton removal, recursive owner
refusal, invalid/duplicate operations, actual mutex/event behavior, preserving
preemption-disable, and post-delete poisoned storage recreation. Queued mutex
and blocked event refusal are also exercised through real wait cleanup paths.

Host CTest 38/38 and m68k -Werror/startup-first gates pass. Claudecode's one
boardless A1200/KS3.1 r40.68 run per artifact passes native stop 13/13 with four
workers reaped, two genuine in-flight timer requests reaped, and three object
recreations after native FINISHED/ACK/public delete. Retained thread passes 14/14,
tasks_reaped=10, restarts=6. Each exits 0 after 14s; parent/child stacks 8192,
harness 300b22e8.
Root read actual stdout/startup/runner and verified all binary/map/evidence hashes
under /Users/turo/ai/evidence/exec-threadx-spike12-stop-native and thread-native.
Independent deepseek-v4 read-only exact review is complete (all19 parts), no
blocker in bounded retirement, no tests run by reviewer. The
notification-config review statement was corrected against the port and
conditional safety-critical guard. The current port compiles notification
callbacks out; no callback runtime support is claimed. Source/native evidence
is separate.

Partial stop-map non-LTO .text: backend 14,372 plus retained ThreadX 3,112 =
17,484 bytes; clock division helpers add 3,300. This excludes the shared test
probe and other fixtures, startup/libc/other helpers/data/BSS/relocations/resources
and missing full integration. No finished-library savings or speedup is measured.

Owned build and both native staging paths are removed after retaining exact
evidence. The research branch remains unmerged. Full ring scans are O(N) per
operation; no performance benefit is assumed.

This closes the bounded primitive retirement gap only. Actual raw IP creation/
helper/delete still needs its lifetime gate, producer drain, native acknowledgement
outside the raw delete boundary, a restricted already-finished terminate adapter,
and complete preflight before ignored raw service results can mutate state. Full
NetX helper/driver/common-clock integration and the performance A/B remain open.

## Spike 13: real IPv4 IP helper lifecycle

Research code 123b95af60961e318fb63a38d142dfee00a58d7e links unchanged pinned
NetX system initialization, IP constructor/helper/delete and their selected IPv4
dependencies. Final code de148d5fd813ad6f80cefda54a3a78c0270eee65 adds a
whole-wrapper-record stack-disjointness guard and regression: the native prepare
already protects its embedded helper record, but the enclosing IP metadata also
needs protection before any reservation or constructor mutation. The IP target uses a separate bridge variant with packet-slice
ICMP traps excluded; its ICMPv4 error generator is the actual vendor body.
This is a selected helper dependency link, not the full replacement library.

The lifecycle wrapper requires an attached creator outside every boundary,
a prepared native worker, an otherwise empty ThreadX object domain, one IP and
primary interface, a retained idle packet pool, and a synchronous driver. The
experiment uses 192.0.2.1/24 and excludes external raw API/packet/driver/clock
producers. It is not a production lifetime gate over all exported NetX APIs.
Enabled protocols, queued packets, raw/ping/pool waiters, fast timers and extra
interfaces are outside the delete contract. The wrapper revalidates after worker
preparation before raw constructor memset, and checks actual constructor outputs
and restored creator threshold rather than trusting NX_SUCCESS alone.

Direct raw create/delete bypasses are refused before mutation. Internal permits
are consumed before vendor calls, so a driver callback cannot recursively delete.
Deletion checks actual socket counts and the quiescence contract before stopping
the helper at its exact event wait. It closes the experiment's event producer
gate and deactivates the actual IP timer under the same boundary, waits for native
FINISHED outside all boundaries, then revalidates before admitting unchanged raw
IP deletion. Public tx_thread_terminate acknowledges only an already native-
FINISHED, privately stopped TERMINATED owner; it cannot terminate a live worker.
Public thread deletion still requires native retirement first.

The new native fixture exercises actual IP startup/driver callbacks, a real UDP
socket blocking deletion, IP mutex ownership refusal, a real READY helper refusal
and retry, driver-deferred processing and helper self-delete refusal, the actual
periodic timer callback/helper event using explicit manual ticks, and three real
create/delete cycles with poisoned IP/stack reuse. Closing callbacks try raw
recursive deletion and the event producer gate; both must refuse. Packet-pool,
created-list, signal and stack recovery are checked. These are lifecycle proofs,
not wire, automatic clock or general production-driver coverage.

Root retained host CTest passes 38/38; the new native integration is not modeled
on the host. m68k -Werror/startup-first link gates pass. Deepseek-v4 completed
independent source review, all 24 parts, with no blocker in the bounded initial
implementation plus final overlap delta. The review ran no tests and gives no
production approval. Claudecode ran each exact artifact once on boardless A1200/
KS3.1 r40.68 with parent/child stacks 8192 and harness 300b22e8. Actual IP passes
13/13, helpers_reaped=3, restarts=2, exit 0 after 15s; retained stop passes 13/13,
tasks_reaped=4, timed_io_reaped=2, restarts=3, object_restarts=3, exit 0 after 14s.
Root read actual stdout/startup/runner and verified all binary/map/evidence hashes
under /Users/turo/ai/evidence/exec-threadx-spike13-ip-native and stop-native.
Initial IP artifact 1b77d435 is 74,336 bytes; stop cdbc211d is 59,884 bytes
including fixtures/runtime. Final IP artifact 789844ed is 74,800 bytes and passes
14/14, helpers_reaped=3, restarts=2, exit 0 after 14s at exact de148d5fd.
Claudecode ran it once on the same boardless A1200/KS3.1/8192-stack setup;
root independently verified actual stdout/startup/runner and all receipt hashes
under /Users/turo/ai/evidence/exec-threadx-spike13b-ip-native. Retained stop is
byte-identical on the final source, but its executed coverage stays attributed
to 123b95af. Host, source review and native execution are separate evidence.

The review notes caller-owned pool/caller disjointness as an experimental
precondition and the manual vendor source closure as a link-time dependency
check; neither blocks this checkpoint. Full review is retained under
/Users/turo/ai/evidence/exec-threadx-spike13-review.log. The owned local research
build and all three native staging areas are removed; small evidence remains.

The final partial non-LTO IP fixture map attributes 17,316 backend .text bytes
and 3,112 retained ThreadX bytes (20,428 subtotal), 10,260 selected NetX bytes,
and 3,300 clock helpers separately. Fixtures/startup/libc/other helpers/data/BSS/
relocations/native resources and missing full integration are excluded. Evidence:
/Users/turo/ai/evidence/exec-threadx-spike13b-code-cost.json. No finished-library
size saving or performance benefit follows from these partial non-LTO totals.

Next gates: protocol packet/wire coverage, a real common clock with producer
shutdown/drain, broader API and driver lifetimes, full replacement-library link
and equal-feature A/B size/performance. No hardware or shipping integration.

## Spike 14: automatic common application clock

Source checkpoint: 0e5f99347bfd55454e353d938ecb9d5f68f9e7cc, against reviewed
spike-13 documentation 4b3e3234a. Research sources only; vendor/public headers,
shipping backend selection, main and hardware are unchanged.

One raw Exec task at priority zero owns an AnxExecWait timer.device request,
port and wake signal. It has no public ThreadX thread/control block or scheduler
baton. The domain reservation remains held from task publication through native
join. Startup waits outside serialization for owner-side IO readiness; failed
startup releases its task/ACK/reservation. A native FINISHED ACK is published
under Forbid immediately before RemTask, with no scheduling gap.

The pinned 50-Hz clock is driven from monotonic EClock absolute phase. Due ticks
advance the existing application-timer dispatcher in a marked task boundary;
callbacks retain actual pinned timer and NetX periodic bodies. A boundary emits
at most eight overdue ticks, then permits dispatch before processing more. No
tick is skipped and the phase is not reset to the current time. This does not
advance private suspension timers: those already use absolute elapsed-time
wait deadlines. Interrupt entry, timeslicing, arbitrary blocking callbacks and
storage retirement inside callbacks remain unsupported.

IP integration is optional and creator-owned. Both clock record and clock stack
must be disjoint from the retained IP record/control, pool/control/arena, caller
and helper stack. Only this associated clock is admitted; external raw API,
packet and other driver producers remain excluded by the bounded experiment.
Delete preflights clock authority before changing helper state, then closes the
helper and clock gates and deactivates the actual IP timer under one boundary.
The clock completes any pending private wait; its owner aborts/reaps outstanding
IO, closes its own device/port/signals and retires. Creator joins clock and helper
outside all boundaries before revalidation and unchanged raw NetX IP deletion.
Successful deletion owns associated clock retirement; do not independently join
or reclaim an associated clock while its IP is live.

The new native smoke checks automatic unchanged periodic callback/helper wake,
marked callback and foreign stop refusal, single-domain clock publication,
startup ACK allocation failure, overlapping IP/clock storage refusal and native
metadata. A deliberate 250ms Forbid fixture stall exercises elapsed-time catch-up
and retained phase. IP deletion starts from an actual pending private timer IO,
checks clock/helper ACK before raw delete callbacks, and poisons freed IP/stacks
before observing no late ticks/callbacks. Three whole IP/clock cycles must recover
created counts, packet pool, signals, canaries and the held runtime. The retained
manual-clock IP smoke covers its existing supported lifecycle separately.

Root host CTest passes 39/39: retained bridge models plus absolute-phase,
bounded-backlog and extreme-arithmetic tests. Native clock/IP integration is not
host modeled. m68k -Werror and startup-first link gates pass. Claudecode ran
both exact 0e5f99347 artifacts once on boardless A1200/KS3.1 r40.68 with
parent/child stacks 8192 and harness 300b22e8. Automatic clock passes 16/16,
clocks_reaped=3, helpers_reaped=3, restarts=2, exit 0 after 19s; retained IP
passes 14/14, helpers_reaped=3, restarts=2, exit 0 after 14s. Root independently
read actual stdout/startup/runner and verified all receipt hashes under
/Users/turo/ai/evidence/exec-threadx-spike14-clock-native and ip-native.
Automatic-clock artifact 12be0e54 is 78,936 bytes; retained IP artifact 4c2abb05
is 77,312 bytes, including fixtures/runtime. Deepseek-v4 completed all 20
independent source-review parts plus closing receipt on the exact implementation,
with no actual blocker in the bounded single-clock scope and no production GO.
The reviewer ran no tests. Full review is retained in
/Users/turo/ai/evidence/exec-threadx-spike14-review.log. Representable microsecond
uptime, two-layer disjointness and the retained ACK idiom are noted limitations.
Native execution, host models and source review are separate evidence.
Owned local research build and both native staging areas are removed; small
useful evidence remains under /Users/turo/ai/evidence/exec-threadx-spike14*.

The retained IP partial non-LTO map attributes 19,392 backend .text bytes plus
3,112 retained ThreadX bytes (22,504 subtotal), 10,260 selected NetX bytes and
3,300 clock division helpers separately. Includes research lifetime guards and
diagnostics; excludes fixture/startup/libc/other helpers/data/BSS/relocations and
native resources. No finished-library size saving or performance claim follows.
Equal-feature clock/protocol maintenance is required for the later A/B comparison.

Next gates: real protocol traffic, socket/driver concurrency and lifetimes, full
replacement-library integration and equal-feature size/performance comparison.

## Spike 15: real IPv4 UDP/TCP loopback traffic

Implementation: 01f214881fc9c2efe67bd72bc9fab1e1d8c017e3, against reviewed
spike-14 checkpoint 3bf732f7d. Research sources only; pinned vendor/public headers,
shipping backend selection and hardware are unchanged.

The protocol target compiles unchanged NetX common sources into an archive and
extracts their actual dependency closure. The existing IP bridge supplies its
reviewed wait/packet/ThreadX services; TCP additionally links unchanged pinned
ThreadX tx_thread_info_get, used for the IP helper's logical priority. There are
no successful terminal protocol stubs. The start-group archive order resolves
shared receive/packet/cleanup symbols from research_ip_bridge first; these use
unchanged pinned bodies with the existing guarded compiler aliases. The
file-scoped ip_create/delete and packet_allocate/pool_delete renames apply to
both vendor archives. That extraction order and source-file scope are part of
the fixture link contract, not independent replacement definitions.
A private compiler-included header gives
only this disposable vendor fixture a deterministic NX_RAND/NX_SRAND PRNG. It
provides controlled test input, not production entropy or a security verdict.
The SYN-cache handshake-millisecond dependency reads the live automatic clock's
real monotonic EClock through its retained device; it is not a fabricated tick.

The synchronous driver establishes a primary TEST-NET IPv4 interface. Actual
NetX internal loopback builds/copies/checksums packet headers, queues packets to
the real IP helper, and transfers ownership through UDP/TCP services. Any physical
packet-send driver request fails the fixture; no external peer or wire claim.
The real automatic clock runs periodic and TCP fast timers. It must start
before TCP SYN-cache services can consult the live handshake clock. For the
final negative retirement tests the parent temporarily uses Exec priority2,
above helper1 and clock0, so neither owner dispatches between the negative
checks and the real stop boundary. The native ACK wait admits their retirement;
the parent restores its original priority afterward. This is fixture scheduling,
not a production priority or performance result.

Only the protocol target opts into ANX_REAL_PROTOCOL_LINK. Its deletion
preflight admits exact pinned UDP and consistent TCP receive/queue/periodic/fast/
deferred-cleanup handlers, with no sockets, listeners, active/accepted SYN-cache
entries, queued packets or retained pool ownership. TCP requires the actual
second timer and exact two-member created ring. Unknown/partial handlers still
refuse before stopping owners. It does not clear enabled protocol callbacks to
make the old gate pass. Both real IP timers are deactivated under the same stop
boundary, clock/helper native retirement is acknowledged outside serialization,
and unchanged raw IP delete retires both timers and all created objects.
Ordinary retained IP/clock targets keep their strict protocol-free gate.

The new guest exercises three UDP payload lengths (129, 513, 1300 bytes) with
actual blocked receives and packet-content checks. Corruption changes one byte
of the real vendor-generated queued copy without repairing its checksum; actual
UDP checksum rejection and timeout cleanup must recover ownership. TCP performs
SYN/SYN-ACK/ACK with blocked server accept, bidirectional 193-byte data, a chained
2048-byte send with segmentation and complete stream-content checks, receive
timeout/deferred cleanup, actual automatic fast/one-second maintenance, and an
orderly FIN exchange. The passive helper callback sends the real FIN without
blocking itself; active disconnect waits for its actual completion. Unbind,
unaccept, unlisten and delete must return all ACK/data packets. Unknown protocol
handler and outstanding-packet retirement are refused. Two whole protocol/IP/
clock cycles reuse poisoned socket/IP/stacks and recover signals, canaries,
created rings, pool and runtime.

Root retained host CTest passes 39/39; new native protocol integration is not host
modeled. m68k -Werror and startup-first gates pass, including the unchanged common
core archive at 01f214881. The first native protocol run failed after four
completed cases at the compound corrupt-UDP check; its original artifact
34eeedb4 (122,544 bytes) and failure receipts are retained. The actual pinned
receive loop releases a rejected packet, then returns a timed error without
clearing the caller's output pointer (nx_udp_socket_receive.c:229-233, 394).
Fixture-only correction 25e94b05e660f958a3476e43973822c41e5829c1 separately asserts
NX_NO_PACKET, exactly one checksum error, waiter cleanup, full packet-pool
recovery and a subsequent empty no-wait receive with null output. Error output
is never dereferenced. No backend/vendor behavior or checksum gate is changed.
The corrected protocol artifact c7ec2dce (123,080 bytes) passed 19/19,
helper2/clock2/restarts1, exit0 after17s. Root verified actual stdout/startup/
runner and every receipt hash. Deepseek independently confirmed the correction
against pinned receive/cleanup bodies in all9/9 source-review parts; no blocker.
The original failure remains retained as a fixture false negative, not a pass.

The retained clock artifact 6755c907 (78,940 bytes) passed 16/16 on the original
01f214881, exit0 after18s, and is byte-identical after the fixture correction.
Root verified actual stdout/startup/runner and all local receipt hashes. Both
original runs and the corrected protocol run used a boardless A1200 KS3.1 r40.68, parent/helper/clock stacks8192,
harness300b22e8 and -t90. Root read the complete deepseek full source review
25/25 plus correction9/9; no blocker in this bounded scope. Native execution is
separate from that read-only review. Owned build outputs and native staging were
removed; exact artifacts, original failure, hashes, cost attribution and useful
receipts are retained. The unmerged research branch remains preserved.
Evidence is retained under /Users/turo/ai/evidence/exec-threadx-spike15*;
queued work is not a pass.

The partial non-LTO protocol map attributes 19,788 backend .text bytes plus
3,260 retained ThreadX bytes (23,048 subtotal), 42,032 selected NetX bytes and
3,300 clock division helpers separately. Research guards/diagnostics are
included; fixture PRNG/handshake/driver/tests, startup/libc/other helpers, data/
BSS/relocations and native resources are excluded. Not a full replacement-library
link, net saving or performance result. No subtraction from shipping LTO spans.

Next gates: concurrent socket/API and asynchronous driver lifetimes, external-peer
wire coverage, full replacement-library integration and equal-feature size/
performance comparison. IPv6 remains unproved by this IPv4 experiment.

## Spike 16: concurrent consumers and task-context driver ownership

Implementation c46af5bddba02a663435c7952ccea1a3952e18d6, against reviewed
spike15 checkpoint 02f9edbfb. Research sources/private records only; shipping
selection, pinned vendor bodies and public NetX/ThreadX headers remain unchanged.

The real-protocol link admits one registered public native task producer after its IP
clock starts. AnxExecIpIo retains a domain hold and blocks IP deletion even when
its single TX slot is empty. Open/receive/complete/close require its exact native
owner; driver accept may run in any normal serialized driver call. Marked
callbacks/ISRs are excluded. Record storage is checked against IP/pool/creator/
helper/producer/clock records and stacks plus the pool arena. Final tightening
114d9027d uses the backend registry accessor to require the current public BOUND
worker, its bridge identity and native Task. The full private producer record
(Task/bridge/wait) is checked before reading or writing io state. An attached
non-public ExecTask is refused; its native lifetime is absent from the created-
thread preflight. Both this refusal and a private-wait-record alias are tested
before normal driver enrollment. The producer must
drain its actual device IO before closing; this is an explicit caller contract,
not automatic SANA-II IO cancellation. Successful close clears the association
and drops the domain hold. Public worker IDs remain until native ACK and actual
ThreadX delete, so the old created-object preflight still prevents premature IP
retirement. Pool/storage reclamation remains quiescent only.

Two non-wrapping token counters distinguish the producer lease generation and
each accepted transmission. A wrong/closed generation refuses before accessing
IP or packet storage. A completion additionally matches its operation token
and exact outstanding packet; the test releases and reallocates the same actual
pool address, accepts the new ownership, then proves that the old completion
cannot free it. A failed accept/receive retains caller ownership. Accepted TX
completes via unchanged nx_packet_transmit_release; received packets enter the
unchanged deferred IP receive queue. This is one outstanding packet and one
task producer, not arbitrary interrupt drivers or a complete device IO layer.
The retained IO record must outlive rejected late calls and retain its counters.

The guest drives two distinct public native UDP consumers into the actual FIFO
receive suspension list. A simulated driver retains actual vendor-generated
broadcast IPv4 datagrams, then a separately scheduled native owner copies a
packet, completes TX and injects RX. The packets keep real NetX headers/checksums;
no fabricated receive result or external peer is used. Each consumer checks all
193 payload bytes and returns actual ownership. The test also aborts a real
blocked UDP receive through unchanged ThreadX/UDP cleanup, refuses a second
abort, and waits for native completion before public deletion/storage reuse.
Exhausting the actual pool parks a third operation, then actual release hands
ownership directly to its allocation waiter. All packets must return.

Foreign-owner, busy-slot, busy-close and live-producer IP shutdown attempts
refuse. The producer closes before native ACK/delete, sockets and its control
event group retire, then real IP/helper/clock shutdown proceeds. Late TX/RX/event
operations refuse after IP/worker/socket storage poison. Two whole cycles test
old lease generation refusal after reopening and recover signals/canaries,
created lists, all pool ownership and runtime holds. All three application
worker records use the existing reservation/normal completion/public delete
path; no forced worker termination or raw object-count patching.

Retained host models pass 39/39; this new integration is native-only. All m68k
research targets, unchanged vendor core, -Werror and startup-first gates pass.
At c46af5bdd the retained protocol passed 19/19 and clock 16/16, each one
boardless A1200 KS3.1 r40.68 run with harness 300b22e8, -t90 and all stacks8192.
Root verified actual stdout and receipt hashes. The first IO guest failed after
six completed cases at reap: its helper-stop assumption required bridge.thread
NULL, whereas unchanged normal complete removes the runtime-list binding but
retains the thread pointer and public ID. The original failure and illegal-
instruction log are preserved; the illegal PC could not be mapped without a
load base. Returning from a failed CHECK with live owners can unload their code,
so no crash-root-cause inference is treated as execution proof.

Fixture-only 22404ec6c0757675c877a949637e7d5292d445f6 corrects reap to require the
retained pointer, TX_COMPLETED/public ID, native ACK, closed private wait and
no timer/cleanup/owned-mutex/pending-wake/abort-pin state, then actual public
delete. The aggregate reap assertion is split; no backend/vendor body changes.
Protocol and clock are byte-identical after correction. Corrected IO 18/18
passed at 22404ec6c, workers_reaped10/helper2/clock2/restarts1, exit0 after15s;
no illegal/guru/alert line was reported in the retained emulator log. Root
verified actual stdout/startup/runner and all receipt hashes. This proves the
corrected bounded lifetime run; it does not prove the specific old illegal PC
cause. At final 114d9027d the owner IO guest passed 18/18, workers10/helper2/clock2/
restarts1, exit0 after15s, including the unregistered-task and private-record
alias refusals. Retained protocol passed 19/19, exit0 after17s; clock 16/16,
exit0 after19s. Each was one boardless A1200 KS3.1 r40.68 run, harness 300b22e8,
-t90, all stacks 8192. Root verified actual stdout/startup/runner/all receipt hashes;
no illegal/guru/alert line was reported. Original and intermediate SHA results
remain distinct from these final artifacts. Independent full source/correction/
owner-supplement review is complete: root read all 6/6 core sections (23 wrapped
chunks), owner 2/2 (6 chunks) and receipt 3/3. Deepseek confirms the corrected
normal-completion predicate and final enrollment/private-record protections;
no blocker in tightened114d9027d. Source review is distinct from execution.
The reviewer used NetX 0260419617; root and deepseek independently confirmed
all 10 reviewed bodies/header byte-identical to the actual built/pinned 2d871dca.
The complete 3/3 pin addendum also confirms ThreadX review at pinned e24aa9c9;
the vendor-drift observation is closed. The local NetX/ThreadX working files and
gitlinks match these research pins. Spike16 is complete within this bounded scope.
Final owned build and all native staging were removed, retaining useful exact
evidence. Original source gaps/failures are not relabeled as original passes.

Caller contracts remain explicit: output-token storage must be disjoint from
control/packet storage, and an accepted packet must actually be caller-owned.
The accept gate checks its pool but does not verify its allocated-vs-free marker;
the actual tested driver TX handoff supplies allocated ownership. Tokens fail
closed at their 32-bit limits. Single producer/one outstanding TX and single-CPU
serialized FIFO are bounded assumptions, not arbitrary driver concurrency.
Artifact/source hashes, original failures and coverage stay distinct in
/Users/turo/ai/evidence/exec-threadx-spike16.json; pending is not a pass.

The final owner-gate protocol partial non-LTO map attributes 20,876 backend .text bytes
plus 3,260 retained pinned ThreadX bytes (24,136 subtotal), 42,032 selected pinned
NetX bytes and 3,300 clock division helpers separately. Includes research guards
and diagnostics; excludes fixture/test/driver/PRNG/handshake/startup/libc/other
helpers, data/BSS/relocations and native resources. No finished-library size
saving or performance claim; no subtraction from shipping LTO spans.

Remaining integration includes real device/peer/ISR boundaries, concurrent TCP
and public socket APIs, IPv6/other enabled protocol lifetimes, and the full
replacement-library link. These belong in the library integration checkpoint;
the later equal-feature size/performance comparison remains required.

## Required performance comparison after functional integration

The user explicitly requests a general performance comparison as well as size,
including the possible benefit of removing the scheduler baton and associated
synchronization. The experiment has removed the explicit ThreadX scheduler/run
baton in its tested paths, but still serializes NetX through Forbid and retains
wait signals, timer IO, mutex/event operations and wakeup cleanup. Fewer handoff
mechanisms do not establish faster execution; per-thread timer IO and broad
critical sections can introduce different costs.

Build both backends against identical pinned NetX sources/configuration, compiler
and optimization settings. Use the same driver, peer, MTU, packet pools, stack
sizes, traffic pattern and enabled protocol features. Record both source SHAs,
images, machine/CPU/emulator settings and actual Exec priority policies. Keep
functional coverage and required periodic work equal: a prototype missing its
common timer task must not be compared as an idle-CPU improvement. Run repeated
A/B trials and retain raw measurements; instrumentation cost should be separated
from the uninstrumented throughput/latency runs.

| Workload | Measurements |
|---|---|
| Idle with the full clock/protocol maintenance running | CPU use, tick/wakeup frequency, resident tasks/ports/signals/timer requests and memory |
| Sustained UDP send/receive, small and MTU-sized, IPv4/IPv6 | Payload throughput, packets/s, CPU per byte/packet, loss and latency |
| TCP bulk transfer and small request/response | Throughput, latency distribution/tail, CPU, retransmissions and packet-pool pressure |
| Contended sockets and producer-to-waiter handoff | Wake/resume latency, fairness, task switches, signals, waits and synchronization operations |
| Repeated create/close and cancellation | Lifecycle latency, peak resources and full resource recovery |
| Network load alongside ordinary Exec tasks | Responsiveness/starvation evidence and sensitivity to priority policy |

Compare image/resident code size and per-thread resources separately. Count the
original baton handoffs and scheduler signals and the replacement's boundary,
signal, wait and timer operations at equivalent API/workload points. Include
uncontended and contended cases rather than extrapolating from primitive loops.
Use a controlled emulator configuration for reproducible functional/instruction
comparisons; host wall-clock emulator runtime is not an Amiga performance number.
Real-hardware measurements need a separate coordinated, non-destructive run once
the prototype is ready; this plan does not start hardware work or deploy it.

Current lifecycle smoke results prove only their bounded correctness contracts.
No throughput, CPU, latency, synchronization reduction or finished-library size
improvement has been established yet.
