# Exec resources

Every `AllocSignal()`, `MsgPort`, `IORequest` and message send in the tree, who
owns it, who waits on it, and what releases it.

This is the companion to `docs/ALLOCATIONS.md`, for the class of bug that has no
Linux, Windows or macOS equivalent. Four facts drive all of it:

* **`AllocSignal()` is permanent.** There are 32 bits per Task, no way to
  recover one, and no reclamation until the Task exits. A library that takes a
  bit out of its caller and does not give it back has cost that caller a bit for
  the rest of its life. Thirty-two open/close cycles and nothing on that Task
  can allocate a signal again.
* **A `MsgPort` names a Task.** `CreateMsgPort()` takes `mp_SigBit` from the
  creating Task's signal allocation and points `mp_SigTask` at it, and
  `DeleteMsgPort()` frees that bit out of whichever Task calls it. A port
  created on one Task and waited on by another, or created on a Task that then
  exits, is a `Signal()` into memory Exec has reclaimed.
* **`AbortIO()` is a request, not a command.** Commodore's `a2065.device` 2.16
  does not honour it on a queued `CMD_READ`. `WaitIO()` has no deadline. A
  device that declines both owns your request and your reply port until it feels
  otherwise.
* **An `IORequest`'s memory and its reply port must outlive the request.**
  Not "until `CheckIO()` says done", until `WaitIO()` has returned.

The project has been bitten twice already. `544398f` is the first:
`CreateMsgPort()` on a throwaway bring-up Process, waited on by the Task that
dropped the last library reference, and the last `CloseLibrary()` never
returned. `src/sana2/sana2_rx.c` is the second: a driver that refuses `AbortIO`
leaves a reader holding reads the shim cannot take back, and the deadline and
the deliberate 4 KB leak there are what that cost.

**`grep` needs `LC_ALL=C grep -a`** in this tree, the NDK headers are Latin-1
and a plain `grep` reads them as binary and silently returns nothing.

Counted across `src/`, `port/threadx-amiga/src/`, `tools/smoke/`, `tests/` and
`clients/`: 16 `AllocSignal`, 22 `CreateMsgPort` plus 5 open-coded ports, 11
`CreateIORequest`, 21 `OpenDevice`, and 54
`SendIO`/`DoIO`/`WaitIO`/`AbortIO`/`CheckIO`. **Ten defects; all ten are fixed
here.**

---

## The ten

| # | Site | What went wrong | Fixed by |
|---|---|---|---|
| 1 | `src/bsdsocket/select.c:475` → `library.c:276` | `WaitSelect()` took a signal bit out of its caller and `bsd_child_destroy()` never gave it back | `ami_signal_free(sb_TimerSignal)` in the destroy |
| 2 | `src/bsdsocket/select.c:497` | the `OpenDevice` failure freed the bit but left `sb_TimerSigMask` naming it | mask and bit cleared with it |
| 3 | `src/sana2/sana2_rx.c:840` | an orphaned reader's Task was deleted under its own live reply port | orphan branch leaks the thread and stack too |
| 4 | `src/sana2/sana2_tx.c:231` → `sana2_device.c:640` | `ami_sana2_close()` freed the interface with `CMD_WRITE`s still queued against it | `tx_orphaned`, mirroring `rx_orphaned` |
| 5 | `src/sana2/sana2_internal.h:92` | the raw probe's deadline-free `WaitIO()` defaulted **on** outside CMake | header default flipped; CMake names both answers |
| 6 | `port/threadx-amiga/src/tx_amiga_adopt.c:535` | the branch written to recover an adopted thread's run signal was unreachable | `tx_thread_amiga_signal_owner` |
| 7 | `port/threadx-amiga/src/tx_amiga_adopt.c:463` | `tx_amiga_discard_thread()` dropped a recoverable bit when the owner called it | frees it when the caller is the owner |
| 8 | `src/common/compat.c:186`, `src/tls/tls_amiga.c:36` | two lazy `OpenDevice`s with no lock and no close, in library segments that get unloaded | semaphore + `ami_timer_close()` / wired-up `ami_tls_timer_close()` |
| 9 | `src/common/compat.c`, `src/tls/tls_amiga.c` | the device base doubled as the "usable" flag, so the fast path could see a live base and a zero rate | an explicit ready flag, set last |
| 10 | `tests/tcpdrill/tapdev.c:743` | timer port and device acquired before `tap_dev`, teardown gated on `tap_dev` | `tap_timer_close()` on both failure returns |

