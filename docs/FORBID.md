# Forbid regions

Every `Forbid()`/`Permit()` and `Disable()`/`Enable()` region in the tree, what
it guards, and whether anything inside it can block.

**Nothing inside a `Forbid()` may block.** Not `Wait()`, not `DoIO()`/`WaitIO()`,
not `ObtainSemaphore()`, not a dos.library call, not `OpenLibrary`/`OpenDevice`,
not `CreateNewProc()`. Calling one does not fail and does not warn: Exec resumes
multitasking for the duration of the block, restores the calling task's
`tc_TDNestCnt` when it is redispatched, and the code returns believing it was
never interrupted. The critical section was not critical. The damage surfaces
later, somewhere else, as corruption -- commit `4a1ad30` is the shape of it.

`Disable()`/`Enable()` is stricter again: interrupts are off, so nothing that
switches or takes long is allowed at all.

**`grep` needs `LC_ALL=C grep -a`** in this tree -- the NDK headers are Latin-1
and a plain `grep` reads them as binary and silently returns nothing.

## `TX_DISABLE` is `Forbid()`

`port/threadx-amiga/inc/tx_port.h:152-172` maps `TX_DISABLE`/`TX_RESTORE` onto
`_tx_thread_interrupt_disable()`/`_tx_thread_interrupt_restore()`, which are one
inline instruction each on `SysBase->TDNestCnt`
(`port/threadx-amiga/src/tx_thread_interrupt_control.c:75-110`). So **every
critical section in the vendored ThreadX and NetX Duo is a Forbid region**, and
the audit had to cover them too. That is 280 regions in the compiled set on top
of the tree's own.

The port is explicit that a ThreadX thread *may* block inside `TX_DISABLE`
(`tx_thread_system_return.c:20-27`) because Exec preserves the nest count across
`Wait()`. That is true and it is why the machine does not freeze. It is not the
same claim as "the section is still exclusive" -- it is not. Exclusion for
ThreadX state comes from the baton (`src/netstack/netstack_baton.c`), not from
the Forbid.

## Counts

| | Regions | Break the rule |
|---|---|---|
| `src/`, explicit `Forbid()` | 48 | 3 |
| `src/`, `ami_mbuf_lock()` (= `Forbid()`) | 16 | 0 |
| `src/`, `ami_bpf_lock()` (= `Forbid()`) | 19 | 1 |
| `src/`, `Disable()`/`Enable()` | 2 | 0 |
| `port/threadx-amiga/`, explicit `Forbid()` | 21 | 0 |
| Implicit -- Exec-held Forbid across a library vector | 9 | 2 (same file) |
| `clients/`, `Disable()`/`Enable()` | 1 | 0 |
| **Total named** | **116** | **5** |
| Vendored `TX_DISABLE`, compiled set | 280 | 0 |

Four defects fixed, one accepted (see the netmonitor hook). `tests/` and
`tools/smoke/` add 66 more `Forbid()` sites; they are harness code that runs on
a machine already under test and were not audited.

---

## The five that broke the rule

### `bsd_aam_launch` held a Forbid across `CreateNewProc()` -- FIXED

`src/bsdsocket/addralloc.c:814` took a `Forbid()`, claimed
`bsd_aam_jobs[index]`, published the job in the file-scope hand-over slot
`bsd_aam_boot`, called `CreateNewProc()` at `:847`, and only then `Permit()`ed.
The comment above the publish named that Forbid as the reason two
`BeginInterfaceConfig()` calls could not swap each other's jobs.

`CreateNewProc()` inherits the caller's current directory, which is a `DupLock()`
-- a packet to a file system and a wait on the reply. The Forbid was broken for
however long that took, so the invariant the comment asserts never held. Two
calls on **different** interfaces (the `AAMR_Busy` test at `:816` only covers the
same interface) interleave:

1. A blocks inside `CreateNewProc`; multitasking resumes.
2. B runs `bsd_aam_launch` end to end and overwrites `bsd_aam_boot` and
   `bsd_aam_boot_parent`.
