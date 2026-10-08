# Exec / ThreadX compatibility research

This directory is an isolated, standalone CMake project. The parent build,
shipping presets and vendor sources do not select it. It is not a complete
ThreadX backend and must not be installed on a machine as one.

## What is implemented

- A dependency capture/check tool using each production build's compiler and
  preprocessor flags. It inventories calls, internal identifiers, structure
  fields and source macros/types. ThreadX headers, both Amiga ports' headers and
  `nx_api.h` are fingerprinted, including critical-section/context macros.
- Full and micro compiled-source contract baselines, including enabled NetX
  modules, AmiNetXDuo consumers and the existing Amiga port. These are an
  over-approximation of the linked dependencies, not an implementation checklist
  of every ThreadX feature or proof of semantic compatibility.
- A compile-only m68k contract probe against the real pinned ThreadX/NetX headers.
- A wait primitive that publishes under protection, completes once, retains an
  absolute deadline across spurious wakes and rejects stale generation tokens.
- A native Exec adapter using `Forbid`/`Permit`, dedicated task signals and
  `timer.device`, with IO abort/reap and owner-only resource teardown.
- A deterministic host model that exercises pre-notification, the check-to-wait
  window, simultaneous arrival/expiry, spurious wakes, cancellation, deletion,
  platform errors, reuse and generation/deadline bounds.

The cleanup hook in the primitive is a queue-removal hook under protection;
the bridge binds NetX's ThreadX cleanup callbacks separately. It must not reenter
the primitive. Producers must be quiesced before freeing the object or closing
the adapter. Generation tokens cannot make freed objects safe to access.

## Reproduce the host model

From the repository root:

```sh
cmake -S research/exec-threadx -B build/exec-threadx-research/host -DCMAKE_BUILD_TYPE=Release
cmake --build build/exec-threadx-research/host --parallel 4
ctest --test-dir build/exec-threadx-research/host --output-on-failure
```

## Compile the native adapter and real-header contract probe

```sh
cmake -S research/exec-threadx -B build/exec-threadx-research/m68k \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchain-m68k-amigaos.cmake"
cmake --build build/exec-threadx-research/m68k --parallel 4
```

Cross compilation is not a native runtime pass. The adapter has not been linked
into a NetX stack. `research_exec_wait_smoke` is a standalone Amiga executable
for later emulator testing of the native timer, pre-notification, cancellation
and resource teardown. It does not test cross-task races or NetX behaviour.

The first A1200/Kickstart 3.1 boardless attempt, using the toolchain's startup
and newlib printing, crashed with no guest output on the harness's 8192-byte
stack. It provides no adapter runtime verdict. The smoke now uses the existing
command startup plus DOS `Write`/`Flush`, with the usual startup-first map check.
The stack ceiling was unchanged on the successful rerun below.

## Verified first-spike results

- Host model and real-compiler extraction regressions: PASS for the deterministic
  schedules, new member names and marker failures listed above.
- Full/micro dependency baselines: UNCHANGED against beta8; a negative check
  with the suspension reference removed from the expected usage was rejected.
  This does not test removal of its backend implementation. A changed expected
  port-header fingerprint was also rejected with its filename in the delta.
  A compiler fixture captured an expanded service and new member name and
  excluded inactive code/header declarations.
- m68k: real-header probe, adapter and smoke compilation/link PASS; the command
  startup-first map gate PASS.
- Native smoke: PASS, `research_exec_wait=PASS checks=4/4`, guest exit 0 after
  14 seconds on one boardless A1200 run. Kickstart 3.1 r40.68, stack 8192 bytes,
  `tools/amiberry-run.sh -m A1200 -t 90`, harness commit `300b22e8`. AgentNet
  owner claudecode reported the actual guest result on 2026-10-07.

Native smoke source: `80e28dfd2e5b27fbb693abbce45398231c74adbf`.
Binary SHA256: `62fc279e1b5c98047aa90a2ec358c117f57319c005d23538f50ce0f27f2e011c`.
The four checks cover timeout/pending-close rejection, pre-notification,
cancellation and final close. They do not exercise a second task, NetX or a
replacement ThreadX backend. The 29,960-byte standalone executable includes
startup/runtime code and is not a replacement library size measurement.

## Check the dependency boundary after an upstream change

```sh
cmake --preset micro -B build/exec-threadx-research/contract-micro
python3 research/exec-threadx/contract.py \
  --database build/exec-threadx-research/contract-micro/compile_commands.json \
  --profile micro --check research/exec-threadx/contract-micro.json
cmake --preset default -B build/exec-threadx-research/contract-full
python3 research/exec-threadx/contract.py \
  --database build/exec-threadx-research/contract-full/compile_commands.json \
  --profile full --check research/exec-threadx/contract-full.json
```

Use `--output PATH` instead of `--check PATH` to capture a reviewed new baseline.
An update that changes a pin, header, per-file reference or aggregate reference
fails the check. Review the actual changes and their semantics before updating.
This tool reads sources and preprocesses them; it does not compile or run NetX.
It checks consumer usage and header contents, not the existence of backend
function bodies. A separate full link against the replacement is required to
detect missing implementations. It rejects flags that suppress macro expansion
or source markers, and fails if a source's own marker is never found. Field
extraction includes any `tx_*` member so a newly named object type is visible.
Changed pins, headers and consumer filenames are printed for review.

## Suspension obligations identified after spike 1

AgentNet's architectural assessment identified the following obligations. They
have been checked against the beta8 sources, but are not satisfied by the
standalone primitive or independently reviewed as an implemented backend.

- NetX publishes a circular ThreadX suspension list, cleanup callback, control
  block, `TX_TCP_IP` state and `tx_thread_suspending` under `TX_DISABLE`.
  It increments `_tx_thread_preempt_disable` before releasing its IP mutex.
  Suspend/resume must balance that counter and honour a resume before blocking.
- Timeout, resume and wait-abort must arbitrate cleanup ownership and timer
  cancellation. ThreadX mutex/event cleanup uses suspension sequences; NetX TCP
  cleanup instead uses its cleanup pointer, including a deferred sentinel.
  This primitive's generation token does not implement either contract.
- Deferred TCP cleanup runs again on the IP thread. Its loop follows the removed
  waiter's own `tx_thread_suspended_next`, so unlinking must retain that link.
  Test multiple deferred waiters, simultaneous close/timeout and list counts.
- Direct accesses to `_tx_thread_current_ptr`, `_tx_thread_system_state` and
  `_tx_thread_preempt_disable` require a reviewed identity/context model.
  The adapter's task-level `Forbid` is not interrupt protection and cannot stand
  in for ThreadX scheduling/preemption semantics. Inspect the existing port's
  context bridge and adoption logic before replacing it.
- The saved full compiled-source manifest records `_tx_thread_wait_abort` in
  `addons/dhcp/nxd_dhcpv6_client.c`; micro coverage does not. DTLS receive is a
  source-level example, not the consumer recorded by this manifest. Compiled coverage includes modules that may
  not survive final linking. An absent micro dependency cannot retire a full
  profile obligation.

The assessment used NetX `02604196` and ThreadX `e24aa9c9`; this experiment uses
beta8 NetX `2d871dca` and the same ThreadX pin. That assessment is architectural
input, not an exact-commit review of this implementation. No backend GO exists.

deepseek-v4 subsequently reviewed implementation commits `f0187a288` and
`80e28dfd2`: the primitive and adapter were judged sound for their task-level
scope, with no replacement-backend GO. The review's extraction gaps are addressed
by broader header/member capture, mandatory source markers and detailed deltas.
Spike 2 binds real NetX cleanup separately from the primitive's protected
queue-removal hook, as described below.

## Existing port code to evaluate for reuse

The current port already maps ThreadX threads to Exec tasks. Reusing its
lifecycle guarantees matters more than reusing every implementation line.

| Existing source in `port/threadx-amiga/src` | Candidate reuse and constraint |
|---|---|
| `tx_amiga_adopt.c` | Caller registration, adoption generations, owner-allocated signal bits and orphan handling. Its calls into ThreadX creation/termination must be replaced or retained deliberately. |
| `tx_thread_system_return.c` | Park/teardown handshakes and zombie handling. Its current-thread baton tests are tied to the existing scheduler and cannot be copied into an independent Exec scheduler unchanged. |
| `tx_thread_context_save.c`, `tx_thread_context_restore.c` | Serialized timer context and `_tx_thread_system_state` accounting. The tick is an Exec task using `Forbid`; nothing in that protected callback region may wait. |
| `tx_thread_schedule.c` | Baton dispatch and waiter wakeup ordering are the reference behaviour. Removing ThreadX ready lists still requires a reviewed substitute for caller identity and wake ordering. |
| `tx_initialize_low_level.c`, `tx_timer_interrupt.c` | Timer clock advancement, callback dispatch and shutdown are the common-timer reference. The spike's private timer per wait does not yet replace this machinery or establish its memory/task cost. |

The next design should state which of these responsibilities it retains and
measure the resulting code, signals, timer requests, tasks and stacks. A wait
adapter compiled in isolation cannot establish the net replacement cost.

## Spike 2: bounded suspension bridge

`tx_bridge.c` implements a deliberately small set of internal ThreadX services
behind the actual pinned headers and control blocks. It links unchanged NetX
TCP suspend/resume, receive cleanup, deferred cleanup and the other cleanup
routines reached by the deferred checker, plus unchanged ThreadX timeout and
wait-abort. Vendor sources and production builds are untouched.

An explicit task call boundary holds `Forbid` while NetX executes. Each frame
publishes its caller identity and system state. Blocking drops exactly the
outer boundary, clears the global caller/frame and balances the preemption
counter; returning reacquires it and restores the original frame. Every test
producer (arrival, timeout, close and abort) enters the same boundary. Host and
native IP actors acquire/release the actual IP mutex around their operations. The marked
timer context uses the actual port's `TX_TIMER_PROCESS_IN_ISR` system-state
convention, but executes in task context; real interrupt callers are unsupported.
Nested frame teardown checks owner and LIFO order.

The bridge owns the internal active-timer list and generation-tagged private
wait. Resume deactivates that timer and completes the wait; native parking reaps
its IO before returning. Timeout invokes actual ThreadX cleanup outside the
primitive's queue-removal hook. TCP cleanup really defers to the IP producer;
a timed-out caller waits indefinitely for that cleanup to resume it. It cannot
return with a live NetX suspension node. Expiry rejects stale/repeated tokens.

Wait-abort preserves upstream ownership: ThreadX changes the state to
`TX_SUSPENDED` and records `TX_WAIT_ABORTED`; NetX unlinks without resuming in
that state. ThreadX then performs the resume and returns `TX_SUCCESS`. This is
linked-source evidence, rather than an emulated cleanup callback.

Implemented services are limited to the suspension/timer seam, identity,
non-inheriting uncontended/recursive mutexes, and event creation/set without
waiters. Blocking mutex contention fails closed because NetX often ignores mutex-get
status; returning an unsupported error would let it proceed without the lock.
`TX_NO_WAIT` contention returns `TX_NOT_AVAILABLE`. Unsupported event waiters
fail closed. Missing services remain missing link symbols.
`_tx_thread_system_preempt_check` only defers Exec dispatch to the boundary's
`Permit`; it does not implement ThreadX ready queues, priorities or thresholds.
The deterministic post-mutex-release seam is NULL in the native experiment.

Host CTest executes eleven bridge schedules: nested caller/timer identity and mutex
ownership/nonblocking contention, resume before blocking, arrival, timeout with
actual deferred checker, close/repeated cleanup, wait-abort, expiry then arrival,
two deferred waiters, stale expiry after reuse, pending foreign detach, and
wait-abort after deferred expiry (IP actor drains cleanup before its Permit).
The last ordering demonstrates that the upstream deferred sentinel is a no-op
when wait-abort invokes it. The bridge rejects returning with a live cleanup
pointer; a general deferred-abort dispatch policy remains open. A separate negative
CTest executes that unresolved ordering without IP draining and verifies the
guard rejects it. That rejection is not a successful abort/cleanup verdict.
Host ULONG/layouts differ from m68k and are not ABI evidence.

`research_tx_bridge_smoke` compiles for m68k using the existing DOS command
startup and a second 8192-byte Exec task. Its five planned native cases are
arrival, real timeout/deferred cleanup, close, wait-abort and injected expiry
then arrival. Worker resources are closed by their owner and the worker removes
itself before the parent can release the executable. Compilation and one boardless A1200 native run are verified: PASS 5/5,
guest exit 0 after 14 seconds, Kickstart 3.1 r40.68, parent/worker stacks 8192.
Tested source `38ff2cb0d62a2d4f0cb205052a1f36e0e83d9346`; binary SHA256
`71c27079f84438ca88c7f3f6fd7d1c2cd4c1377d73569e7df1920ddc81386581`,
41,424 bytes. The cleanup-guard follow-up at `9af3a1965` also passed the same
five native cases, exit 0 after 14 seconds, with binary SHA256
`e0404172a3b188c720abee24cc0ab28320a46e31b365bebefe87575d1c836c9f`,
41,488 bytes; its separate evidence is `exec-threadx-bridge-native-guard`.
The eleventh deferred-abort schedule and rejection probe are host-only; they
were not additional native cases. Harness `tools/amiberry-run.sh -m A1200 -t 90` at `300b22e8`;
AgentNet owner claudecode saved actual stdout, exit and startup evidence under
`/Users/turo/ai/evidence/exec-threadx-bridge-native`. This executable includes
runtime and test fixtures; its size does not measure a replacement library. These fixtures
exercise suspension machinery, not complete TCP/socket APIs or packet delivery.