---

## 1. `WaitSelect()` leaked a signal onto every caller

`bsd_timer_open()` (`select.c:468`) is lazy: the first `WaitSelect()` that passes
a timeout opens `timer.device` and takes a signal bit **out of the calling
task**, which is the task the base belongs to. `bsd_child_destroy()`
(`library.c:263`) closed the device and freed `sb_EventSignal`, and never
touched `sb_TimerSignal`.

So an ordinary program, `OpenLibrary("bsdsocket.library")`, `WaitSelect()` with
a timeout, `CloseLibrary()`, lost one of its 32 bits per cycle, for good. A
long-lived program that opens and closes the library around each job runs out.
CycleDrill's `phase_opens` nests eight opens a cycle: four cycles.

`sb_EventSignal` next to it was always correct, and its comment
(`bsdsocket_internal.h:414`) states the rule the timer bit broke: "allocated by
the opening task ... a base belongs to exactly one task".

**CycleDrill could not have caught it.** It reads `tc_SigAlloc` precisely because
"a library that AllocSignal()s on its opener and does not free it shows here and
nowhere else", and it never called `WaitSelect()`, so the one code path that
does that was never reached. It now passes a timeout on every base it opens, in
all three phases, and asserts inside a single cycle that the nested opens gave
every bit back rather than waiting for the drift table to show it.

The `OpenDevice` failure path did free the bit, but left `sb_TimerSignal` and
`sb_TimerSigMask` pointing at it. Not live, everything that reads the mask is
behind `timer_running`, which is only set after a successful open, but it is
stale state on a half-built object, which is the shape the rest of this document
is about. Cleared.

---

## 2. A SANA-II reader's Task deleted under its own reply port

`ami_sana2_rx_teardown()` (`sana2_rx.c:426`) is careful about this and says so:
when a device will not give a `CMD_READ` back, it releases nothing, not the
pinned packets, not the reply port, because "each of those is a pointer the
device still holds". Then `ami_sana2_rx_stop()` ran
`tx_thread_terminate()`/`tx_thread_delete()` and freed the reader's stack.

The port survives. The Task it names does not. `mp_SigTask` points at that
reader, so exec's `PutMsg()`, called from the device, possibly at interrupt,
`Signal()`s a Task whose control block and stack have been handed back.

The branch immediately above it already had the right answer for the case where
the reader would not *exit*: log, set `rx_orphaned`, `continue`, leak the stack.
The case where the reader exited but left reads behind fell through to the
teardown. It now takes the same route, for the reason the port makes necessary.

Also reset in `ami_sana2_rx_start()`: `wake_mask` and `orphans` survived a
restart, so a reader that failed to get a `MsgPort` on the second run was
`Signal()`led on the first run's bit.

---

## 3. The transmit ring had no orphan guard at all

`ami_sana2_tx_drain()` aborts every busy slot, spins 64 ticks, and on failure
warned "TX ring did not drain" and returned. `ami_sana2_close()` then called
`CloseDevice()` and `ami_free(iface)`.

`slot->req` is `iface->tx[i].req` and its `mn_ReplyPort` is `&iface->tx_port`.
Both are fields of the allocation being freed. A device still holding a
`CMD_WRITE` writes its completion into whatever takes their place, and
`CloseDevice()` with requests outstanding is illegal in its own right.

The receive side has understood this since the reader work: `rx_orphaned` stops
the close. The transmit side has the same exposure through the same struct and
had nothing. `tx_orphaned` is assigned rather than or'ed on every drain, so a
later drain that gets everything back clears it and an interface bounce still
recovers; both places `ami_sana2_close()` would free the interface now test it,
and `ami_sana2_orphaned()` reports either.

