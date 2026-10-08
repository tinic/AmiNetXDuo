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
- Full compiled-source coverage includes `_tx_thread_wait_abort` through DTLS
  receive; micro coverage does not. Compiled coverage includes modules that may
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

## Still open

Full current-thread/adoption semantics, blocking mutexes, event waiters, thread
lifecycle, priority/preemption semantics, common timer integration and replacement
backend selection remain unimplemented. Minimum-profile coverage, broader native schedules,
UDP direct cleanup, NetX/socket conformance and net
size/runtime comparison remain pending. The compile probe checks that referenced
fields exist; target/profile-specific layout goldens and full replacement link
checks remain to be added. No complete backend or size-saving claim exists.

See [the research plan](../../docs/plans/exec-threadx-compat.md).