Independent AgentNet review by deepseek-v4 covered exact `38ff2cb0d` and
`9af3a1965`: sound for the bounded scope, no concrete correctness blocker, no
full backend GO. It confirmed the real timer-context discriminator and
wait-abort ownership. `_tx_thread_system_resume` deliberately relies on its
producer to unlink and clear cleanup; clearing it inside resume would hide the
deferred-abort node still attached. The guard rejects that unresolved ordering. Claudecode independently reviewed
the host-test/CMake delta `9af3a1965..cd8d6a80a` and found no blocker; the negative
test correctly checks rejection, rather than claiming the unresolved ordering
works. Claudecode also identified NetX ignoring blocking mutex-get errors;
`8b724ee53` rejects blocking contention and makes the host/native IP actors hold
the IP mutex. Claudecode reviewed that fix and its thread-identity guard `6d9ff19d8`: no
bounded blocker. The final native binary passed the same five cases. Final host CTest PASS 6/6 covers the primitive model, eleven real-source bridge
schedules, extraction and three separate rejection probes (deferred-abort without
a drain, blocking mutex contention, and mutex get in marked timer context).
Rejection results are protection evidence, not successful unsupported operations.
The final source `6d9ff19d8` passed five native cases, exit 0 after 14 seconds:
binary SHA256 `d7e229defb15101aa2a170f786200d5005a050ddf81962b06f6d19a07d12316e`,
41,720 bytes, same boardless A1200/KS3.1 configuration and 8192-byte stacks.
Actual evidence is `/Users/turo/ai/evidence/exec-threadx-bridge-native-final`.
The tests still exercise the suspension seam, not a complete stack. No library
size saving has been established.

## Spike 3: cleanup gating and UDP receive

The bridge now keeps a resumed owner parked while its cleanup pointer is live.
The unchanged ThreadX abort returns TX_SUCCESS to the abort caller and leaves
TX_READY in the control block; physical Exec dispatch waits for actual cleanup.
This changes wake timing and is research policy, not complete ThreadX scheduling
compatibility. Registered callers must use the serialized boundary; general
ThreadX thread-state queries, priority semantics and adoption are still absent.

An outer context-end checks each pending wake independently before Permit.
It signals only a READY thread whose cleanup was actually cleared. Pending wakes
carry the private wait generation; the owner reties that token atomically when
it arms the bounded cleanup wait. Detach rejects a pending wake/token. Owners,
waits, sockets and all producer references must remain alive until quiesced.
A one-second research grace bounds missing/stalled cleanup; expiry is a fatal
failure, never a fabricated API return or forced removal of an invalid node.
A host negative case retains socket storage but invalidates its ID, runs the real
checker and verifies that the stalled gate fails closed. This is not safe socket
freeing or deletion conformance.

Host TCP coverage adds a distinct non-IP abort caller, a later IP cleanup boundary
and two gated owners released one at a time. The former deferred-abort rejection
case is now a successful delayed-cleanup schedule; cleanup/list/status assertions
remain. Normal and delayed expiry, generation reuse and ownership checks remain.
Actual NetX UDP receive and cleanup are linked with real checksum/packet-release
helpers and real ThreadX sleep. No vendor edits or success stubs are introduced.

UDP host fixtures cover queued receive, empty/no-wait, arrival, direct timeout,
abort, unbind/repeated cleanup, two timeout waiters and stale expiry after reuse.
Delivery list manipulation is a fixture, not `_nx_udp_packet_receive` or wire
routing. IPv4 fixtures use the existing per-socket checksum-disable option;
checksum and packet-release branches are linked but not exercised. Host ULONG
and header sizes differ from m68k; these are semantic schedules, not wire/ABI
conformance. Sleep host coverage executes actual zero wait, timeout, abort and
illegal timer-caller paths. Host CTest PASS 8/8: TCP 13 schedules, UDP 8, sleep 4,
primitive/extraction and three rejection probes (stalled cleanup, blocking mutex,
timer-context mutex).

The expanded m68k smoke compiles with three real Exec tasks: owner, IP producer
and independent abort caller. Ten executed cases are the original five TCP cases,
delayed TCP cleanup after the independent abort, UDP arrival/timeout/abort and
real ThreadX sleep. Both workers own and reap their resources and remove their
tasks before the parent releases the program. Actual native PASS 10/10,
workers_reaped=2, guest exit 0 after 14 seconds on one boardless A1200/KS3.1 r40.68
run; parent and both workers use 8192-byte stacks. Source `2ba6f0d6e`, binary SHA256
`a524f38bcd7f31ad10c92b1f1b122a8441c431ac393e7e076eb933a8276c790c`, 47,160 bytes.
Harness `tools/amiberry-run.sh -m A1200 -t 90` at `300b22e8`; actual stdout,
startup and exit evidence are `/Users/turo/ai/evidence/exec-threadx-spike3-native`.
Implementation review and follow-up status are recorded below.
Independent review of exact `2ba6f0d6e` + host follow-up `f366e2361` found no
blocker in the bounded schedules. It confirmed per-wake/generation arbitration,
detach protection and completion before Permit. The recommended suspend-entry
assertion rejecting a stale pending flag/token is added at `96325636c`; the
independent follow-up review found it correct/minimal, with no bounded blocker.
The guarded binary passed the same ten native cases, workers_reaped=2, exit 0
after 15 seconds, same A1200/KS3.1/stacks. Binary SHA256
`4beada458c1f1663a708b361b5f7ea0ea4b2ae08bcb3e4e081862dace79fe0c9`, 47,228 bytes;
actual evidence is `/Users/turo/ai/evidence/exec-threadx-spike3-native-guard`.
Task build/cache and emulator staging files were cleaned after retaining evidence.

Source review confirms the remaining arrival race: the gated thread is still
linked, and `_nx_tcp_socket_thread_resume` can overwrite TX_WAIT_ABORTED with
NX_SUCCESS if it selects that node. The already-READY bridge resume preserves
the pending flag and later releases that changed status. This is OPEN and is not
covered by the ten native cases. The full backend needs an ownership/dispatch
policy for it; restoring only status would not resolve packet ownership.
Production also needs safe total cleanup/lifetime and producer-boundary exits;
the research grace is not a recovery or production latency guarantee. This spike does not establish a full NetX stack or net size savings.

## Spike 4: synchronous deferred receive-abort cleanup

`netx_resume.c` adds an explicit, owner-configured integration hook on registered
research callers. The backend saves the actual cleanup callback, control pointer
and suspension sequence when arming each wait. Before making a resumed target
READY, it calls the hook after balancing the preemption counter. The hook accepts
only normal registered caller context, TX_SUSPENDED/TX_WAIT_ABORTED, the NetX
deferred sentinel, a matching saved receive cleanup/control/sequence and a valid
retained socket. It acquires the real IP mutex, calls the original
`_nx_tcp_receive_cleanup`, verifies that cleanup cleared with abort status/state
preserved, and releases the mutex. The actual ThreadX abort and all NetX vendor
sources remain unchanged. No callback pointer, list removal or status is forged.

The actual cleanup sees TX_SUSPENDED, so it removes the node/count without its
TX_TCP_IP timeout-status/resume branch. Backend resume can then wake the owner
normally. A later packet cannot select that aborted node. This fixes the bounded
receive-abort interaction by removing ownership before READY, rather than
repairing status after packet delivery. Without this explicit hook the spike-3
gate still has the documented race; the host baseline deliberately reproduces
NX_SUCCESS plus packet delivery following an abort that returned TX_SUCCESS.
That baseline reproduction is not a successful compatibility verdict.

Host coverage adds eight integration schedules: single aborted waiter/packet
remaining queued, aborted head or tail with another live waiter, both waiters
aborted, recursive mutex ownership, arrival winning before abort, ordinary abort,
and TCP/sleep/TCP reuse refreshing the saved callback. Four separate rejection
probes cover stale sequence, missing saved callback, invalid socket ID with
retained storage, and foreign mutex contention. Total host CTest PASS 13/13 also
retains previous primitive/TCP/UDP/sleep/extraction and guard coverage. Packet
queue/delivery is a fixture matching the transfer in
`nx_tcp_socket_state_data_check.c`; it does not execute TCP wire/header/checksum
processing, full public receive, packet-pool release or socket lifetime deletion.
The decoded payload is delivered once to a live waiter or retained on the queue.

The m68k smoke adds two native cases to the prior ten: independent non-IP abort
followed by packet arrival, with and without the abort caller already owning the
IP mutex. The receiver may wake before the later producer; the parent waits
outside the serialized boundary with a bounded poll before checking queue
ownership. At exact `e4a4db11adcacdb9a1a727bf8a8b4e87897229cd`, one boardless
A1200/KS3.1 r40.68 run passed 12/12 cases, workers_reaped=2, exit 0 after
14 seconds, all stacks 8192. Binary SHA256
`190c9f27ebad5c25208ff8b425aac3b946351d8c3d46df755cc2fd44f211ed53`,
50,068 bytes; harness `tools/amiberry-run.sh -m A1200 -t 90` at `300b22e8`.
Actual stdout/exit/startup/hash evidence is
`/Users/turo/ai/evidence/exec-threadx-spike4-native`. Remote staging is removed.
deepseek-v4 independently reviewed exact `e4a4db11a`: P1-P6 implemented as
advised, G4 removed for the opted-in receive-abort path, no blocker in bounded
fixture scope. No full backend/scheduler/wire/lifetime verdict. Owned local
research builds and remote staging were removed after retaining small evidence.

The new `netx_resume.c` helper has 224 bytes of m68k object text and no static
data/BSS with the current research compiler/flags. That excludes backend capture
and hook-dispatch additions, per-thread metadata, relocations and test/runtime
costs; it is not a net replacement-library saving. The smoke executable also
includes all fixtures and runtime. No full-library size result exists.

This is not the production blocking-mutex design. Boundaries already serialize
these research operations. Mutex acquisition inside resume supports only
uncontended/recursive ownership; foreign contention fails closed. A full backend
must move a potentially blocking acquisition outside resume/Forbid and implement
its scheduling/lifetime policy. Only TCP receive cleanup is supported by the hook;
other TCP wait classes and real ISR callers are rejected when cleanup remains.
The deferral event flag can remain set after synchronous removal: the later real
deferred checker finds no such node, harmlessly. Socket/control/producer storage
must stay alive; ID checking is not use-after-free protection. One-second gate
grace and rejection paths are fatal research diagnostics, never recovery.

## Spike 5: blocking mutexes and abort acquisition ordering

The NO_INHERIT mutex backend now supports FIFO blocking acquisition, recursion,
atomic ownership handoff and actual `tx_thread_owned_mutex_list/count` bookkeeping.
It publishes real mutex wait fields and uses the unchanged pinned
`_tx_mutex_cleanup` for timeout/cancellation. Final put assigns ownership before
resuming the head waiter; NO_WAIT contention still returns TX_NOT_AVAILABLE.
Detach rejects owned mutexes. Priority inheritance/priority queues, mutex deletion
and forced owner termination remain unsupported. The cleanup TU also defines
`_tx_mutex_thread_release`, but link presence is not lifecycle conformance; it is
not exercised or supported as a successful foreign-owner release path by this
backend. Follow-up `56878cb89` rejects preemption-disabled foreign put before
mutation, so an accidental thread-release call cannot spin on NOT_OWNED. Its
host rejection invokes the real upstream helper and verifies retained ownership.

Blocking get is allowed only in an outer registered normal-task boundary with
preemption enabled. The existing suspend bridge drops that boundary before
parking and restores it on wake. Calls from a resume hook, a nested blocking
context or with preemption disabled fail closed before publishing a waiter,
since NetX ignores many blocking-get errors. The old blanket contention rejection
probe now tests the still-unsupported preemption-disabled context explicitly.

The actual ThreadX wait-abort TU is compiled unchanged under the private symbol
`anx_tx_original_wait_abort` via a source-specific research compile definition.
The ABI-name wrapper invokes an optional per-target abort policy. The NetX receive
policy acquires the IP mutex BEFORE the raw abort changes target state. Its resume
cleanup hook is strictly nonblocking (TX_NO_WAIT), normally recursive under the
already-owned IP mutex. Other wait kinds fall through to the unchanged raw body.
No production target, source pin, protocol source or vendor header is modified.

The wrapper holds a counted target pin while the abort caller can park; detach
and policy/cleanup-hook reconfiguration reject pins. Socket/IP storage must still
be retained externally. After acquiring the mutex the policy rechecks state,
cleanup kind, control pointer, ThreadX sequence and a backend operation epoch.
The epoch advances only for a NEW armed suspension; a private cleanup-grace wait
keeps the same epoch. This distinguishes real re-suspension from grace rearming,
which changes the primitive wait token. ThreadX's NetX TCP suspension sequence
alone cannot detect a new receive. Counter exhaustion fails closed.