---

## 4. The raw probe's deadline-free `WaitIO()` defaulted on

`ami_sana2_probe_raw()` (`sana2_device.c:294`) posts a raw `CMD_READ`, calls
`AbortIO()` and then `WaitIO()` with no deadline. `CMakeLists.txt:130` documents
what that costs on `a2065.device` 2.16, "ami_sana2_open() never returns,
verified under FS-UAE, 2026-07-25", and turns it off.

`sana2_internal.h` defaulted it to **1**. The CMake option only defined
`AMI_SANA2_PROBE_RAW=0` in the OFF direction and relied on the header for ON, so
a build that did not go through CMake got the hang. The safe answer is now the
header's, and the option names its answer both ways.

The probe itself is left as it is. It is opt-in, it is the only way to learn
whether a device implements `SANA2IOF_RAW`, and `AMI_SANA2_RAW_DEFAULT` is 0 so
its result is discarded unless a caller has separately opted into raw framing.
Making it safe means never posting a stack-local request, see "Known and not
fixed".

---

## 5. An adopted thread's run signal could not be recovered

`tx_amiga_orphan_thread()` (`tx_amiga_adopt.c:508`) has a branch for the case
where the `TX_THREAD` has already been torn down, and its comment names the
route: "someone called tx_thread_terminate() plus tx_thread_delete(). Just
recover the signal."

It was unreachable. `_tx_thread_delete()` invokes the port's delete completion,
which is `_tx_amiga_reap()`, and for an adopted thread that returns at
`tx_thread_schedule.c:186-194` after doing one thing:

```c
thread_ptr -> tx_thread_amiga_task =  (VOID *) 0;
```

The ownership test above the branch compared exactly that field against the
caller, so every arrival took `TX_CALLER_ERROR` and the recovery never ran. That
clearing is deliberate and correct, the scheduler must not `Signal()` a Task
that is no longer a thread, so the same field cannot also be the record of who
owns the signal.

`tx_thread_amiga_signal_owner` is that record. Set at adoption, cleared by
whoever frees the bit, untouched by teardown. The torn-down test now runs before
the ownership test and against the new field.

The reachable caller is `ami_netstack_enter_cached()`
(`src/netstack/netstack.c:205-219`), whose comment asserts "tx_amiga_orphan_thread()
handles a TX_THREAD that has already been deleted". It did not. Each pass
through that path lost one of the calling task's 32 bits, and after 32 every
`tx_amiga_adopt_thread()` on that task returns `TX_NO_MEMORY` for ever, which
is a socket vector that can no longer enter the stack.

`tx_amiga_discard_thread()` is the second half. It exists for the case where
somebody *else* is tearing the thread down, where `FreeSignal()` cannot work
because the bit belongs to another task, and `netstack.c:310-312` documents that
intent. But `netstack.c:219` and `:306` both reach it on the owning task, where
the bit is recoverable, and it dropped it anyway. It now frees the bit when the
caller is the recorded owner and does exactly what it did before otherwise.

---

## 6. Two lazy `timer.device` opens with no lock and no close

`ami_timer_init()` (`src/common/compat.c`) and `ami_tls_timer_open()`
(`src/tls/tls_amiga.c`) are the same shape: test a file-scope base pointer, then
`OpenDevice()` a file-scope `timerequest`.

**The race is reachable.** `ami_millis()` is called from SANA-II reader Tasks by
way of `bpf_amiga.c:92` while application tasks call it too, and
`ami_tls_eclock()`, `ami_tls_eclock_hz()` and `ami_tls_eclock_micros()` all reach
the TLS one with no lock of their own from whatever task is doing crypto. Two
tasks that both fail the base test both open the same static request: the second
overwrites `io_Device`, and `timer.device` is one open up with no second request
to close it against.

A `SignalSemaphore`, not `Forbid()`: `OpenDevice()` may `Wait()`, which breaks a
`Forbid` and would make it a lock in name only. The semaphore's own one-time
init is the `Forbid`-and-flag shape `netstack.c:54` already uses.