3. Worker A takes **job B** out of the slot and signals **task B**, so A's
   `Wait(SIGF_SINGLE)` at `:869` is never satisfied.
4. Worker B finds `job == NULL` and exits at `:625` without replying. Its
   `AddressAllocationMessage` is never `ReplyMsg()`ed, so an application doing
   the documented `BeginInterfaceConfig()` + `WaitPort()` hangs forever -- the
   exact bug the file header says was fixed.

Fixed by deleting the hand-over slot. The job is now a `struct Message`
(`baj_Msg` first) handed to its worker by `PutMsg()` to the worker's own
`pr_MsgPort`, which is what `tcp_ctrl_find()` (`tcp_handler.c:943`) already does
for a `TCP:` session. The `Forbid()` shrinks to the table-slot claim, and the
`SetSignal`/`Wait(SIGF_SINGLE)` handshake goes with it.

`bsd_tcp_handler_start()` (`tcp_handler.c:1143`) is the same pattern done
correctly: it serialises on `master->sb_Lock` and calls `CreateNewProc` outside
any Forbid.

### `ami_bpf_capture` reached `OpenDevice()` through the clock -- FIXED

`src/bpf/bpf_channel.c:432` called `ami_bpf_now()` with the channel lock held.
The comment there says `GetSysTime()` is short and safe under Forbid, which is
true and was not what the function did first: `ami_bpf_now()` called
`ami_millis()` unconditionally, and `ami_millis()` -> `ami_timer_init()`
(`src/common/compat.c:186`) does `OpenDevice("timer.device")` when `TimerBase` is
NULL.

So on a machine where the timer was not yet open, the first captured frame ran an
`OpenDevice()` under Forbid on a SANA-II reader thread -- and because
`ami_timer_init()` caches nothing on failure, once per captured frame for as long
as the open kept failing.

Fixed with `ami_bpf_time_init()`, called from `ami_bpf_open()` before the lock
and on the opener's own Process. `ami_bpf_now()` now reads `TimerBase` and
nothing else, reporting a zero timestamp if there is none -- cosmetic, where
refusing to capture would not be.

### `ug_LibOpen`/`ug_LibClose` waited on the database semaphore -- FIXED

`src/usergroup/ug_library.c` has no `Forbid()` token, which is why it looked
clean. Exec calls the Open/Close/Expunge vectors with the library list already
`Forbid()`ed -- the file's own comment at `:191` says so -- and then `:227` and
`:243` took `ug_Global->lock`.

That lock is the *database* lock: `ug_db.c:289`/`:392` hold it across passwd and
group file reads, and `ug_dos()` (`ug_library.c:140`) holds it across an
`OpenLibrary()`. An `OpenLibrary("usergroup.library")` arriving while another
task was reading `DEVS:passwd` blocked inside Exec's library-list Forbid for the
length of a disk access, with the library list supposedly frozen.

Fixed by giving the children list a `Forbid()` of its own in both vectors, and
changing the one reader outside the vectors -- `ugl_getcredentials()`
(`ug_context.c:209`) -- to take the same `Forbid()`. That walk is a handful of
nodes and a pointer compare; it neither blocks nor allocates.

### `pool_mix` hashed caller-sized material under Forbid -- FIXED

`src/common/ami_random.c:257` ran the whole SHA-256 reseed inside a `Forbid()`,
including `sha256_update(&ctx, data, length)`. `ami_random_add_entropy()` is
public (`include/aminetxduo/random.h:69`) and the documented use is "read a seed
file and feed it", so `length` is whatever the caller has. One compression per
64 bytes at roughly 300-430 us on a 14 MHz 68020 makes 16 KB about 100 ms with
the scheduler off.

Not a broken Forbid -- nothing inside blocks -- but a Forbid-duration bug with no
bound. Fixed by folding the caller's material to 32 bytes *before* the Forbid, so
what runs inside is a fixed 97 bytes: two compressions, the same cost
`random_refill()` already pays. `random_gather()`'s own 440-byte sample gets the
same treatment.