If arrival/cleanup finishes the target or it starts another wait while the caller
parks, the abort returns TX_WAIT_ABORT_ERROR without affecting the newer wait.
If the abort caller's own mutex wait is cancelled, it similarly releases the pin
and returns TX_WAIT_ABORT_ERROR with the target untouched. This is an explicit
research policy for the added acquisition wait. IDs/generations do not make freed
socket pointers safe. Event/wire/socket-lifetime and full scheduler validation
remain open.

A concurrent pthread host fixture models serialized boundaries and real parked
callers rather than nesting a second logical get before the first returns.
Eight mutex schedules cover handoff, recursive release, real finite timeout,
actual wait-abort, stale cleanup sequence, two FIFO waiters and both timeout/
release orders. It checks ownership/list/count/status and detach protection.
Five NetX schedules cover contended abort winning, arrival winning, the receiver
starting another receive (first waiting for the mutex), stable grace rearm and
cancellation of the abort caller. Packet transfer is a decoded-payload fixture,
not TCP wire/header/public receive coverage. Previous checks remain: host CTest
PASS 15/15 includes those 13 concurrent schedules, the foreign-release rejection
and the earlier guard/model
coverage; rejection probes are not successful unsupported operations.

The native smoke adds five cases to the prior twelve: mutex handoff, finite
mutex timeout, mutex wait-abort, deferred receive abort after contended IP mutex
acquisition and packet arrival while the abort caller waits. Three real Exec
tasks retain/reap their resources. Exact `9cec1234db175dab186de65c09c69d8dcf1c55b6`
passed 17/17 native checks, workers_reaped=2, exit 0 after 15 seconds on one
boardless A1200/KS3.1 r40.68 run, all stacks 8192. Binary SHA256
`8b1a45f7094f3ff5ec952cd2f8dec93236294a6ef26667f0489d8f6b4bb32fe0`,
55,012 bytes; same 90-second harness `300b22e8`. Actual stdout/exit/startup/hash
is retained in `/Users/turo/ai/evidence/exec-threadx-spike5-native` and native
staging is removed. Host-only `0c48a9bb9` additionally registers the abort policy
in UDP/sleep/mutex fixtures to verify raw fallthrough; native inputs unchanged.
A control compile without the raw abort symbol rename produced identical m68k
.text (216 bytes) and relocation records; evidence is retained in
`exec-threadx-spike5-raw-abort-identity.json`. Exact independent implementation
review by deepseek-v4 covers exact `9cec1234d` + host `0c48a9bb9` + guard
`56878cb89`: correct within bounded scope, no blocker. It confirms pin release,
operation stability/re-wait rejection, FIFO handoff, nonblocking hooks, raw
fallthrough and the targeted foreign-release guard. All callers must remain
registered; normal blocking requires an outer boundary. Socket lifetime, full
scheduler/adoption and broader dispatch integration remain open. The foreign-release guard `56878cb89` passed the same
17 native cases, workers_reaped=2, exit 0 after 15 seconds on one same A1200/
KS3.1/stacks run. Guard binary SHA256
`dc73e1c5db8d1fc52fe1726d5ecf030e826844c4927e456cb9b6f461f3b8f689`,
55,080 bytes; actual evidence is
`/Users/turo/ai/evidence/exec-threadx-spike5-native-guard`, staging removed.
Owned research/control-build outputs and both native staging runs are removed;
small useful evidence remains under `/Users/turo/ai/evidence/exec-threadx-spike5*`.
No replacement-library size saving is measured.

## Integration priorities from the independent follow-up

A separate read-only review of exact `56878cb89` freshly rebuilt the host suite
(PASS 15/15), retained `exec-threadx-insights-56878cb89-host.txt`, and removed its
own build. Its manifest correction above is verified against `contract-full.json`.
These follow-ups are not bounded-spike failures or production approval:

- Exercise the actual DHCPv6 stop/start consumer before enabling it. It combines
  preemption thresholds, explicit suspension of an already waiting worker, wait
  abort, UDP unbind and timer deactivation. Upstream delayed suspension keeps a
  resumed wait explicitly SUSPENDED; this bridge currently makes it READY and
  has no `tx_thread_suspend` service. Implement/prove the combination or reject
  delayed suspension before exposing that consumer. Include abort/timeout/arrival
  with explicit suspension pending and later explicit resume.
- Add named identity/lifecycle tests for external Exec Wait/DoIO and callback
  reentry. Existing SANA-II `ami_sana2_do_io` brackets DoIO with baton enter/leave;
  dropping globals only in `_tx_thread_system_suspend` is insufficient for those
  foreign waits. Measure the longest Forbid interval alongside throughput.
- Document abort-policy contexts and lock order; exercise two abort callers
  pinning one target, cancel one, and prove detach stays blocked until the last
  pin is released. Target pins still do not retain socket/IP storage.
- Build the next executable slice around the actual NX_IP helper event wait,
  periodic timer and real receive/packet-pool ownership, then shutdown/restart.
  Compare traces with the original backend and add a replacement-only link gate
  that rejects accidentally linked ThreadX core implementations.
- Measure resident memory, caller signal consumption, IO requests, tasks/stacks
  and lower-priority wait expiry under sustained higher-priority producer load.
  Each private Exec wait currently uses two signals and a timer IO request;
  smoke executable growth is not replacement-library cost.

## Still open

Full current-thread/adoption semantics, general mutex/event deletion and scheduling,
thread lifecycle, priority/preemption semantics, automatic common timer integration
and replacement backend selection remain unimplemented. Event waiters and explicit
application timer callbacks are implemented in spike 6. Minimum-profile coverage,
broader native schedules, full UDP wire/IPv6 checksum coverage, NetX/socket
conformance and net size/runtime comparison remain pending. The compile probe checks that referenced
fields exist; target/profile-specific layout goldens and full replacement link
checks remain to be added. No complete backend or size-saving claim exists.

See [the research plan](../../docs/plans/exec-threadx-compat.md).

## Spike 6: IP event wait and explicit application timer clock

The research bridge now links unchanged pinned ThreadX event get/set/cleanup and
application timer create/activate/change/delete bodies. Compiler-only symbol
renaming lets wrappers check the supported call boundary before these bodies
publish state. Event creation remains a bounded bridge service; event deletion,
notification callbacks and interrupt-driven list search are not implemented.
Actual event get returns the upstream current-flags snapshot, including bits
outside the requested mask. Multiple clear waiters see the same set snapshot.
Timeout and generic wait-abort use actual cleanup; the receive-specific abort
policy falls through for event waits.

`anx_tx_timer_tick` advances only created application timers, one tick per call
from an outer marked task timer boundary. It never advances private thread wait
IO/deadlines. The flat internal active list replaces the timer wheel, so public
deactivation preserves the actual remaining countdown without wheel arithmetic.
Expiry installs periodic reload before invoking the callback. Callbacks may set
events and deactivate timers, including themselves or another due timer. Create,
activate, change, delete and nested ticks during dispatch fail closed; storage
must remain retained for the full dispatch boundary. Created-list order is a
research dispatch policy, not a ThreadX callback-order conformance claim.

The native driver waits through its existing private Exec/timer.device adapter
outside the boundary, then dispatches one tick. There is no independent common
timer task, elapsed-time catch-up or scheduling guarantee. This explicit clock
proves the callback/event seam, not production timekeeping or a complete backend.
The existing ThreadX time clock wraps as an unsigned ULONG; host ULONG is 64-bit,
while the m68k build uses its real 32-bit ABI.

The host fixture uses real concurrent callers and the actual NetX
`_nx_ip_periodic_timer_entry` callback. It exercises the IP helper's mutex
release/event wait/mutex acquire ordering, without claiming to run the entire
`_nx_ip_thread_entry` loop. Ten event schedules cover immediate AND/clear,
preemption-disabled rejection, arrival, real finite timeout, abort, stale cleanup,
expiry/set order, two clear waiters, a retained unmatched AND waiter, and head
abort with tail delivery. Three application timer scenarios cover periodic event
wakeup/coalescing, countdown-preserving deactivate/reactivate, ignored active
change, inactive change, one-shot/null callback, self/other cancellation, active
delete and clock wrap. Four negative probes verify unsupported nested/timer
blocking, tick context and callback deletion fail before waiter/lifecycle mutation.
Host CTest PASS 20/20 at exact `a7d6196cb` includes the retained tests, the new
concurrent event/timer model and four explicit rejection probes; those probes
are successful guard rejections, not supported operations. The expanded native
smoke passed 21/21 at that commit, workers_reaped=2, exit 0 after 15 seconds, on one
boardless A1200/KS3.1 r40.68 run. Parent and both workers have 8192-byte stacks;
harness `tools/amiberry-run.sh -m A1200 -t 90` at 300b22e8. Binary `008689e7`,
60,880 bytes including all fixtures/runtime, is not replacement-library cost.
Native evidence is retained in `/Users/turo/ai/evidence/exec-threadx-spike6-native`;
owner claudecode confirmed zero staging files remain. deepseek-v4's independent exact `a7d6196cb` implementation review is complete:
no blocker in the bounded scope, with all 18/18 parts read. The reviewer did not
run tests; host/native execution is separately attributed above. The bridge owns
application-timer globals; linking full ThreadX initialization would duplicate
those symbols and must be rejected by the future replacement-only link gate.
Flat O(N) dispatch cost, common clock catch-up, the real IP helper boundaries and
object lifetime remain unmeasured or unsupported. Owned local build/edit scripts
and native staging are removed; useful logs, hashes, binary and map are retained.

Packet pool/public receive ownership, the full IP helper and original-backend
comparison remain the next integration slice. Thread creation, delayed suspend,
foreign Exec IO, common timer lifetime/drain and replacement-library linking and
size measurement remain open. Vendor and shipping build inputs are unchanged.

## Spike 7: real packet-pool and UDP ownership

This slice links unchanged pinned packet-pool initialize/create/delete,
allocate/cleanup/release, data append/copy and UDP packet delivery bodies. The
allocation wrapper rejects unsupported blocking contexts before publishing;
quiescent pool deletion requires zero waiters and every packet returned. It does
not implement pool deletion with outstanding packets or sleeping allocators.
The actual pool initializer owns its globals; the bridge does not duplicate them.

Host concurrent schedules cover empty-pool NO_WAIT, FIFO handoff, saved prepend
offset and output slot, timeout/abort against release, cancelling the head with
a live tail, copy, chain release across two pools and double-release rejection.
Actual UDP delivery queues or resumes a receiver; actual receive strips the UDP
header and the application returns the owned packet to its pool. Each scenario
walks the recovered free list and checks owner, sentinel, count and no waiters.
Four rejection probes cover timer/nested/preemption-disabled allocation and
pool deletion with an outstanding packet. Host checksum is disabled deliberately:
the existing LP64 model has 64-bit ULONG and is not the target wire ABI.

The m68k smoke adds seven cases: exhausted-pool handoff, finite timeout, abort,
real pooled UDP delivery with valid checksum, bad checksum then valid queued
packet, bad checksum with empty continuation, and bad checksum release waking a
pool allocator while UDP receive continues to a valid packet. An independent
byte-wise IPv4/UDP checksum fixture supplies target data; the receive path runs
actual NetX checksum/release code. Input is injected at UDP dispatch after
synthetic IP decoding, with a manually initialized bound socket. There is no
link driver, actual bind lifecycle or full wire/IP-stack verdict. IPv6 checksum,
fragment chains, shared-port/multicast fan-out and notify callbacks remain open.

UDP delivery references optional ICMP generators. This isolated build has two
explicit terminal ICMP transmit sentinels because routing/transmit are absent.
They never return a fabricated success and are excluded from all claimed paths;
a future replacement-only link gate must reject these sentinels. No vendor or
shipping build changes select this experiment. Common task/timer lifecycle,
foreign Exec IO, socket retirement, original-backend comparison and net library
size savings remain open. Exact `586278ae3` passes host 25/25 and the expanded
native 28/28 smoke, workers_reaped=2, exit 0 after 15 seconds on one boardless
A1200/KS3.1 r40.68 run, parent and both workers 8192 bytes. Binary `bf8b5e86`, 70,284 bytes,
includes runtime/fixtures and is not a library size result. Actual stdout, exit,
startup and hashes are retained under `/Users/turo/ai/evidence/exec-threadx-spike7-native`;
claudecode confirmed zero staging files remain. deepseek-v4 independently
reviewed exact `586278ae3`: no blocker in the bounded scope, all 20/20 parts read.
The reviewer ran no tests; host/native execution is attributed separately. The
stricter prepublication allocation and quiescent deletion policy is deliberate,
not full upstream lifecycle conformance. Owned local build and native staging
are removed; small useful evidence is retained.

## Spike 8: owned Exec task startup and normal reaping