The fast path needed a flag of its own, and this is the part worth reading
twice. The obvious answer is to publish `TimerBase` last, after the rate and the
accumulator are set, so nobody on the fast path sees a half-built timer. It does
not work: the NDK's `ReadEClock()` is an inline that resolves the library base
*through* `TimerBase`, so `TimerBase` has to be set before the rate can be read,
and it therefore cannot also be the "usable" flag. Both timers now have an
explicit ready flag, set after the last field and cleared first on close, and
both fast paths test that. `ami_tls_timer_is_open()` answers from it too, since
`ami_tls_crypto.c:69` calls `ami_tls_eclock()` on the strength of that answer.

**Neither had a `CloseDevice` on any path.** Both requests are statics in a
library segment. `bsd_lib_expunge()` hands that segment to `UnLoadSeg()` with
`timer.device` still open against memory inside it, and does so again on every
load/expunge cycle, which is precisely what CycleDrill's phase E drives.
`ami_timer_close()` is new and called from `bsd_runtime_close()`;
`ami_tls_timer_close()` already existed but was called only from `tests/`, and is
now called from `tls_runtime_close()`. The TLS one also resets `ami_tls_hz`, or a
reopen keeps a rate read through a base it no longer holds and
`ami_tls_eclock_micros()`'s `hz == 0` guard never re-arms.

Both ports stamped `mp_SigTask` with `FindTask(NULL)` despite being `PA_IGNORE`.
Nothing reads it today. The task recorded exits long before the library does, so
the first person to give either port a reason to be signalled inherits a
`Signal()` into freed memory. `NULL` instead, with the reason written down.

---

## 7. `tapdev.c`, the `sana2_rx.c` shape again

`tap_install()` creates `tap_timer_port` and opens `timer.device` at `:743-758`,
**before** `tap_dev` is set at `:796`. `tap_remove()` returns immediately on
`tap_dev == NULL`. Both failure returns in between, `MakeLibrary` failed, the
transmit ring's `AllocMem` failed, lost the port and the open device.

This is the exact defect the reader-stack finding was: a resource acquired
before the flag the teardown gates on. `tap_timer_close()` is factored out and
called on both.

---

## Everything else, and why it is clean

### Ports created and waited on by the same task

| Port | Created on | Waited on by | Deleted by |
|---|---|---|---|
| `netstack_rexx.c:380` AMITCP | the ARexx host Process | itself (`:434`) | itself (`:453`) |
| `tcp_handler.c:733` session | the session Process | itself (`:840`) | itself (`:895`) |
| `tcp_handler.c:976` control | the control Process | itself (`:1042`) | itself (`:1119`) |
| `sana2_rx.c:483` reader reply | the reader thread | itself (`:563`) | itself (`:469`) |
| `sana2_rx.c:337` flush | the reader thread | itself (`DoIO`) | itself (`:355`) |
| `sana2_device.c:119` command | the calling task, per call | itself (`DoIO`) | itself (`:140`) |
| `sana2_device.c:555` open | the opening task | not waited on | itself (`:572`) |
| `sntp.c:251`, `tool_diag.c:284` | the command's Process | itself | itself |
| every port in `port/threadx-amiga/` | function-local, one task | itself | itself |

`ami_sana2_command()` is the one worth naming: it creates a **fresh port per
call** and says why, "control commands come from whichever task is driving
(startup task at open, IP thread from the driver entry) and DoIO() waits on
mn_ReplyPort->mp_SigTask, so a cached port would signal the wrong task". That is
the 544398f lesson applied before it was learned.

`iface->tx_port` is not a `CreateMsgPort()` at all: it is built in place by
`ami_sana2_port_init()` as `PA_IGNORE` and owns no signal bit. A reader binds
itself to it under `Disable()` (`sana2_tx.c:126`) and unbinds before it exits or
frees its bit (`sana2_rx.c:574`), which is the right order. `Disable()` rather
than `Forbid()` because a device may `ReplyMsg` from its interrupt and exec's
`PutMsg` reads all three fields as one unit.