### `TX_DISABLE_NOTIFY_CALLBACKS` was not defined -- FIXED

`tx_amiga_discard_thread()` (`tx_amiga_adopt.c:471`) and
`tx_amiga_orphan_thread()` (`:524`) hold a `Forbid()` across
`_tx_thread_terminate()` with `_tx_thread_system_state` raised. ThreadX's
terminate calls `tx_thread_entry_exit_notify` from inside its own `TX_DISABLE`
(`tx_thread_terminate.c:176`, `:253`). A registered callback that blocked would
break the port's Forbid and the raised-state window at the same time, which is
precisely the `4a1ad30` failure mode.

Nothing in NetX Duo or this stack registers one -- NetX's `*_notify` APIs are its
own, not ThreadX's. `TX_DISABLE_NOTIFY_CALLBACKS` is now defined in `tx_port.h`,
so the call sites are compiled out rather than left latent.

---

## Accepted, not fixed

### `bsd_netmon_dispatch` calls an application hook under Forbid

`src/bsdsocket/netmonitor.c:277` invokes `entry.bme_Fn(hook, NULL, message)`
inside the `Forbid()` at `:270`. The callee is application code. A hook that
calls `Delay`, `OpenLibrary` or `ObtainSemaphore` breaks the Forbid, and the walk
at `:272-283` is then reading `mln_Succ` from a node
`bsd_RemoveNetMonitorHook()` may have unlinked.

The Forbid is what makes the walk safe against removal, so it cannot simply go;
snapshotting the list would mean allocating on the packet path. The autodoc's
"perform its tasks swiftly and without delay" is the contract, and it is a
request rather than something the library can enforce. Recorded as a trust
boundary. The three call sites (`socket.c:1562`, `socket.c:2750`,
`transfer.c:1345`) are outside any NetX bracket, so at worst a bad hook stalls
the machine -- it cannot re-enter ThreadX state we hold.

### The library Open/Close/Expunge vectors block

Exec runs all nine of them (`bsdsocket`, `usergroup`, `tls`) with the library
list `Forbid()`ed. `bsd_lib_open` (`library.c:391`) deliberately breaks it:
`ObtainSemaphore` at `:412`, `ami_netdb_load()` at `:422` (which reads
`DEVS:Internet`), `bsd_netstack_bringup()` at `:428` (`CreateNewProc` + `Wait`),
and `bsd_tcp_handler_start()` at `:464`.

This is what every real `bsdsocket.library` does, and the code is written knowing
it -- the `lib_OpenCnt++` at `:410` is there so the reference is held across the
block, and the comment on the line above says as much. Kept.

`CloseLibrary(DOSBase)` in the two expunge paths (`library_runtime.c:71`,
`ug_library.c:286`) stays too: dos.library's open count never reaches zero on a
running machine, so the call is a Forbid, a decrement and a Permit.

### The unbalanced-by-design exits

Six `Forbid()`s have no `Permit()` and are correct:

| Site | Function |
|---|---|
| `addralloc.c:622`, `:720` | `bsd_aam_worker` |
| `tcp_handler.c:902` | `tcp_session_main` |
| `tcp_handler.c:1116` | `tcp_ctrl_main`, the `ACTION_DIE` arm |
| `netstack_rexx.c:469` | `ami_rx_main` |
| `tx_initialize_low_level.c:450`, `:1169` | the tick task and the master task |
| `tx_thread_system_return.c:52` | `_tx_amiga_task_destroy` |

Each is the last thing the task does before it dies, and the point is that
nothing can expunge the library and `UnLoadSeg` the code these instructions live
in between the final store and the death. Exec discards the nest count of a task
it removes, so the machine does not freeze. Verified there is no code after
each -- in `tcp_ctrl_main` the `Forbid()` at `:1116` is followed only by
`running = FALSE; break;`, which falls out of both loops without re-entering
`WaitPort`.

Residual exposure worth knowing: for the four `CreateNewProc`-made Processes,
dos.library's exit code then runs forbidden. With `NP_Cli = FALSE` and no
`NP_Input`/`NP_Output` there is no `Close()` on that path, so it is frees under
Forbid, not a block.

