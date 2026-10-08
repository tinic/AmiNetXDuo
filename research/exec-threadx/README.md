# Exec / ThreadX compatibility research, spike 1

This directory is an isolated, standalone CMake project. The parent build,
shipping presets and vendor sources do not select it. It is not a complete
ThreadX backend and must not be installed on a machine as one.

## What is implemented

- A dependency capture/check tool using each production build's compiler and
  preprocessor flags. It inventories calls, internal identifiers, structure
  fields and source macros/types. Pinned ThreadX headers are fingerprinted.
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

The cleanup hook in the primitive is a queue-removal hook under protection; it
is not yet a binding to NetX's ThreadX cleanup callbacks. It must not reenter
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

## Suspension semantics to implement next

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

## Still open

The current-thread model, ThreadX wait-list/cleanup binding, mutexes, event flags,
thread lifecycle, common timer integration and replacement backend selection are
unimplemented. Minimum-profile coverage, actual native wakeup/resource races,
NetX/socket conformance, independent review and net size/runtime comparison remain
pending. Passing the model does not validate those parts. The compile probe
checks that referenced fields exist; target/profile-specific layout expectations
and link checks against a replacement remain to be added.

See [the research plan](../../docs/plans/exec-threadx-compat.md).