### The AMITCP stop handshake is not a port, on purpose

`netstack_rexx.c:96-115` records why, and it is the fix from 544398f: startup and
shutdown run on different Tasks, so the stopper allocates the signal in its own
task and registers itself, and the host only sets a flag and pokes whatever is
registered. `ami_netstack_rexx_stop()` frees the bit it allocated, on the task
that allocated it, and falls back to `Delay()` polling when no bit is free
rather than skipping the wait.

`ami_netstack_rexx_start()` uses `SIGF_SINGLE` for its own bring-up handshake,
which would collide with the ThreadX run-signal on an adopted task. It cannot:
`netstack.c:1343-1347` orphans the caller from ThreadX before calling it.
`bsd_netstack_bringup()` (`library.c:343`) uses a private `AllocSignal()` for
the same reason, states it, and frees it on all three exits including the
`CreateNewProc` failure.

### Signals, all 18

Allocated and freed on the same task, on every path, with the failure returns
checked one at a time:

* `library.c:357` bring-up, freed at `:378` (`CreateNewProc` failed) and `:384`
* `library.c:242` `sb_EventSignal`, freed at `bsd_child_destroy`; the
  allocation failure at `:243` frees the base first
* `select.c:475` `sb_TimerSignal`, **was defect 1**
* `sana2_rx.c:508` TX reaping, freed at `:582`; the `port == NULL` return at
  `:485` precedes the allocation, so there is nothing to lose
* `netstack_rexx.c:549` stop, freed at `:573`
* `compat.c:161`/`:167`, wrappers; ownership is each caller's
* `tx_initialize_low_level.c:1205` starter, freed on all three exits
  (`:1214`, `:1231`, `:1243`)
* `tx_initialize_low_level.c:1442` stop, freed on every refusal (`:1517`) and
  every completion (`:1623`)
* `tx_thread_schedule.c:198` reaper, freed by `_tx_amiga_reap_cleanup()`,
  called at all three exits
* `tx_amiga_adopt.c:241` adopted run signal, **was defects 6 and 7**
* `clients/dropbear/amiga_dropbear.c:1056-1057`, both freed on every failure
  and on stop
* the test harnesses, `aamprobe.c`, `clockset.c`, `c68k_amissl_bench.c`,
  `mbuf_bpf_test.c`, `tests/perf/bracket_test.c`, `tests/tls/tls_api.c`

`tx_initialize_low_level.c:311` (`_tx_amiga_scheduler_signal`) has no
`FreeSignal`. Clean in the shape this project uses: it is allocated on the
master Task, which `RemTask(NULL)`s itself at `:1173`, and Exec reclaims a Task's
bits with the Task. See "Known and not fixed" for the other supported shape.

### I/O requests

Every `SendIO` in `src/` is reaped before its request's memory or reply port can
go away:

* `select.c:681` timer, `AbortIO`+`WaitIO` on all five exits from
  `WaitSelect()` (`:719`, `:743`, `:766`, `:779`, `:792`), including the two
  early returns
* `sana2_rx.c:184` `CMD_READ`, `AbortIO`, then a bounded reap, then
  `CMD_FLUSH`, then a second reap, then refuse to free anything
* `sana2_tx.c:461` `CMD_WRITE`, `AbortIO` then a bounded reap, and **now** a
  refusal to free the interface if that fails (defect 4)
* the ThreadX port's tick, guard, reap-timeout and stop-timeout requests,
  every one is `CheckIO`/`AbortIO`/`WaitIO`'d before its `CloseDevice`,
  `DeleteIORequest` and `DeleteMsgPort`, in that order, on every branch

The `port`/`tr` and `guard_port`/`guard` pairs in
`tx_initialize_low_level.c:629-807` were walked branch by branch, eight failure
paths, and every one unwinds exactly what it acquired and nothing it did not.
The promotion of the guard to the tick request retires the outstanding probe
first (`_tx_amiga_timer_discard`). No partially-constructed leak there.

### Messages