---

## `src/` -- explicit regions

| Site | Guards | Inside | Verdict |
|---|---|---|---|
| `addralloc.c:704/709` | `bsd_aam_jobs[]` slot + `baj_Done` before the reply | nothing | clean |
| `addralloc.c:811/815,824` | `bsd_aam_jobs[index]` claim | nothing (`CreateNewProc` now outside) | clean -- **was the defect** |
| `addralloc.c:855/858` | rollback after a failed spawn | nothing | clean |
| `addralloc.c:979/985` | `bsd_aam_find` + the `baj_Abort` write | `bsd_aam_find` (table scan) | clean |
| `netmonitor.c:170/183,190` | `bsd_mon_list[]` install | `bsd_mon_setup`, `bsd_mon_installed` | clean, balanced on both exits |
| `netmonitor.c:204/218` | hook removal | `bsd_mon_installed`, `Remove` | clean |
| `netmonitor.c:270/285` | list walk against concurrent removal | **application hook** | accepted, see above |
| `netmonitor.c:298/300` | one list-head read | nothing | clean |
| `tcp_handler.c:926/928`, `:933/935` | `tcp_sessions` | nothing; `CreateNewProc` is correctly outside | clean |
| `compat.c:41/52`, `:69/71` | `ms_Live`/`ms_Refused` | nothing -- `AllocVec`/`FreeVec` are outside | clean |
| `compat.c:86/97`, `:102/107` | `ms_Sockets`, `ms_Opens` | nothing | clean |
| `compat.c:256/282` | the EClock accumulator | `ReadEClock` | clean -- `ami_timer_init`/`OpenDevice` is outside |
| `ami_random.c:275/292` | DRBG reseed of `pool_key` | `sha256_*` over a fixed 97 bytes | clean after the fix |
| `ami_random.c:536/554` | `TaskReady` + `TaskWait` walk | nothing | clean; Forbid is mandatory here, not a choice |
| `ami_random.c:739/757` | one DRBG output block | `random_refill` -> 2 compressions, ~0.6-0.9 ms | clean -- the loop drops the Forbid between blocks |
| `crashguard.c:324/327`, `:337/339` | `SetFunction` on the Alert LVO | `SetFunction` | clean |
| `netstack.c:56/62` | one-shot `InitSemaphore` | `InitSemaphore` | clean |
| `netstack_baton.c:91/93` | the sampler pointer | nothing | clean |
| `netstack_baton.c:114/120`, `:130/133` | publish/withdraw the health mark | `FindSemaphore`, `AddSemaphore`, `RemSemaphore` -- all of which *require* Forbid | clean |
| `netstack_baton.c:208/215,228,240,279` | the slot table and `_tx_thread_system_state` | `tx_thread_suspend` (gated by `TX_THREAD_SYSTEM_RETURN_CHECK`) | clean; `AMI_WARN` is after the Permit |
| `netstack_baton.c:291/296,303,313,329` | the same, on the way back | `tx_thread_resume` | clean |
| `netstack_rexx.c:391/394,402` | `FindPort` + `AddPort` as one step | `FindPort`, `AddPort` | clean; `DeleteMsgPort` and `AMI_WARN` are after the Permit |
| `netstack_rexx.c:446/449` | `RemPort` + clearing the pointer | `RemPort` | clean |
| `netstack_rexx.c:553/557`, `:567/570` | the stopper registration | `FindTask`, `Signal` | clean |
| `netstack_rexx.c:599/602`, `:607/610` | `RemPort`/`AddPort` around an iComp `OpenDevice` | port ops only | clean -- the `OpenDevice` is the caller's, outside |
| `sana2_driver.c:41/50,57` | `ami_sana2_bindings[]` claim | nothing | clean, balanced on both exits |
| `sana2_driver.c:66/76` | binding release | nothing | clean |
| `sana2_tx.c:274/284` | claim a free TX slot | 8-iteration scan | clean |
| `tls_store.c:483/492`, `:605/615` | the 8-slot connection registry | nothing | clean |
| `tool_diag.c:216/228` | `SysBase->DeviceList` walk | `tool_stricmp` | clean; Forbid is mandatory |
| `tool_diag.c:598/612` | `FindPort` + `LibList` walk | `FindPort`, `FindName` | clean |
| `tool_nx.c:476/496` | reading the health mark without owning it | `FindSemaphore`, three struct copies | clean -- `Forbid()` deliberately instead of `ObtainSemaphore()`, so a diagnostic never blocks on the machine it is diagnosing |
| `ug_library.c:235/238`, `:252/254` | the opener-base children list | `AddTail`, `Remove` | clean after the fix |
| `ug_context.c:212/229` | the same list, read side | pointer arithmetic | clean after the fix |

