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