`exec_task.c` provides an explicit research task mechanism, not the public
ThreadX create/terminate/delete services. Caller-owned control/stack storage is
published before AddTask; the worker opens its own private wait IO and attaches
the real pinned TX_THREAD before acknowledging startup. Entry runs in one outer
bridge context and can park on supported ThreadX services. Normal return requires
quiescent producers, no pending wait/cleanup/abort pins, no owned mutexes and no
active private timer. The worker detaches, closes/reaps private IO, then publishes
FINISHED and calls RemTask without a scheduling gap. Only the creator can reap
and release its ACK signal; record/stack storage remains retained until then.
The embedded Task has an empty memory-entry list; Exec cannot free caller storage.

The native fixture tests invalid storage/context rejection, ACK signal allocation
failure before publication, identity/entry/stack metadata, two independently
blocked tasks, live/duplicate/double reap rejection, normal return, twelve reuses
of the same stack/record and completion before startup observation (the fixture
temporarily lowers its creator's Exec priority). It verifies stack canaries,
creator signal allocation recovery, detached IDs and closed private IO after
every successful reap. Host models additionally check the idle-boundary query;
they do not execute Exec task lifecycle. Native verdict and independent exact
review are recorded after execution, not inferred from cross-compilation.

All child tasks use Exec priority zero. No ThreadX priority/threshold/timeslice
contract is claimed, and no full public thread creation, TX_COMPLETED state,
external forced termination, adoption or NetX IP lifecycle is fabricated.
Child-side IO-open/registration failure rollback exists but is not fault-injected
by this fixture; only creator ACK allocation failure is exercised. Producers
must already be quiescent on entry return; future full NetX shutdown needs an
explicit drain/stop protocol. Shipping/vendor inputs remain untouched. Next:
connect this owned task mechanism to the public service contract, implement
quiescent object retirement and helper stop/drain, then create a real NX_IP.

Spike 8 exact `101d52195` independent source review is complete: deepseek-v4,
all 19/19 parts read, no blocker in bounded scope, no tests run or full backend GO.
Host 25/25 and native 11/11 (15 tasks reaped, 12 restarts, exit 0 after 14s) are
separate execution evidence. Local build/native staging is removed.

## Spike 9: reserved-worker public thread creation boundary

This section records the spike 9 checkpoint; spike 10 below extends its policy.

The native-only `exec_thread.c` implements the pinned public create/resume/delete
signatures over caller-retained reservations. Prepare opens IO in the real owner
before the constructor enters its boundary and parks on Exec's reserved SINGLE
signal. Preparation leaves the target control block untouched. Public create
consumes a matching creator/control/stack reservation, initializes the real
TX_THREAD and timeout metadata, publishes the runtime binding and circular
created list, then marks READY/signals for AUTO_START. The worker preserves these
public fields and cannot enter until the creator's outer boundary releases.
DONT_START stays suspended until the explicit initial public resume. No public
thread-create call blocks or opens IO while inside the constructor boundary.

Exec priorities are capped to the production port's priority band: logical
0..15 map to Exec 1, 16..31 map to Exec 0; logical fields remain unchanged.
Ties are deliberate. Strict ThreadX scheduler
conformance is not established. Unequal thresholds and nonzero time slices return
TX_FEATURE_NOT_ENABLED before control mutation. NetX IP create's hardcoded slice
of one is therefore still unsupported: a real NX_IP create/helper verdict remains
open. There is no unreserved allocation path (TX_NO_MEMORY), general suspended
thread resume, public threshold change, forced termination or helper drain.
No vendor/header/production build changes select this experiment.

Normal owner return checks quiescence, removes its runtime binding and retains ID
with TX_COMPLETED. Private IO is closed before FINISHED plus RemTask under one
uninterrupted Forbid. Creator-side wait only observes native retirement; public
delete then clears ID/created links/native owner pointers, releases its ACK and
reservation, and permits stack/control/record reuse. A completed ID alone never
authorizes delete. Unbound cancellation closes IO and reaps without touching
public control storage. Domain reservation holds prevent runtime reset even while
workers are unbound or completed but not deleted. No producer quiescence is
invented: caller must stop all producers before owner return.

Host models cover preserved metadata, duplicate binding, foreign/active/owned-
mutex/pinned retirement rejection, completed ID retention and reserved-domain
reset rejection. They do not execute Exec/public native lifecycle or target ABI.
The native fixture covers missing reservation and ACK failure without mutation,
prepared IO with untouched target, unsupported slice/threshold/stack rejection,
nested AUTO_START publication, DONT_START/initial resume, normal retirement,
foreign/premature/double deletion, circular-list/signal/stack recovery, six public
create/complete/delete restarts and one unbound cancellation. Child-side IO-open
failure is not fault-injected; AddTask failure is reviewed but not exercised.
Final tested code is `d3720c81826960b046e44c4aecfa6ad4ff49ceb3`: root host
27/27 and m68k cross/startup gate pass; claudecode's single boardless A1200
KS3.1 r40.68 native run passes 12/12, tasks_reaped=10, restarts=6, exit 0
after 14s, with parent/child stacks 8192. The additional case creates logical
priority 2 and verifies its actual Exec priority 1 through normal retirement.
Root read actual output/startup/runner and verified binary/map/evidence hashes
under `/Users/turo/ai/evidence/exec-threadx-spike9b-native`; binary `62efc96f`,
58,048 bytes including fixtures/runtime. Initial `a87f95f58` native 11/11 is
retained separately and does not establish the corrected priority policy.
Deepseek-v4's independent read-only source review covers the initial change
(19 parts) and final priority delta (eight parts): no blocker in the bounded
scope; no execution is attributed to the reviewer. Owned build/native staging
are cleaned. A never-resumed bound DONT_START thread still requires a future
termination path; the reserved SINGLE gate and created-thread global ownership
also need care in full integration.

The plan's required performance comparison includes baton handoffs, task switches,
CPU, throughput, latency and resource costs under equivalent protocol workloads.
These correctness fixtures provide no measured performance improvement or
finished-library size verdict.

## Spike 10: bounded Exec scheduling policy for NetX creation arguments

The public constructor now stores nonzero time_slice/new_time_slice values.
They are advisory metadata: Exec round robin outside serialized boundaries
governs dispatch; there is no ThreadX slice countdown, Quantum/Elapsed change,
priority inheritance, ready-list scheduler or strict scheduling conformance.
Initial creation threshold must still equal priority; unequal creation thresholds
remain rejected before mutation. Logical priorities and capped native bands are
unchanged. This accepts the shape of NetX's one-tick helper creation argument;
it does not yet run the real IP constructor/helper.

The public preemption-change service requires a registered current owner in an
unmarked serialized call. It validates the new threshold against user priority,
returns the old user threshold, and changes both logical threshold fields.
Foreign-thread changes return TX_FEATURE_NOT_ENABLED without mutation. Unsupported
inheritance is fatal before mutation. Invalid ID/output/threshold and marked
timer calls fail without changing fields or output. Attached model owners now
explicitly initialize their inheritance sentinel to TX_MAX_PRIORITIES.
The slice-change service validates live registration/output/normal thread caller,
returns the old new_time_slice and stores both fields; it may change another
registered thread's advisory metadata under the same boundary.

An outer context_end with a raised current threshold is fatal before Permit.
Nested contexts can return while the outer boundary remains held. Real ThreadX
event suspension may release that boundary while retaining its logical threshold;
the bridge reacquires serialization and identity before returning to caller code.
The owner must restore its threshold before its outer call returns. No yield or
relinquish path is supplied. No claim is made that a woken sender wins native
dispatch against its producer; each running caller's boundary is protected.

The host model tests normal/error/foreign/timer metadata operations, nested
boundaries, and actual pinned event wait/set suspension with the raised threshold
preserved until restoration. Three separate fatal guards cover outside-context
mutation, raised-threshold outer exit and unsupported inheritance. Together with
retained models, host CTest passes 31/31; the m68k cross/startup gates pass.

The native fixture retains the bounded creation/lifetime cases and adds actual
priority-2/slice-1 helper-shaped creation under a temporarily raised creator
threshold. It checks restoration and marker publication before entry, actual
slice changes in the worker, raised-threshold event blocking, producer marker
publication before the woken worker can resume, protected identity on return and
threshold restoration before retirement. Expected 14/14, tasks_reaped=10,
restarts=6. Claudecode's single boardless A1200/KS3.1 r40.68 run at exact
`37776d2651c6c85f3e414c759de70e2c8436c72e` passes all 14 named cases,
tasks_reaped=10, restarts=6, exit 0 after 14s, parent/child stacks 8192, harness
`300b22e8`. Root verified actual stdout/startup/runner and all binary/map/evidence
hashes under `/Users/turo/ai/evidence/exec-threadx-spike10-native`; binary
`4cba8af3`, 60,168 bytes including fixtures/runtime. Independent deepseek-v4
read-only source review is complete (all 19 parts): no blocker in this bounded
scope, no tests run by the reviewer. Owned build/native staging are removed.
The proposal for real IP retirement is retained separately and is unimplemented.
Scope remains research only, with
no production/vendor/header changes, real NX_IP helper, forced stop/drain, full
replacement link, automatic clock, performance improvement or net size verdict.

## Spike 11: exact-event pre-stop and private owner retirement

The native research creator can request anx_exec_thread_stop_event inside one
normal outer serialized boundary, even while preempt_disable is incremented.
This is a narrow integration command, not a public tx_thread_terminate service.
Only an entered worker with a live pending generation on the exact event object,
actual event cleanup/sequence/list membership, no owned mutex, pending wake or
abort pin is eligible. READY, self/foreign creator, wrong object and nested
requests are refused before mutation. Producers outside this domain must already
be quiesced; ID validity alone does not make arbitrary accesses safe.

The bridge deactivates the real internal timer, marks TX_TERMINATED, invokes
unchanged pinned event cleanup to unlink the waiter without normal resumption,
and removes the runtime binding. It clears its private public-control pointer
before completing the private wait generation DELETED under protection. The
owner's suspend loop recognizes its private terminal flag immediately after wait
returns: it checks private state under protection, releases that protection,
and calls its no-return terminal callback before restoring the ThreadX context
or dereferencing the public control block. A returning terminal callback is fatal.
The worker closes its private IO in its real owner outside all call boundaries,
then publishes FINISHED/ACK/RemTask without a scheduling gap. A stopped ID remains
valid with TX_TERMINATED until creator wait observes native retirement and ordinary
public delete succeeds; the FINISHED-before-clear/reuse contract is preserved.
There is no deferred logical delete, forced removal of a running task, event/
mutex deletion, full IP helper or real driver IO integration in this checkpoint.

Native wait adapters now count timer sends/reaps for lifecycle proof and reject
close with an unreaped submitted request. These are correctness counters, not
performance measurements. The stop fixture checks two genuinely in-flight
timer.device requests before stopping; owner wake aborts/reaps them through the
normal park path before close. Its four stop/delete cycles poison and reuse
control/stack storage only after native ACK plus public delete. It also exercises
a real owned mutex refusal, exact event cleanup under preemption-disable, no
return into entry, premature-delete refusal and signal/domain recovery.

Host CTest passes 33/33, including real event cleanup/timer unlink/private terminal
dispatch and a fatal returning-callback guard, alongside retained models. The
host callback escape uses setjmp/longjmp only in the fixture; no native mechanism
uses it. m68k cross/startup gates pass. Exact code
`972c5f418254d0f64b80c907b10e349a2def1797` passes native stop 9/9,
tasks_reaped=4, timed_io_reaped=2, restarts=3, plus retained public thread 14/14,
tasks_reaped=10, restarts=6 on the same code because normal worker startup shares
the new terminal hook. Claudecode ran each once on boardless A1200/KS3.1 r40.68,
exit 0 after 14s, parent/child stacks 8192, harness `300b22e8`. Root read actual
stdout/startup/runner and verified all binary/map/evidence hashes under the
separate `/Users/turo/ai/evidence/exec-threadx-spike11-stop-native` and
`exec-threadx-spike11-thread-native` directories. Stop binary `c008e803`, 54,128
bytes; thread binary `bb61fc1f`, 61,644 bytes including fixtures/runtime.
Independent deepseek-v4 read-only exact source review is complete (all 22 parts):
no blocker in the bounded pre-stop scope. The reviewer ran no tests; root host
and claudecode native execution are separate evidence. This remains exact-event
stop, not general termination or a full IP helper verdict.
Owned build and both native staging paths are removed. Research-only;
vendor/public headers/shipping selection unchanged.

## Spike 12: quiescent mutex and event retirement

Research mutex/event creation now maintains the actual ThreadX circular created
lists and counts. Membership checks reject duplicate creation and forged IDs
without reading uncreated control blocks. Mutex creation remains the bounded
NO_INHERIT bridge implementation; it does not install the raw constructor's
forced-release callback. Public layouts and vendor sources are unchanged.

Guarded deletes call compiler-renamed, unchanged pinned ThreadX delete bodies
only after the object is a valid list member and quiescent. Mutex owner/count,
owned links and suspension list/count must be empty. Event suspension list/count,
search/delayed-clear state and any configured notification callback must be
empty. Captured active wait, pending wake and abort-pin references also prevent
retirement. Busy deletion returns TX_FEATURE_NOT_ENABLED without mutation;
invalid membership returns TX_MUTEX_ERROR or TX_GROUP_ERROR. Creation/deletion
outside a normal serialized context or during a timer/resume callback is fatal.
The upstream bodies preserve an existing preemption-disable increment. Runtime
reset refuses any retained created object, even after every thread detaches.

This is quiescent deletion only. External API/packet/driver/timer producers must
be quiesced before reclaiming object storage; an ID or generational wait token
cannot enforce that lifetime. The future unchanged NX_IP delete path needs an
integration wrapper that preflights each service whose status raw code ignores before any raw
mutation. The general ThreadX delete-with-waiters contract is unsupported.

The shared host/native object probe checks head, non-head and singleton list
removal, duplicate/forged/NULL rejection, recursive-owner refusal, post-delete
service rejection, real get/set behavior, preemption-counter preservation and
poisoned storage recreation. The host mutex model also tries deletion with real
queued waiters, then retains handoff/FIFO/timeout/abort schedules. The stop model
tries event deletion with a real blocked worker before cleanup. Existing fixtures
now retire their objects before detach/reset instead of forgetting live objects.

Root host CTest passes 38/38 and m68k -Werror/startup-first builds pass at source
05cd6c7592c29862d3dc63b863c378c8722c3c75. Notification callbacks are disabled
in the current port configuration; the conditional callback guard is not a
runtime-tested callback implementation. Independent deepseek-v4 read-only exact
source review is complete (all 19 parts): no blocker in the bounded retirement
scope, no tests run by reviewer. Claudecode ran each artifact
once on boardless A1200/KS3.1 r40.68 with parent/child stacks 8192 and harness
300b22e8. Actual native stop passes 13/13, tasks_reaped=4, timed_io_reaped=2,
restarts=3, object_restarts=3; retained public thread passes 14/14,
tasks_reaped=10, restarts=6. Each exits 0 after 14s. Root independently read the
actual guest stdout, startup and runner receipts and verified all binary/map/
evidence hashes in exec-threadx-spike12-stop-native and thread-native under
/Users/turo/ai/evidence. Stop binary d7a67d68 is 59,704 bytes; retained thread
02a6b169 is 64,144 bytes including fixtures/runtime. No queued run is a pass.

Partial non-LTO stop-map input .text totals 14,372 backend bytes and 3,112
retained ThreadX bytes (17,484 subtotal), plus 3,300 clock division helper bytes.
Object probe/fixtures/startup/libc/other helpers/data/BSS/relocations/native
resources and missing full integration are excluded. Evidence is
/Users/turo/ai/evidence/exec-threadx-spike12-code-cost.json. These totals cannot
be subtracted from shipping LTO spans or establish a finished-library saving.
Full NX_IP constructor/helper/delete, driver integration, common clock, general
termination and the required performance A/B comparison remain open.

Owned build and both native staging paths are removed after retaining exact
artifacts and receipts. The research branch remains unmerged. Full created-ring
scans cost O(N) per operation; no size/performance benefit is assumed. A review
statement that notification callbacks were active was corrected against the
actual port header and conditional TX_SAFETY_CRITICAL prohibition in tx_api.h;
those callback fields/tests are compiled out in this configuration.

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

## Full library integration: replacement link and semaphore services

This stage is in progress. Source b0b35f3b6e9fb11894db583004694bf2e6711ddb
adds a reproducible diagnostic gate in research/exec-threadx/library_gate.py.
The baseline builds the actual default full-feature bsdsocket.library with the
pinned compiler, unchanged vendor bodies, Werror, entry/romtag/version gates,
Release and LTO off. The replacement attempt preserves its public-vector objects,
feature/layout definitions (including IPv6, DHCP, DNS and mDNS) and NetX archives.
It compiles the research backend and retained upstream ThreadX service bodies
with those same full-profile flags, excludes both original ThreadX archives and
removes netstack_baton.c.obj from a disposable copy of the netstack archive.
No tracked shipping archive/source is rewritten. No protocol fixture, test PRNG,
success stub or ignored unresolved symbol satisfies the full link.

The actual replacement link remains **FAIL**, with 31 unresolved symbols. These
are contracts to implement and execute, not 31 independent fixes:

| Remaining boundary | Unresolved symbols | Required behavior |
| --- | ---: | --- |
| Caller registry | 9 | Cached/uncached pointer+generation handles, owner identity, signals, eviction, release and dead-owner cleanup |
| Exec wait hooks and health | 10 | Drop/reenter serialized context around actual device waits, nesting, retained owner lifetime and accurate diagnostics |
| Kernel and worker lifecycle | 6 | Runtime/clock startup and shutdown, dynamic worker preparation, retained stacks and native removal |
| Exec task checks | 3 | Safe task-context/liveness checks and signaling |
| Upstream addon scheduling and timer query | 3 | Explicit suspend/resume and relinquish behavior, timer metadata for DHCP/AutoIP/mDNS |

Resolving these names is only a compile/link prerequisite. Existing services also
have bounded behavior: initial reserved-worker resume differs from arbitrary
addon resume; stopped-event termination differs from general worker stop; the
experimental IP constructor admits a fixed IPv4 profile. The full library still
needs actual creation/publication, all enabled protocol lifetimes, concurrent
TCP/public socket calls, device IO drain and close/cancellation verification.
There is also a concrete stack-policy mismatch: shipping IP/AutoIP/DHCPv6 stacks
are 4096 bytes and the DHCPv6 work stack is1536, while the research native-worker
reservation requires at least8192. Integration must establish safe native stack
budgets/ownership and account for them in resident resources; resolving the link
alone would not make these constructors succeed. No guard is lowered here.
A link without execution cannot establish these contracts or a size benefit.

The first implemented service group is semaphore create/get/put/delete and
cleanup, needed by RAW socket receive and SANA-II reader ready/exited handshakes.
The real pinned ThreadX bodies retain count, FIFO suspension, direct token handoff,
timeout and abort cleanup. Research wrappers require a serialized context and
real created-ring membership; duplicate or forged controls refuse. Busy or
pending-resume/abort-referenced deletion refuses without mutation. Runtime reset
and the existing bounded IP preflights now include live semaphores. Count limits
fail closed: put at ULONG_MAX returns TX_CEILING_EXCEEDED rather than wrapping.
Notify callbacks and delete-with-waiters remain unsupported; full-library close
paths must drain users before reclamation. These restrictions are not full
ThreadX semaphore/API compatibility claims.

Host models pass 40/40, including the added live-semaphore reset rejection.
The native IO fixture adds seven substantive cases to its prior 18: real FIFO
waiters, direct handoff without count inflation, busy/pending-resume deletion
refusal, finite timeout, public abort cleanup, duplicate/forged controls, count
limits, actual deletion and poisoned storage reuse. Two full cycles now retire
18 application workers.

The first b0b35f3b IO run failed after 14 completed cases at the second token
handoff/deletion assertion; the harness timed out after90s with no exit marker.
Original stdout/startup/runner and emulator logs are retained. Source showed
that direct _tx_thread_system_resume completes the private AnxWait immediately
when raw cleanup is already clear. The old object reference predicate therefore
missed a signaled caller whose real semaphore get had not returned. The fixture
also returned from a failed CHECK with its call boundary/native owners live;
the specific reset/timeout PC cause is unproven and failure-path cleanup remains
a deferred guest-harness obligation.

Backend-only f8dd41d0b9a5ec0dd1e5bea887e76daf691c3ae9 adds a private semaphore_call
reference before the actual potentially blocking upstream get, cleared only
when that function really returns. Deletion and detach consult it, retaining
the object through producer signaling and native owner dispatch. Public vendor
layouts/bodies and all 25 native assertions remain unchanged. Host40/40 and
m68k Werror/startup-first gates pass after the fix.

At exact f8dd41d0b the IO guest passed25/25, application workers18/helpers2/clocks2/
restarts1, exit0 after15s. Refreshed protocol19/19 exited0 after17s and clock16/16
exited0 after18s. Each was one boardless A1200KS3.1r40.68 run with harness300b22e8,
-t90 and all stacks8192; root verified actual stdout/startup/runner/all receipt
hashes. No illegal/guru/alert/reset line was reported, native staging is0 and the
owned build was removed. Independent source-only implementation review is complete:
root read initial5/5 and correctness6/6, focused fix5/5, receipt4/4 and rename
erratum4/4. Deepseek confirms the real original lifetime gap, minimal final fix
and honest replacement-only link diagnostic; no remaining blocker in this bounded
service scope. The public wait-abort macro reaches the guarded backend wrapper;
the contrary initial bypass note was independently withdrawn against pinned e24.
Source review and execution remain separate. A matching event-flags returning-
call lifetime audit is OPEN for full-library close integration. This verified
service/link-gate checkpoint does not complete full-library replacement.
The original b0b35f3 failure and SHA evidence remain distinct. Native service
coverage uses the earlier IPv4 guest profile; the full-profile IPv6 replacement
link is separate and still fails with31 unresolved symbols at the fixed source.

AgentNet's complete 12/12 caller advice and 5/5 erratum are retained separately.
The corrected advice confirms that attaching an existing Exec task needs retained
control/bridge/wait storage, with no new task stack. AmiNetCaller can remain an
on-stack pointer+generation handle into a backend-owned registry. The >=8192-byte
stack rule applies to newly launched research workers. Cached caller lifecycle,
dead-owner reclamation and nested blocking still require implementation.

Evidence is indexed in /Users/turo/ai/evidence/exec-threadx-library-integration.json
and exec-threadx-library-gate-final.json. Equal-feature finished-library size,
resident resources, synchronization cost and throughput remain unmeasured.

## Full library integration: retained contexts around Exec IO

Source c6432596460c576e10c9cf56d6f7c12c08323cc5 adds
anx_tx_context_pause/resume for an admitted, live owner. The pause retains the
complete normal same-thread context chain in its private bridge record, clears
the domain's current-thread/frame state and releases every bridge Forbid level.
After actual Exec IO, final resume restores that exact chain and level count.
Nested release/acquire pairs retain the paused state until the final acquire.
No ThreadX suspension queue, timer or original baton/scheduler transition is used
for this Exec wait. Existing deferred cleanup wakes are flushed before release.

The platform preflight runs under a temporary guard and checks that the held
Forbid levels belong precisely to this context chain and interrupts are enabled.
External Forbid/Disable, marked callbacks, unrestored ThreadX critical sections,
foreign/unmatched restore and nonquiescent waits are refused unchanged. The owner
cannot begin/end another context, mutate its private integration hooks, detach
or reset the runtime while paused. The chain and record must remain retained
through real IO and re-entry; this contract does not permit foreign task removal.

The host model executes two caller identities, preserving a real owned mutex
while a second admitted caller makes semaphore progress. It checks three-level
restore, nested releases, protection/identity/retirement guards and fatal begin,
end, reset and ThreadX-service misuse while paused. Host tests pass **45/45**.
All four native targets compile with Werror and the startup-first check.

Exact native binary/map hashes and complete receipts are indexed in
/Users/turo/ai/evidence/exec-threadx-context-pause.json. AgentNet claudecode ran
each binary once serially on the boardless playhouse3 A1200 KS3.1 r40.68 with
stack8192, timeout90 and harness300b22e8. Root checked actual guest stdout,
startup, runner, fault logs and every receipt hash:

| Native fixture at c64325964 | Actual verdict | Exit / host elapsed |
| --- | --- | --- |
| Context pause | 14/14; two created workers reaped, one record/stack reuse | 0 /15s |
| Concurrent driver IO | 25/25;18 workers,2 helpers,2 clocks reaped | 0 /15s |
| IPv4 protocols | 19/19;2 helpers,2 clocks reaped | 0 /17s |
| Clock lifecycle | 16/16 | 0 /18s |

The new fixture executes actual Wait, WaitIO and DoIO outside all protected
contexts. A created worker pauses both its automatic outer and explicit nested
context, performs its own real timer DoIO, restores its identity and performs a
ThreadX semaphore operation while the attached caller's real timer request is
still pending (checked with CheckIO). The caller subsequently restores its
three-level context. Two cycles close owner IO, wait for native ACK, perform
public delete, poison/reuse storage and recover all signals and runtime state.
All four runs have one boot, zero ROM resets and zero illegal/guru/alert lines;
claudecode reports staging0. These elapsed times are harness host wall clock,
not Amiga performance measurements.

Independent AgentNet deepseek source review is complete: all5/5 logical parts
(17 physical chunks), ack4/4 and closing verdict3/3 were read and retained in
/Users/turo/ai/evidence/exec-threadx-context-pause-review.txt. No blocker in the
bounded retained-live-owner scope. Review confirms the exact Forbid accounting,
platform/chain preflight, mutation guards, deferred wake flush and serialized
re-entry. Execution results above remain separate from this source-only verdict.

This is the Exec-wait mechanism required by the replacement, not a completed
production hook or caller registry. Shipping ami_netstack_baton_* entry points,
health/kernel lifecycle and the public cached/uncached adoption APIs remain
unimplemented in the replacement. The last actual full-profile replacement
link at f8dd41d0b remains FAIL31; no new full-library link/runtime verdict is
inferred from these native fixtures. General nested ThreadX blocking still
requires an outer boundary; this change supports nested Exec-wait brackets.

Next implement backend-owned cached caller records behind the unchanged
AmiNetCaller pointer+nonwrapping generation handle. Explicitly resolve native
request/reply-port/signal ownership for externally removed callers before
admitting them: the current AnxExecWait adapter signals its retained live owner
and closes resources only in that owner. Merely storing its record outside the
caller stack does not prove late timer replies or foreign cleanup safe. A
backend-owned deadline driver is a possible design to evaluate, not an
implemented or verified solution. Dead-owner cleanup, library close/cancellation,
event-flags returning-call retention and the4096/1536-versus8192 worker stack
policy remain OPEN. Full-library size/resources/performance comparisons remain
unmeasured until functional integration and equal-feature verification.

## Full library integration: cached callers and retained clock deadlines

Source **448bd099a58c440b7bdfdb1275b27641ffc570fe** implements the public
adoption/cache seam with backend-owned control blocks, bridge frames and wait
records. Existing Exec Tasks retain their original stacks; adoption allocates
one owner signal and no timer request, reply port, Task or stack. There are 16
caller records (one reserved from normal admission), 64 signal leases and 32
pending-adopter records. Cache suspend/resume retains the control, generation
and signal. Full normal admission parks outside Forbid and rechecks after a
clock-driven wake. Reserved overflow refuses immediately.

An explicit handle validates pointer membership before dereference and checks
its exact nonwrapping generation. Generation exhaustion refuses admission and
survives registry/clock restart. Private wait generations also survive record
reuse. Upstream ThreadX APIs retain their bare-pointer ABI: using a deleted and
recycled raw object pointer remains caller use-after-free, as in ThreadX itself.
AgentNet corrected its initial stronger generation requirement; no vendor ABI
or public control layout was changed.

Dormant eviction requires a quiescent bridge and an owner-handed signal. A
separate lease keeps the evicted owner's signal debt until that owner returns
it; repeating the free cannot release an unrelated signal subsequently allocated
at the same bit. Wake checks Task membership, allocated bit and retained identity
stamp with Signal under the same Forbid. Classic Exec lacks a universally unique
Task ID: exact address/shape/bit recycling cannot establish a general identity
proof. No general ABA or ISR-safety claim follows from this checkpoint.

Finite waits use the already retained backend clock's EClock and polling service.
No caller-owned timer reply can arrive after the caller's stack is freed. Polling
evaluates absolute deadlines but wake timing is tick-granular, coarser than the
previous per-caller timer adapter. Actual pinned ThreadX timeout/cleanup bodies
still run when the admitted owner returns from its private wait.

Foreign reclamation has two narrow proved paths: dormant quiescent callers and
object-free actual TX_SLEEP callers. The latter removes the private timer and
binding, marks the control terminated and suppresses the private DELETED wake
only after positive owner-removal evidence. It refuses cleanup/object/mutex,
abort, paused-context and active-frame references. Other removed active or queued
owners remain fatal/unreclaimed; they are an unfinished integration requirement.
Admission preflight checks removal before a new producer context can write into
a removed caller's stack. Pending-adopter/dead-debt cleanup exists in source but
has no direct native fixture verdict yet.

Host tests pass **47/47**; five m68k targets compile with Werror and startup-first
validation. AgentNet claudecode ran each exact binary once, serially, boardless
playhouse3 A1200 KS3.1 r40.68, stack 8192, timeout 90, harness 300b22e8. Root verified
binary/map hashes, every receipt hash, actual stdout/startup/runner/serial and
fault logs. Each run has one boot, exit 0 and no illegal/guru/alert/ROM reset:

| Native fixture at 448bd099a | Actual verdict | Exit / host elapsed |
| --- | --- | --- |
| Cached callers | 25/25; 33 raw Tasks reaped, 1 removed dormant, 1 removed sleeping, 2 clocks | 0 / 15s |
| Context pause | 14/14 | 0 / 15s |
| Concurrent driver IO | 25/25; 18 workers, 2 helpers, 2 clocks, 1 restart | 0 / 15s |
| IPv4 protocols | 19/19 | 0 / 17s |
| Clock lifecycle | 16/16 | 0 / 18s |

The caller fixture covers real sleep and semaphore deadlines, twelve cache
brackets, exhausted signals, full normal/reserved pools, parked admission,
foreign-owner rejection, eviction/debt/double-free, same-address new-generation
reuse, generation exhaustion and clock restart. It externally removes and
poisons actual dormant and sleeping Task/stack storage, then waits past the old
sleep deadline and checks no late notification. All fixture resources recover.
Failed-guest fixture teardown remains deferred; these passing runs do not prove
it. Host elapsed times are harness wall clock, not Amiga performance measurements.

Independent exact-source AgentNet deepseek review is complete: root read all
five logical parts (16 physical chunks), retained in
/Users/turo/ai/evidence/exec-threadx-caller-cache-review.txt. No blocker in this
bounded caller-cache/dead-dormant/object-free dead-TX_SLEEP scope. Review confirms
the actual pinned sleep body and replacement timer list (there is no assumed
vanilla sleep list), notification ordering, handle/token boundary, signal debt,
protected parked-adopter wakes, guards and honest link gate. Tick processing is
Task-driven under Forbid; this is not an ISR-driven timer proof. Repeated sweep
and the two idempotent deadline paths remain performance considerations, not
measured improvements. Execution above is separate from this source-only verdict.
Evidence is indexed in /Users/turo/ai/evidence/exec-threadx-caller-cache.json,
with plan/ABI clarification and native receipts retained separately. Native
staging is zero; the owned build was removed after retaining useful evidence.
The unmerged research branch and upstream vendor pins are preserved.

The actual full-feature replacement link at this SHA is **FAIL with 19 unresolved symbols**, down from
31 unresolved symbols: nine caller services and three Exec Task helpers now resolve. The gate
uses actual library objects/vendor archives, full-profile Werror backend objects,
removes the original scheduler/port archives and baton object, and uses no
fixture sentinels, success stubs or ignored unresolved symbols. The full original
backend diagnostic build passes; its 441460-byte non-LTO artifact is retained as
an unexecuted baseline, not a size comparison. Replacement runtime is NOT_RUN.

| Remaining full-link contracts | Count |
| --- | --- |
| Thread relinquish/suspend and timer query | 3 |
| Production Exec-wait hooks, sampler/statistics and health | 10 |
| Kernel start/stop/running, stack/tick/zombie accounting | 6 |

The previous checkpoint's unimplemented caller-registry obligation is now
partially resolved with the exact scope above. Next connect kernel lifecycle and
production Exec-wait/health hooks, then complete general dead-owner and library
close/cancellation behavior. Event-flags returning-call lifetime, worker stack
policy (shipping 4096/1536 versus new-worker research minimum 8192), enabled IPv6,
actual public sockets/device/ISR integration and full create/close remain OPEN.
Equal-feature full-library image/resident resources and performance comparison,
including synchronization removed with the original baton, remain unmeasured.

## Full library integration: kernel ownership and production Exec wait hooks

Source **65f963b902fc8b3d434005906810fbf51720e9bd** adds a retained management
Exec Task that owns the central clock and caller registry. Start/stop use a
single backend-owned command record; different requester Tasks can operate the
same kernel. Shutdown refuses live cached/paused callers, registered objects and
prepared or completed-but-not-deleted workers. Actual manager removal and stack
retirement precede release of owned storage.

This implementation replaces the production baton entry points with the
verified context pause/resume mechanism. Nested Exec waits retain mutex ownership
and the bridge frame while other Tasks make progress; the original ready-list
baton object is excluded from the replacement link. The existing health ABI
publishes actual clock, bracket, memory and current-holder counters and preserves
the master socket lock. A foreign semaphore at the health name is preserved.
Stack probes cover actual owned management/clock stacks and prepared worker
records, including conservative endpoints. The timer query uses the actual
replacement flat timer list with registered-pointer validation; host assertions
cover active/inactive, countdown/reload, invalid and deleted controls.

The original native kernel at 65f963b90 **hung after 9/22 cases** on playhouse3,
with no exit and a 90-second timeout. Its DOS Delay completion polling was not
safe for the raw Exec Task requester. The failure and original binary/map/logs
are retained separately; the other five fixtures were not run at that SHA.
Corrected source **b6dc71ad63a4aee80eec1c0320df542a25c94c3e** uses one requester
ACK signal and Exec Wait. Reply and manager-death notifications validate Task
membership, retained identity stamp and allocated signal bit under the same
Forbid. A retained clock observer wakes pending commands if their manager is
removed. Existing assertions remain, with an added real signal-exhaustion
rollback case before Task publication.

Removed-manager recovery is bounded to an empty UP domain. It closes admission,
transfers the clock creator and a fresh ACK signal to the recovery caller,
transfers the empty registry creator, detaches its service/admission hold, and
stops/joins the clock. Retained ownership holds unwind 2 -> 2 -> 2 -> 1 -> 0.
Live clients refuse cleanup. Half-start manager death, recovery-caller death,
worker preparation during recovery and general busy removed owners remain OPEN;
post-claim invariant failures stop rather than fabricate cleanup.

Host tests pass **47/47** and six m68k artifacts compile with Werror and
startup-first validation. The independent AgentNet source review is complete:
all 3 logical parts / 13 physical chunks are retained in
/Users/turo/ai/evidence/exec-threadx-kernel-source-review.txt. It confirms the
original polling blocker and the correction, with no post-fix blocker in the
bounded quiescent lifecycle scope. Source approval does not establish execution.

AgentNet's lab run was queued behind independent CI without a boot or verdict.
Root cancelled only that queued research execution and ran all six exact fixed
artifacts once serially on the existing local Mac Amiberry **8.3.0 (2026.08.05)**,
using the same supplied A1200 Kickstart 3.1 r40.68 ROM (SHA256
6d43840d4099a74170ea0f0425b6257c3891ebcaa39c4d1840075a9ab22b5707), boardless,
8 MB Fast RAM, guest stack 8192, timeout 90 seconds. Each used a unique disposable
directory drive and locked serial port, with emulator configuration under the
owned build. No hardware disks, standing guests, lab CI or audit were changed.

| Native fixture at b6dc71ad6 | Actual verdict | Exit / host elapsed |
| --- | --- | --- |
| Kernel and production hooks | 23/23 | 0 / 15s |
| Cached callers | 25/25 | 0 / 16s |
| Context pause | 14/14 | 0 / 15s |
| Concurrent driver IO | 25/25 | 0 / 14s |
| IPv4 protocols | 19/19 | 0 / 16s |
| Clock lifecycle | 16/16 | 0 / 18s |

All **122/122 native cases** pass. Root verified exact binary/map hashes, every
receipt manifest, actual stdout, startup, guest exit, run-token identity, serial
and emulator logs: one boot per run, exit 0, no illegal/guru/alert or unexpected
ROM resets. Initial startup resets are excluded from the fault count. The local
emulator logs host Denise queue warnings, without a corresponding guest fault.
The local emulator/version differs from the prior lab run and is recorded,
not hidden. Host elapsed times are harness wall clock, not performance evidence.

The fixed kernel's original 22 assertions remain; its added 23rd case checks
real requester ACK-signal exhaustion and allocation rollback. Coverage includes
nested production hooks with retained mutex ownership and independent peer
progress, actual timer query and marked callback guards, prepared/finished worker
shutdown refusal, different-requester stop/start, generation restart, removed
manager refusal while a cache is live, then actual empty-domain ownership
transfer, clock join and resource recovery. Failed-guest fixture teardown remains
deferred; the original failure is not relabeled as a pass.

Evidence is indexed in
/Users/turo/ai/evidence/exec-threadx-kernel-lifecycle-final.json. New
`exec-threadx-kernel-final-*-local-native` receipts preserve the Mac runs;
original 65 failure receipts remain separate. Useful binary/map/build/link/review
and run evidence is retained. All task-created native staging, local emulator
home/locks, copied ROM and owned build outputs were removed after verification.
The existing emulator installation, user files, unmerged research branch and
vendor pins are preserved.

The actual full-feature replacement link at b6dc71ad6 is **FAIL with two
unresolved symbols**, down from 19: only `_tx_thread_relinquish` and
`_tx_thread_suspend` remain. This gate compiles the actual full-profile shipping
objects and research backend, including IPv6, with Werror. It removes the old
ThreadX scheduler/port archives and original baton object and uses no fixtures,
success stubs or unresolved-symbol bypass. The original backend diagnostic build
passes; its retained 395540-byte non-LTO image is unexecuted and is **not** an
A/B size measurement. Replacement full-library runtime remains NOT_RUN.

The new clock statistics describe the real retained clock but are preliminary:
service time currently counts nonempty application tick batches, and unsupported
stall/budget fields remain zero. They do not yet provide a complete equal-feature
performance comparison. Zero zombie counters follow the bounded public-delete
contract (finished/removed workers before control release), not proof of safe
arbitrary external removal. Suspend/relinquish, default shipping worker creation
and its stack policy, general dead-owner/close/cancellation behavior, enabled IPv6,
actual public sockets/device/ISR integration and full create/close remain OPEN.
Equal-feature image size, resident resources and performance, including the
removed original baton's synchronization, remain unmeasured. This checkpoint is
research-only and the branch remains unmerged.

### Public suspend/resume/relinquish and full-link closure (2026-10-08)

The research replacement now links the actual full-feature library with **zero
unresolved symbols**, including IPv6 and the existing NetX add-ons. The gate
compiles actual shipping objects and the research backend with Werror, removes
both the old ThreadX scheduler/port archives and the original baton object, and
uses no fixture, success stub or unresolved-symbol bypass. Static library entry
and version-tag checks also pass. **Full-library runtime remains NOT_RUN**;
link closure is a prerequisite for execution, not a functional completion or a
size/performance comparison. This branch remains unmerged and research-only.

Public explicit self-suspend executes the pinned upstream ThreadX body, retaining
the upstream API and control layouts. Its actual zero-tick explicit suspension
maps to an indefinite private owner wait without entering the application timer
list. Public resume completes the real retained generation. A new private
return-lifetime pin persists through READY until the suspended owner actually
returns; detach/quiescent checks include that pin. Both cached callers and
prepared native workers execute repeated suspend/resume cycles. Foreign READY
or blocked/delayed suspension remains unsupported and is refused before mutation.
Initial DONT_START worker resume retains its existing behavior.

Two native yield attempts actually **FAILED**: source 8317ac6c5 used same-value
SetTaskPri; source b80c05575 used a protected priority dip and restoration. Both
failed the unchanged strict equal-priority peer-progress assertion, printed no
buffered CASE_PASS lines, then timed out without a guest exit and reset during
failed-fixture teardown. Other six fixtures at those sources were NOT_RUN.
Original binaries/maps/stdout/serial/emulator receipts remain separate; later
passes do not relabel these failures. AgentNet's explanation of classic Exec's
scheduling decision is an inference, not a ROM-disassembly result.

Source 35e80a3e461d62cdcfeefbabd2cb32404d6bc410 instead yields through an actual
retained owner wait and passed all seven native fixtures, **150/150**. No extra
Task, control allocation or IO object is created per yield. Outside the released
context, an interrupt-protected ready-list scan checks for an equal/higher native
Task; when one is ready, the owner waits for a real TIMEOUT using a fresh private
generation, then restores its exact nested bracket. With no such ready Task it
skips the wait. ThreadX state, mutex ownership and native priority are retained.
This is a correctness baseline: workers request **1000 microseconds**, cached
callers use the central clock's deadline granularity, and scheduler load adds
latency. It is not proof of strict ThreadX round robin or a performance gain.

AgentNet claudecode independently reviewed the actual implementation, pinned
suspend body, lifetime/context/wait checks, CMake and full-gate wiring. All review
parts are retained in /Users/turo/ai/evidence/exec-threadx-schedule-source-review.txt.
The bounded source verdict is NO BLOCKER, with one recommended policy fix:
context_pause could release the boundary with a raised preemption threshold.
Final executable source **6eaf6d226afe1ce63cee331957fc6945d01e5a32** rejects that
pause before changing frames or releasing protection. Host and native assertions
use actual public preemption_change 16 -> 8, verify refusal with identity and
protection retained, then restore 16. The independent delta rereview confirms
F1 resolved and no bounded blocker. It does not approve full-library runtime.

Final host tests pass **48/48**. Seven artifacts compile with Werror and
startup-first validation. Actual native results at 6eaf6d226 are:

| Native fixture | Actual cases | Guest/harness exit |
| --- | --- | --- |
| Public scheduling | 29/29 | 0/0 |
| Kernel and production hooks | 23/23 | 0/0 |
| Cached callers | 25/25 | 0/0 |
| Context pause | 14/14 | 0/0 |
| Concurrent driver IO | 25/25 | 0/0 |
| IPv4 protocols | 19/19 | 0/0 |
| Clock lifecycle | 16/16 | 0/0 |

All **151/151** execute once serially on the existing local Mac Amiberry 8.3.0
(2026.08.05), supplied A1200 Kickstart 3.1 r40.68 ROM, boardless Fast 8 MB, guest
stack 8192 and timeout 90. Root verifies actual stdout/counts/exit, one matching
boot token per run, binary/map hashes and receipt manifests, and no guest
illegal/guru/alert or unexpected ROM reset. Host Denise queue warnings are
recorded without a corresponding guest fault. Run times are harness wall clock,
not benchmark results. The old strict peer-progress assertions are unchanged;
the added 29th scheduling case covers the review's threshold guard.

Evidence is indexed in
/Users/turo/ai/evidence/exec-threadx-public-schedule-checked.json. Earlier failed
and 150-case indexes/receipts remain separate. The exact clean 6eaf full-profile
gate is /Users/turo/ai/evidence/exec-threadx-schedule-library-gate-checked.json:
PASS_LINK_ONLY_UNVERIFIED, zero unresolved symbols, runtime NOT_RUN. Useful final
binaries/maps, failed scheduling artifacts, build/link/review and native receipts
are retained. Owned build outputs, disposable native drives/configs/serial files,
local emulator home/locks, staged helper and copied ROM are removed after proof
verification. User assets, emulator installation, hardware disks, upstream audit
and unchanged vendor pins are preserved.

Remaining limitations include public relinquish with a raised threshold or
native priority -128 (fatal through the public VOID unsupported path), foreign or
delayed suspension, default shipping worker creation/stack policy, general busy
removed-owner/cancellation/close behavior, actual public sockets/device/ISR and
enabled IPv6 execution. The priority -128 refusal is now conservative rather than
required by a priority dip; removing it needs its own reviewed contract. The
ready scan includes all qualifying Exec Tasks, not just ThreadX peers, so yield
cost can occur more broadly. Host-model internals/docs were not independently
reviewed as part of claudecode's implementation review.

The next integration step is **worker creation through the real library path**:
_tx_thread_create currently requires an idle-prepared record, otherwise returns
TX_NO_MEMORY, and preparation currently needs an 8192-byte stack while production
mDNS allocates 4096. Implement a retained manager-mediated creation/stack contract
before attempting real library open/create/close and public socket execution.
Then compare equal-feature image size, resident resources and performance,
including the synchronization formerly imposed by the original baton. No
shipping integration or improvement in those measurements is established yet.

### Manager-owned public creation and pinned IP lifecycle (2026-10-08)

Final executable source **d7c2e234ee22818899ab35833ec9d7e77b639bc4** adds automatic
worker creation in a normal caller context through the existing stable management
Task. It copies the command's scalar arguments into retained backend storage;
public control, name and stack remain caller-owned and must stay valid through
reservation cancellation or public deletion. The caller's exact normal context
chain is paused while the manager prepares the real worker and worker-owned IO,
using an allocated ACK signal. After restoration, ordinary tx_thread_create
publishes the unchanged pinned public control fields and created ring. No extra
management Task or standing pool of spare workers is added.

NetX's unchanged IP constructor raises the creator threshold before calling
thread_create and ignores its return status. A research-only parent netstack
preflight therefore reserves the exact helper/control/name/public-stack tuple
before invoking that body. The reserved bind does not wait or drop protection,
including under a raised threshold/nested bracket. Early constructor errors
cancel any unconsumed reservation; an apparently successful constructor without
a bound helper is fatal. This is compiled only with AMINETXDUO_EXEC_RESEARCH.
The diagnostic gate recompiles the actual parent netstack.c with all shipping
feature/layout flags plus this definition and replaces exactly that object in a
private archive. Vendor bodies, APIs/layouts and pins remain unchanged.

The manager owns the private record and ACK signal; the originating client Task
and retained identity stamp authorize binding, unused cancellation and deletion.
Public deletion first proves actual worker completion/removal and closed owner
IO. It clears public ID/created links under the current boundary, clears the
private public-control/client pointers, publishes RETIRE_PENDING and signals the
verified live management Task. This is nonblocking, including under the pinned
IP delete body's preemption-disable counter. The public control and supplied
stack can be immediately overwritten/reused. The manager drains pending records
at every loop entry, before every command/STOP, and touches only private storage:
its own ACK, guarded owned native stack and record. Retained resource counts and
runtime holds persist until that actual drain; a signal alone is not reclamation.

Aligned public stacks must meet the pinned TX_MINIMUM_STACK of 1024. A supplied
stack of at least 8192 is also used as the native stack. Smaller public stacks
retain their original public fields and range but currently receive an owned
8192-byte native stack with two guard words (8200-byte allocation), plus the
private record and worker IO. Both public/native ranges are probed; both guards
are checked before native storage release. This is a conservative research floor,
not a measured stack requirement. The resource getter counts private record and
owned native allocation bytes, not all resident RAM/IO allocations or high-water.
Small-stack fields do not describe the actual native running stack; native
snapshots expose it separately. The extra memory cost must be included in the
future equal-feature comparison and sized from actual high-water measurements.

Initial implementation **06f85486b** actually passed all nine native fixtures,
190/190, and host 48/48. Its full-profile parent preflight compile FAILED because
TX_THREAD_ID's internal header was missing; the replacement link was NOT_RUN.
The first native harness attempt also refused before boot because its owned lock
directory was missing: that attempt is NOT_RUN, with no serial/token/guest verdict.
Both are retained distinctly from successful execution. Earlier incremental
compile failures remain in build logs and are not counted as passes.

Independent AgentNet claudecode reviewed the actual implementation/parent/gate
and found R1: a missing retirement hook could return a status dropped by an
unchecked upstream delete. The final source makes that invariant fatal and
refuses unsupported managed terminate/delete under preempt_disable fatally
before an unchecked caller can discard the failure. Checked ordinary live
public deletion still returns DELETE_ERROR. Manager-side record traversals are
also protected with Forbid against concurrent legacy record removal. All design,
source review/resend and final delta parts (64 physical messages) are retained in
/Users/turo/ai/evidence/exec-threadx-managed-source-review.txt. Final bounded source
rereview: NO BLOCKER, R1 resolved. Fatal guard paths are not exercised natively;
smoke internals/host models were not independently reviewed. preempt_disable is a
bounded fail-closed heuristic, not a universal unchecked-caller detector.

Final host tests actually pass **48/48**, and all nine native artifacts compile
with Werror/startup-first checks and execute once serially:

| Native fixture at d7c2e234e | Actual cases | Guest/harness exit |
| --- | --- | --- |
| Managed workers and pinned IP lifecycle | 25/25 | 0/0 |
| Public scheduling | 29/29 | 0/0 |
| Kernel/production hooks | 23/23 | 0/0 |
| Cached callers | 25/25 | 0/0 |
| Context pause | 14/14 | 0/0 |
| Concurrent driver IO | 25/25 | 0/0 |
| IPv4 protocols | 19/19 | 0/0 |
| Clock lifecycle | 16/16 | 0/0 |
| Legacy prepared public workers | 14/14 | 0/0 |

All **190/190** native checks pass on the existing Mac Amiberry 8.3.0
(2026.08.05), supplied boardless A1200 Kickstart 3.1 r40.68, Fast 8 MB,
guest stack 8192, timeout 90. Root verifies all 18 original/final receipt manifests,
binary/map hashes, stdout counts/exits, one matching boot token per run and no
illegal/guru/alert or unexpected ROM reset. Host Denise warnings are recorded
without a guest fault. Harness wall-clock times are not performance results.
The managed fixture covers actual ACK exhaustion/rollback, refusal before
publication, client-stamp mismatch, unused cancellation, four worker lifecycles,
4096 public versus 8192 native and supplied 8192 reuse, delayed DONT_START resume,
nonblocking deletion under preemption-disable, immediate public storage reuse,
private drain, guards and final kernel cleanup. It also executes the actual
pinned IP constructor/helper, controlled boardless driver initialization, private
stop only after timer/event quiescence and real IO/Task removal, then the unchanged
pinned IP delete body. Its fixture PRNG/controlled driver are explicit: this is
not production entropy, real-device/ISR or whole-library socket evidence.

Evidence is indexed in /Users/turo/ai/evidence/exec-threadx-managed-final-workers.json.
The exact clean final full-feature gate, including IPv6 and the parent preflight,
is /Users/turo/ai/evidence/exec-threadx-managed-library-gate-matched.json:
**PASS_LINK_ONLY_UNVERIFIED, zero unresolved symbols**. Actual static entry and
version checks pass. A preceding diagnostic used cached shipping objects; a
final original rebuild changed generated commit tags, so its different hash was
caught and the matched gate rerun against the fresh final objects. The two
reports remain separate. Full-library runtime is still **NOT_RUN**; diagnostic
non-LTO image bytes are not an A/B size or performance result.

Useful final binaries/maps, original managed artifact, all 18 native receipts,
preboot refusal and compile/link/review evidence are retained. Owned build,
all native directory drives/configuration/serial files, copied ROM and staged
helper are removed after verification; redundant intermediate adjacent artifacts
are retired explicitly without changing their actual verdicts. User assets,
installed emulator, unmerged branch, hardware disks and upstream audit are
preserved. This remains research-only and is not a shipping integration.

Next: execute the actual full library open/create/close/public socket path,
including parent integration of quiescent helper stop, remaining add-on/reader
shutdown, and library creation/close from different caller Tasks. Private helper
stop is not arbitrary ThreadX termination. Other unchecked callers outside the
matched reserving client, removed/busy-client ownership, general forced
termination/cancellation, actual device/ISR and IPv6 execution remain OPEN.
Then measure equal-feature image size, complete resident resources/high-water,
yield latency and performance against the original ThreadX/baton backend.

### Actual loaded full-library loopback lifecycle (2026-10-08)

Final executable source **3021975e7face41b5bcb6ff7e00617e547605f4e** now runs
through the actual full-profile bsdsocket.library, loaded with OpenLibrary and
called through its public LVOs. The standalone public test links only command
startup and DOS/Exec support: no embedded backend/NetX stack, fixture entropy or
replacement library callbacks. The original ThreadX/baton library is the control.
Both use the same complete feature profile and original vendor pins; these are
diagnostic non-LTO builds. TCP handler is configured OFF in the disposable,
boardless A1200 guest, so physical drivers and add-on workers are not exercised.

Three runtime integration changes were necessary:

- Managed workers bind to library-owned public storage. Once BOUND, the temporary
  startup Process no longer authorizes their lifecycle: its client pointer/stamp
  are cleared. Registered normal, non-target application contexts may perform
  the exact quiescent private stop and FINISHED public deletion. The management
  creator, marked/unregistered clock and target remain excluded. PREPARED
  reservations/cancellation and legacy ACK owners retain their original strict
  authority. Bound retirement is public nonblocking delete plus private drain.
- A Task can open two private library bases. A new adoption may evict its other
  handed, identity-matched, quiescent DORMANT cache after successfully allocating
  the new signal; ACTIVE/unhanded records still refuse. Existing generation and
  signal-debt recovery reclaims the evicted base's signal on its next entry or
  close. Switching bases pays allocation/free/generation overhead, unmeasured.
- Exec enters the actual Close vector under its own Forbid. The research Close
  path releases exactly that one level only at TD==0 and ID<0, keeping the open
  count as a segment pin through teardown, then restores Forbid before count
  decrement and deferred expunge. Additional caller Forbid/Disable refuses before
  mutation. The gate recompiles actual library.c with its own full command flags
  plus the research definition and swaps exactly that explicit link object.

The research parent shutdown refuses physical interfaces, retained readers and
AutoIP/DHCP/mDNS/DHCPv6 producers. It deactivates the actual IP timers and waits
at most 100 ticks outside protection for the exact parked event safe point,
actual terminal IO closure and Task removal, before unchanged pinned nx_ip_delete.
After STOPPING, a terminal marker closes admission immediately: retained storage
is for reap retry, never resumed service. Before STOPPING, refusal can restore
previous timer states. Successful kernel retirement clears the marker; startup
requires that success before allocating a replacement domain.

Independent AgentNet claudecode reviewed the implementation and bounded repairs:
L1 terminal admission, L3 explicit segment unload and L4 manager exclusion were
resolved. M1 allocator-ABA concern was withdrawn after startup gating was proved.
The final test-only dispatch repair and DOS string cast have NO BLOCKER. Full
review parts are retained in
/Users/turo/ai/evidence/exec-threadx-library-lifecycle-source-review.txt. Initial
smoke flow and host-model internals were not fully independently reviewed;
source approval is not a whole-platform runtime verdict.

Actual final receipts at the clean executable source prove:

| Check | Actual result |
| --- | --- |
| Original full library, two load/use/close/unload cycles | 16/16 PASS |
| Exec replacement, same public cycles | 16/16 PASS |
| Extra caller Forbid/Disable and explicit research retry | 5/5 PASS |
| Nine adjacent native fixtures | 190/190 PASS |
| Host models/contracts | 48/48 PASS |
| Full-profile replacement link | Zero unresolved; link-only gate PASS |
| Actual static library entry/version checks | PASS |

The public cycles execute IPv4 UDP, IPv6 UDP and IPv4 TCP data between private
bases, close an evicted base while the second survives, then verify actual helper,
kernel and health removal. The real Expunge LVO returns a segment that is actually
UnLoadSeg-ed before reload; final Task signal allocation exactly matches entry.
The separate five-case probe preserves extra exclusion/count/base/health and
then explicitly retries in normal context. CloseLibrary returns void: real
clients cannot learn this refusal and will generally never retry, leaving a
permanent library pin. Deferred cleanup is OPEN; the probe is not a supported
application recovery policy. Terminal timeout/fatal/busy-removed-owner paths
remain unexercised, as do physical/add-on startup rollback and shutdown.

All native receipts use the existing Amiberry 8.3.0, Kickstart 3.1 r40.68, boardless
A1200 Fast 8 MB, guest stack 8192 and timeout 90. Exact case counts, guest/harness
exits, one actual boot token, binary/library hashes and no illegal/guru/alert or
unexpected ROM reset are checked. Harness elapsed time is not performance data.
Earlier real failures are preserved: second-base socket errno50 (3/16), final
close refused by TD=0/ID=-1 (6/16), and incorrect EXCLUSION argument dispatch
(actual16 versus expected5, FAIL). The final parser uses GetArgStr's raw DOS line,
requires an exact token and rejects unknown non-empty input; Werror caught and
required an explicit signedness conversion. No failed receipt was relabeled.

The earlier FILE-size comparison reported 395,532 baseline bytes versus
463,680 research bytes, or +17.23%. **That comparison was invalid:** CMake
stripped the baseline, while the diagnostic relink retained research symbols.
Applying the same strip tool to that exact research artifact yields **418,860
bytes (409.04 KiB)** versus **395,532 bytes (386.26 KiB)**: **+23,328 bytes /
+5.90%**. The original unstripped artifacts and verdicts are retained; they are
not a matched size comparison. The matched LTO results follow below. No resident
RAM/high-water, latency or throughput improvement is established. Conservative
owned native stacks for small public stacks still add 8200 allocation bytes plus
record/IO overhead; include these in the later complete resource comparison.

Evidence and retained binaries/compressed maps are indexed in
/Users/turo/ai/evidence/exec-threadx-library-lifecycle.json. Task-owned builds,
disposable guest directory drives/configs/logs, staged helper and copied ROM are
removed after manifest verification. Vendor pins, user assets, hardware disks,
standing guests, upstream audit and the unmerged research branch are preserved.

Next: complete physical/add-on lifecycle and deferred close ownership, then
measure equal-feature LTO image size, complete resident resources/high-water and
performance against ThreadX/baton. This checkpoint proves public loopback use
and normal unload, not a shipping-ready replacement.

### Matched full-feature LTO size comparison (2026-10-08)

Executable/gate source **5dbe7c53b454695f936735fdd06e9c88dfab93fd** rebuilds
both libraries with the same shipping Release profile, CPU any (-m68000),
-Os -flto -fno-ident, Werror, identical feature/layout definitions and unchanged
vendor pins. No feature pruning or protocol fixtures are used. The gate requires
--lto to agree with actual compile and link flags, uses GCC plugin-aware ar for
slim IR, and applies the full build's exact KEEP_SYMBOLS/CMAKE_STRIP policy in
both LTO and non-LTO modes. All original library vector/romtag roots and calling
conventions remain; only the reviewed research scheduler/parent integration
objects differ. AgentNet claudecode reviewed both gate deltas: NO BLOCKER,
LOW unmatched diagnostic stripping resolved. Runtime backend sources unchanged.

| Same full feature set, symbols stripped | File bytes | KiB |
| --- | --- | --- |
| Original ThreadX/baton, LTO | 355,340 | 347.01 |
| Exec research replacement, LTO | 375,240 | 366.45 |
| Difference | +19,900 | +19.43 (+5.60%) |

The research LTO relink before stripping is 442,136 bytes, illustrating why that
post-processing must match before comparing. The corrected earlier non-LTO
comparison is +5.90%; the prior +17.23% mixed-symbol-policy claim is withdrawn.
LTO does not establish a size saving for this prototype. RAM/high-water,
synchronization overhead, latency and throughput are still unmeasured. The
user's continuation criterion is a size saving or significant performance gain;
neither benefit has been established. Do not infer performance from run duration.

Both newly built LTO libraries actually execute the retained public LVO-only
fixture: original **16/16 PASS**, research **16/16 PASS**, each two real loaded
IPv4 UDP/IPv6 UDP/IPv4 TCP and close/Expunge/UnLoadSeg/reload cycles. Research
extra Forbid/Disable probe **5/5 PASS** with explicit test-only retry. Every
receipt has clean source, exact loaded-library hashes, guest/harness exit0,
one real boot token and no illegal/guru/alert or unexpected ROM reset. The test
command binary is the unchanged non-LTO public ABI fixture compiled at3021975e;
it embeds no backend. Loaded libraries/vendor/backend use the new full LTO
build. Baseline post-build and research explicit entry/version checks pass;
replacement link has zero unresolved symbols. These are actual new executions,
not reused native results. Physical/add-on/deferred-close limitations remain.

Small useful libraries/compressed maps, configuration/commands, review and native
receipts are indexed in /Users/turo/ai/evidence/exec-threadx-lto-comparison.json.
Task-created full/relink builds, copied ROM, staged helper and all three disposable
guest drives/configs/logs are removed after verification. User assets, hardware,
standing guests, unmerged branch, vendor pins and upstream audit are preserved.

## Performance comparison: 2026-10-08

Actual source **363576077bc3271057c8f681c0174bc8c95b8c9c**, clean and
unmerged. Five fresh pairs execute the same public LVO-only command against
matched full-feature CPU-any Release/Os/LTO stripped libraries, in AB/BA/AB/BA/AB
order. A1200 emulator uses 68020, multiplier4, JIT0, no warp; unchanged CPU
kernels calibrate 13.048–13.077 MHz in each guest boot. EClock rate709379 Hz.
Both DOS peers have priority0. No embedded backend/NetX or fixture entropy.
The tester alone uses68020 for the existing assembly calibration kernels.

All **ten actual native boots PASS6/6 (60 case verdicts)** and all **48 host
models/negative guards PASS**. Every final boot has guest/harness exit0, one
boot token, no terminal panic/illegal/guru/reset, and identical tester, ROM,
emulator, harness, normalized configuration and effective TCP windows. Only
owned disk paths, serial port and descriptions are normalized out. Parent
RCV/SND25088/8192, peer50176/8192. Every 64B reply and the entire1MiB stream is
verified after its measured interval. Bulk stops after peer drain, before
verification, release ACK and teardown. Final entry/version checks and link pass.

| Metric (fastest per arm over five boots) | ThreadX/baton | Exec prototype |
| --- | ---: | ---: |
| Full LTO library file bytes | 355,340.00 | 375,492.00 |
| Cold first socket, us | 1,031.89 | 1,726.86 |
| Cached FIONREAD, us/call | 362.82 | 820.56 |
| Alternating private base, us/call | 364.31 | 1,773.78 |
| TCP 64-byte request/reply p50, ms | 8.11 | 15.58 |
| TCP 1 MiB drain, KiB/s | 213.67 | 109.93 |

The prototype is **20,152 bytes larger (+5.67%)** including retained terminal
panic diagnostics. TCP throughput is about **48.5% lower**; the paired median
latency ratio is **2.03x**, cached call ratio **2.26x**, base-switch ratio **4.87x**.
Raw values, paired medians/ranges and each receipt are retained in
/Users/turo/ai/evidence/exec-threadx-perf-comparison.json. Connected guest RAM
consumption medians are935936B baseline and936480B prototype (+544B); these are
whole-guest snapshots including client/OS allocations, not backend peak RAM or
proof of a leak. Post-unload consumption is12,560–12,584B in both arms relative to pre-open;
these snapshots cannot isolate OS caching from library allocations.

The initial prototype benchmark really failed. Terminal diagnostics identified
`_tx_time_get`: production WaitSelect reads time outside its NetX bracket,
which the upstream read-only API permits, but the prototype required a caller
frame. The independently reviewed research correction serializes the read with
platform enter/leave, preserving caller state and the other strict guards.
Regression checks cover no frame, an existing outer exclusion, a caller frame
and a marked frame. Temporary service-name instrumentation is removed so existing
negative tests retain their exact messages. Compile failures, rebooting pilots
and their distinct SHAs remain recorded as FAIL/NOT_RUN and excluded from the
comparison; final results are fresh executions after the fix.

**Continuation benefit criterion UNMET:** this prototype is larger and slower
in every measured operation. The benchmark is complete; further redesign is a
separate decision. Measurements cover boardless IPv4 loopback, not physical
network/hardware or ISR compatibility. The branch remains research-only.
Useful libraries, command, compressed maps, source review, configuration, hashes
and reproducible measurement scripts are retained under
/Users/turo/ai/evidence/exec-threadx-perf-artifacts. Owned builds, copied ROM,
helper and all14 disposable guest stages are removed after receipt verification;
user assets, hardware disks, standing guests, audit, pins and other owners remain.