## `src/` -- `Disable()`/`Enable()`

Two pairs, both in `src/sana2/sana2_tx.c`, both correct and both right to be
`Disable()` rather than `Forbid()`.

| Site | Guards | Verdict |
|---|---|---|
| `sana2_tx.c:131/135` | `mp_SigTask`/`mp_SigBit`/`mp_Flags` on the TX reply port | clean -- three stores |
| `sana2_tx.c:148/152` | the same triple, teardown order | clean -- three stores |

A SANA-II device may `ReplyMsg()` from its own interrupt, and Exec's `PutMsg`
reads those three fields as one `Disable()`d unit. `Forbid()` does not stop
interrupts, so it would not be sufficient here. The store order is independently
safe: the bind sets `SigTask`/`SigBit` before `mp_Flags = PA_SIGNAL`, the unbind
sets `PA_IGNORE` first. Note that `mp_SigBit = 0` at `:151` is a valid signal
number, not a sentinel -- it is inert only because `mp_Flags` was cleared on the
line above.

`clients/compat/amiga_libgcc.c:164` is the third `Disable()` in the tree:
`__atomic_exchange_4`, a load and a store. Correct, and it has to be `Disable()`.

## `src/mbuf` -- `ami_mbuf_lock()`

16 regions across `mbuf_alloc.c` and `mbuf_ops.c`. **All clean, all balanced.**
Nothing allocates, frees or blocks inside one: `ami_mbuf_grow()`
(`mbuf_alloc.c:188/190`, `:204/206`) and `ami_mbuf_cluster_get()` drop the lock
across `ami_alloc()` and re-check the ceiling afterwards, which is the right
pattern and is implemented properly.

One fragility to know about: `ami_mbuf_unlock()` is `Permit()`, so the drop at
`:188` only actually re-enables switching if the nest count is exactly 1. It is
today -- `ami_mbuf_grow()` has one caller, which has no path that already holds
the lock. Anyone who calls `ami_mbuf_raw_get()` from inside another mbuf region
turns that `AllocVec()` into an allocation under Forbid, silently.

`ami_mbuf_cluster_of()` is an O(`max_clusters`) walk called from four regions.
Default 16, but `ami_mbuf_init()` takes the ceiling from its caller and bounds it
nowhere.

## `src/bpf` -- `ami_bpf_lock()`

19 regions across `bpf_channel.c` and `bpf_tap.c`. One violation (`ami_bpf_now`,
fixed above); the rest clean and balanced.

`bpf_read()`'s copy-out really is outside the lock: `bpf_channel.c:586` sets
`reading` under the lock, `:588` unlocks, `:591` copies, `:593` re-locks to
commit. The tap's rotate at `:449` checks `reading`, so the buffer cannot be
swapped underneath. The wait loop at `:525-551` also releases before
`ami_bpf_sleep()` -- the single easiest place in the file to get wrong, and it is
right.

The longest region in either directory is `ami_bpf_capture()`, not `bpf_read()`.
`ami_bpf_view_copy()` at `:476` runs inside the lock with `caplen` clamped to
`ch->blen - AMI_BPF_HDRLEN`, and `blen` is client-settable through `BIOCSBLEN` up
to `BPF_MAXBUFSIZE` (32768). The MTU-bounded taps cannot supply that much, but
`netstack_capture.c` builds a view over a whole `NX_PACKET` chain, so a large
loopback datagram reaches the clamp. Add the filter run at `:422` (up to 512
interpreted instructions) and that is tens of milliseconds with switching off on
a 68000. The design does limit the blast radius: the lock is taken inside the
per-channel loop, so four channels give four brackets with a scheduling point
between them, not one.