`ami_rx_drain()` (`netstack_rexx.c:339`) takes the port as a parameter rather
than reading the global, because the closing drain runs after `RemPort()` has
cleared it, that was two Enforcer hits and a sender waiting for a reply that
never came. Every message it dequeues is replied, including one that is not a
`RexxMsg`. The shutdown order is `RemPort()` first, then drain, then
`DeleteMsgPort()`, so nothing new can arrive and nothing already queued is lost.

`tcp_reply()` (`tcp_handler.c:219`) is not `ReplyPkt()`, and says why: `ReplyPkt`
stamps `dp_Port` with the current process's `pr_MsgPort`, and neither handler
process takes DOS packets there.

`bsd_aam_reply()` (`addralloc.c:760`) replies the caller's message from the
worker Process. That is correct, the reply port is the caller's, so `PutMsg`
signals the caller, and the job leaves the table before the reply, because
after it the message is the caller's again and may already have been deleted.

---

## Known and not fixed

* **`ami_sana2_probe_raw()` still posts a stack-local request.** Bounding it
  properly means a heap-allocated probe block that can be leaked when a device
  refuses, because the alternative is returning while the device holds a pointer
  into a dead stack frame, and it cannot use the readers' `tx_thread_sleep()`
  deadline, since `ami_sana2_open()` runs before ThreadX starts
  (`netstack.c:1233`). Left as the opt-in it already is, with the default now
  safe in both places rather than one.
* **`_tx_initialize_low_level()`'s `AllocSignal` leaks in the
  `tx_kernel_enter()` shape.** `port/threadx-amiga/inc/tx_amiga.h:35` documents
  that a standalone program may call `tx_kernel_enter()` from `main()` instead of
  `tx_amiga_kernel_start()`. In that shape the bit is allocated in the
  application's own Task and `_tx_amiga_kernel_task_entry()` never runs, so it is
  never freed and `_tx_amiga_scheduler_task` is never cleared. Nothing in this
  project uses that shape.
* **`_tx_amiga_stop_wait()` and `_tx_amiga_reap()` `Wait()` unbounded when
  `timer.device` will not open.** `portsig` is 0, so the first `Wait()` has no
  timeout, even though the comments around it discuss giving up. A liveness hole,
  not a resource one; recorded because the surrounding prose reads as though it
  were handled.
* **`tx_initialize_low_level.c:1117-1122` is dead code** and would be a
  use-after-free if it were not: the `armed == TX_FALSE` teardown closes and
  deletes with no `WaitIO`. `armed` is forced `TX_TRUE` at `:857-863` before the
  loop and the only place it goes false has no exit.
* **`sb_TimerPort` is a cross-task port waiting to happen.** `mp_SigBit` is a bit
  allocated by the task that called `WaitSelect()`; `mp_SigTask` is
  `base->sb_Task`, the task that called `OpenLibrary()`. They are the same task
  only because base sharing is off, `sb_CanShareBases` is initialised `FALSE`
  and nothing acts on it. If `SBTC_CAN_SHARE_LIBRARY_BASES` is ever honoured,
  `timer.device` signals task A on a bit owned by task B and B's timeout never
  fires. Check this before enabling sharing.
* **`tcp_session_main()` stops draining its port on `ACTION_END`.** The inner
  loop breaks on `!running` without emptying the queue, and `DeleteMsgPort()`
  follows. Nothing sends a packet to a file handle after `Close()`, so it is not
  live; a second task using the same handle would be.
* **`mbuf_bpf_test.c:864` frees its signal without unregistering.**
  `ami_bpf_set_notify_mask()` still holds the task and the mask, so a capture
  after the free signals a bit the task no longer owns. Same task, so no crash.
* **`tx_amiga_adopt_thread():298` discards `_tx_amiga_thread_park()`'s answer**
  and returns `TX_SUCCESS` even when park reports the thread was torn down,
  unlike `tx_amiga_adopt_resume()`, which checks it. Not a resource lifetime bug.

## Not audited

`third_party/` (ThreadX and NetX Duo proper, their `tx_`/`nx_` objects are not
Exec resources), and the Exec resources inside `clients/dropbear` beyond its two
handshake signals.