## `port/threadx-amiga/` -- 21 regions, all clean

No `ObtainSemaphore`, no `OpenLibrary`, no `CreateNewProc`, no dos.library call,
no stdio and no allocation inside any of them. Everything blocking in the port --
`CreateMsgPort`, `OpenDevice`, `Wait`, `WaitIO`, `CloseDevice`, `AllocMem` -- sits
at nest 0. Three regions are worth naming:

**`tx_thread_schedule.c:63-99` drops the Forbid before waiting.** `Permit()` at
`:70`, `Wait()` at `:71`, `Forbid()` at `:72`, with the dispatch condition
re-evaluated under the Forbid on every iteration -- a correct condition-variable
idiom, not a lost wakeup (Exec signals latch). It rests on an unstated invariant:
`_tx_thread_schedule()` is entered at nest 0, so that `Permit()` really reaches
zero. Anything that ever called it with a Forbid held would turn `:71` and `:99`
into silent Forbid-breakers.

**`tx_thread_context_save.c:45` opens a Forbid that
`tx_thread_context_restore.c:63` closes.** There is exactly one call site pair --
`tx_initialize_low_level.c:1058-1060`, three consecutive statements -- and the
only early exit in that loop body is above line 1058. Correct today, and
structurally fragile: a `return` or `break` inserted between those two lines
leaks one Forbid level onto the tick task permanently, and because Exec restores
the nest count across `Wait()` there would be no diagnostic at all.

**`tx_timer_interrupt.c:41-89` does not call application code.**
`TX_TIMER_PROCESS_IN_ISR` is not defined, so `_tx_timer_expiration_process()`
compiles to a ready-list insertion and the application's `timeout_function` runs
on `_tx_timer_thread` at thread level, which is what `tx_port.h:98-101` claims.
The branch that *would* call it under Forbid is dead code in this build.

The `_tx_thread_system_state` windows in `tx_amiga_adopt.c` (five regions,
`:253`, `:329`, `:414`, `:471`, `:524`) are the `4a1ad30` shape and are correct:
the raise happens after the validation `Permit()`s and the lower before the exit
`Permit()`, on every path. Every route from a raised state into the ThreadX core
reaches `_tx_thread_system_return()` only through
`TX_THREAD_SYSTEM_RETURN_CHECK`, which ORs in exactly that counter -- checked at
all 6 gates in `tx_thread_system_suspend.c`, all 4 in
`tx_thread_system_resume.c`, and the one in `tx_thread_system_preempt_check.c`.

Two latent hazards, neither reachable:

- `tx_thread_stack_build.c:88` would call `_tx_amiga_task_create()` -- two
  `AllocMem`s and an `AddTask` -- under the Forbid at `tx_amiga_adopt.c:253` if
  `_tx_amiga_adopt_task` were ever 0 there. It is set at `:258` inside the same
  Forbid, so the fall-through is unreachable; the handshake is process-wide
  globals, which is why it is worth writing down.
- `_tx_amiga_reap()` does `AllocSignal`/`CreateMsgPort`/`OpenDevice`/`Wait`
  (up to 2 s)/`WaitIO`/`CloseDevice`. It is called at Forbid nest 1 from
  `tx_amiga_discard_thread:496` and `tx_amiga_orphan_thread:566`, and is safe
  there only because both act on adopted threads and `_tx_amiga_reap()`
  early-returns on `TX_AMIGA_THREAD_ADOPTED` (`tx_thread_schedule.c:191`). That
  flag is set in `tx_thread_stack_build.c:72` and never cleared. If it ever were,
  those two paths would do a device open and a two-second `Wait()` under Forbid.

## Vendored ThreadX and NetX Duo -- 280 compiled regions, no hits

The question that mattered was whether NetX Duo ever calls the link driver from
inside a `TX_DISABLE`, because this project's driver
(`src/sana2/sana2_device.c:131`) does `DoIO()` -- it blocks. It does not. All 58
`link_driver_entry(&driver_request)` sites in the compiled set are outside any
region, and no function in the 324-function transitive closure that reaches a
driver entry is called from inside one.

Three sites looked like hits under a deliberately over-approximating region model
and were cleared by hand: `nx_ip_driver_packet_send.c:528` (both arms of the
preceding `if/else` restore and fall into a `return`), and two in
`addons/BSD/nxd_bsd.c`, which is not compiled.

Every application callback in the compiled set is invoked **after** a
`TX_RESTORE` -- verified individually for `nx_udp_packet_receive.c:543`,
`tx_timer_expiration_process.c:347`, `tx_event_flags_set.c:597`, the three
`tx_queue_front_send.c` sites, both `tx_semaphore_ceiling_put.c` sites, and both
`tx_thread_shell_entry.c` sites. No TCP connect/disconnect/receive callback
appears in any region. No allocation, `printf` or file I/O inside any region.

Two callbacks *are* inside `TX_DISABLE` in the vendored tree and both are dead
code here: `tx_thread_stack_error_handler.c:80` (needs `TX_ENABLE_STACK_CHECKING`,
which `tx_port.h:112-118` rules out) and the `TX_NOT_INTERRUPTABLE` arms of
`tx_thread_shell_entry.c:144` / `tx_thread_terminate.c:143`. **Remember the first
one if anyone ever turns stack checking on.**

Two genuine non-blocking callouts: `nx_ip_driver_link_status_event.c:74` and
`nxd_dhcp_client.c:8072` call `tx_event_flags_set()` inside a Forbid. Neither
blocks, but note the second-order effect -- `_tx_event_flags_set` takes its own
`TX_DISABLE`, so its `TX_RESTORE` at `tx_event_flags_set.c:588` drops only one
level and the notify callback at `:597` still runs inside the outer caller's
Forbid. The first of the two is unreachable from `src/`
(`_nx_ip_driver_deferred_processing()`, which `sana2_tx.c:180` calls, has no
`TX_DISABLE` at all).

The baton hook is worth being precise about: `ami_netstack_baton_release()`
releases the ThreadX baton and nothing else. It does not unwind an outstanding
`TX_DISABLE` nesting. If a driver entry ever did land inside a region, the hook
would not save it -- the `DoIO`'s `Wait()` would still happen with `TDNestCnt`
raised. The safety here rests on there being no such call site, not on the hook.

---

## Not traced

Recorded rather than guessed at:

- **`tests/` and `tools/smoke/`** -- 66 further `Forbid()` sites (the bulk in
  `tests/soak/soak_test.c` and `tests/tcpdrill/tapdev.c`). Harness code on a
  machine already under test; not audited.
- **PPPoE** -- `nx_pppoe_client.c:1625/1634/1639` and
  `nx_pppoe_server.c:1867/1876/1881` were flagged by the over-approximating pass
  and not hand-verified, because neither file is in `NETXDUO_ADDON_SOURCES`.
  Re-check them if PPPoE is ever added.
- **`nx_link_*()` / `nx_ip_driver_direct_command()` callers** -- verified that no
  NetX caller holds a `TX_DISABLE` across them. Not exhaustively verified that no
  `clients/` program holds a hand-written `Forbid()` across one; `src/` and
  `port/` contain none.
- **The vendored region model is textual plus brace depth, not a CFG.** It
  over-approximates, which is the safe direction, but a `goto` jumping into the
  middle of a region would defeat it. No `goto` appears in any compiled-set
  region.
- **Third-party SANA-II drivers.** `ami_sana2_tx_reap_bind()` assumes a device
  may `ReplyMsg()` from its interrupt and is written for it. What a given driver
  does inside `OpenDevice`, `CMD_WRITE` or `S2_ONLINE` is outside this tree and
  outside this audit.
