# Reentrancy

Every writable file-scope object in `src/bsdsocket`, `src/netstack`, `src/config`,
`src/sana2`, `src/bpf`, `src/common`, `src/tls`, `src/tlslib` and
`src/usergroup`, and what makes it safe.

An AmigaOS shared library is **one code image with one data segment**, opened by
many programs at once. There is no per-process copy, no copy-on-write and no
address space separation, so every file-scope `static` is shared by every opener
simultaneously. Two programs using the stack at the same time see the same
bytes.

The mechanism for per-opener state is the **child library base**:
`bsd_lib_open()` (`src/bsdsocket/library.c:391`) hands each opener its own
`struct AmiSocketBase` and `bsdsocket_internal.h` carries sixty-odd `sb_*`
fields on it. `usergroup.library` does the same (`ug_library.c:184`). Anything
belonging to one program belongs there. **`tls.library` deliberately does not**
-- `tls_library.c:8` -- so every static in `src/tls` and `src/tlslib` is
machine-wide whether or not that is semantically right.

**`grep` needs `LC_ALL=C grep -a`** in this tree: the NDK headers are Latin-1 and
a plain `grep` reads them as binary and silently returns nothing.

## The census

Counted against `origin/main`, over the seventy-five shipping `.c` files in the
nine directories (`src/*/test/` excluded). The tables below are the authority --
one row per object, or per named group -- and these totals are read off them:

| | |
|---|---|
| writable file-scope `static` | 102 |
| writable function-local `static` | 1 (`socket.c:196`) |
| writable non-`static` file-scope global | 19 |
| **writable, total** | **122** |
| read-only file-scope `static const` | 38 |
| read-only `const` file-scope global | 10 |
| function-local `static const` | 11 |

And the verdicts on the 122:

| Verdict | Count | Fixed here |
|---|---|---|
| 1 -- singleton, correctly guarded | 58 | -- |
| 2 -- constant | 23 | -- |
| 3 -- misplaced | 21 | 2 |
| 4 -- unguarded | 19 | 17 |
| could not settle | 1 | -- |

Two more were touched without changing class: `bsd_raw_list` and
`bsd_raw_installed` are correctly guarded singletons that had one leak path.
So twenty-one objects across eleven sites, and one new static
(`pool_gathering`).

Twelve of the twenty-one misplaced are crashguard, which is not linked into any
shared library; five more are unreachable through any LVO. That leaves four live
ones, and the serious one is `_nx_secure_tls_record_block_buffer`.

---

## The four verdicts

1. **Singleton** -- one per machine, and a shared static is right. The `NX_IP`,
   the packet pool, the interface table, the SANA-II readers.
2. **Constant** -- never written. Several cannot be `const` because an Exec or
   NetX Duo struct field is typed `char *` or non-const; those are named below.
3. **Misplaced** -- per-opener or per-connection state at file scope. The bug
   class.
4. **Unguarded** -- sharing is right, but two openers on two tasks can collide.

## The three locks that do the work

- **The ThreadX baton.** `bsd_nx_enter()` (`netx_call.c`) adopts the calling Task
  as a ThreadX thread, and `port/threadx-amiga/src/tx_amiga_adopt.c:35` is
  explicit that while it is adopted no other ThreadX thread runs -- including the
  NetX Duo IP thread and every other base. It is a machine-wide mutex, which is
  why `bsd_raw_list`, `bsd_closing_head` and the multicast tables need no lock of
  their own. **It is released at every ThreadX suspension point**, so a static
  that stays live across `nx_packet_allocate(..., wait)` or
  `nx_tcp_socket_receive()` is *not* covered. That distinction is the whole of
  `_nx_secure_tls_record_block_buffer` below.
- **`Forbid()`.** Used where blocking is not allowed: `src/bpf`, the netmonitor
  registry, the address-allocation table, the baton slots, the CSPRNG.
- **Semaphores.** `master->sb_Lock` serialises `bsd_lib_open`/`bsd_lib_close` and
  everything the first open does; `ami_ns_lock` (`netstack.c:49`) serialises
  bring-up and teardown; `TLSBase->tb_Lock` serialises tls.library setup.

## What survives a last close

`bsd_runtime_open()` runs from `InitResident()`, i.e. on the **first**
`OpenLibrary()` only. `bsd_lib_open()` initialises the child base and nothing
else. `bsd_lib_expunge()` is declined while the TCP: handler, an address
allocation or a monitor hook is live (`library.c:539/552/565`).

**So a static is re-initialised only when the segment is reloaded.** Everything
below keeps its value across any number of last-close-without-expunge cycles, and
a value one program left is what the next unrelated program sees.
`tests/tools/run-cycledrill.sh` exercises exactly that cycle -- from one task, so
it proves the persistence half and not the concurrency half.

Two statics were found to be wrong on precisely this point, `tcp_started` and
`ami_bpf_iface[]`; both are fixed below.

---

## src/bsdsocket

| Object | Verdict | Guard |
|---|---|---|
| `library.c:29` `SysBase` | singleton | written once from `bsd_lib_init()`, under `InitResident` |
| `library.c:38` `bsd_lib_name` | constant | `struct Resident.rt_Name` is `char *`, so it cannot be `const` |
| `library.c:39` `bsd_lib_id` | constant | as above, `rt_IdString` |
| `library.c:330` `bsd_net_boot` | singleton | `master->sb_Lock`, held across the whole of `bsd_netstack_bringup()` |
| `library_runtime.c:33` `DOSBase` | singleton | opened at `InitResident`, closed at expunge |
| `netmonitor.c:77` `bsd_mon_list[7]` | singleton | `Forbid()` on install, remove and dispatch. See the note below |
| `netmonitor.c:78` `bsd_mon_ready` | singleton | `Forbid()`; `bsd_mon_setup()` is called with it held |
| `netmonitor.c:79` `bsd_mon_count` | singleton | `Forbid()` |
| `addralloc.c:508` `bsd_aam_jobs[]` | singleton | `Forbid()` on claim and on release; one row per interface |
| `addralloc.c:509` `bsd_aam_workers` | singleton | `Forbid()` |
| `addralloc.c:514` `bsd_aam_boot` | **unguarded -- FIXED** | see below |
| `addralloc.c:515` `bsd_aam_boot_parent` | **misplaced -- FIXED** | moved into `BsdAamJob` |
| `netdb.c:47` `bsd_no_aliases` | constant | never written; `char *[]` because the BSD ABI says `char **` |
| `oob.c:122` `bsd_oob_mark` | singleton | the baton, with a documented degradation: a second OOB send that finds it armed sends its byte unmarked rather than racing |
| `raw.c:110` `bsd_raw_list` | singleton | `ip->nx_ip_protection`, a recursive ThreadX mutex the IP thread already holds for its whole event loop. **One leak path fixed** |
| `raw.c:111` `bsd_raw_installed` | singleton | as above |
| `socket.c:58` `bsd_tcp_name` | constant | `nx_tcp_socket_create()` takes `CHAR *` |
| `socket.c:59` `bsd_udp_name` | constant | as above |
| `socket.c:196` `last_budget` | singleton | function-local; a log de-duplicator, and a torn read costs one repeated line |
| `socket.c:580` `bsd_closing_head` | singleton | the baton. Drained on the last close by `bsd_closing_drain()` (`docs/ALLOCATIONS.md`) |
| `mcast.c:71` `bsd_mcast_table[16]` | **unguarded -- FIXED** | see below |
| `mcast.c:499` `bsd_mcast6_table[16]` | **unguarded -- FIXED** | see below |
| `tcp_handler.c:122` `tcp_ctrl_port` | singleton | written only by the handler Process; `bsd_tcp_handler_alive()` reads it to decline the expunge |
| `tcp_handler.c:123` `tcp_node` | singleton | as above |
| `tcp_handler.c:124` `tcp_boot` | singleton | the `tcp_started` latch is tested and set under `master->sb_Lock`, so only one launcher at a time reaches this and it is cleared before that launcher returns |
| `tcp_handler.c:125` `tcp_sessions` | singleton | `Forbid()` |
| `tcp_handler.c:126` `tcp_started` | **unguarded -- FIXED** | see below |

### The multicast tables were the right shape and the wrong window (FIXED)

Sixteen rows shared by every opener, and that is correct: membership in NetX Duo
is per `NX_IP`, refcounted and capped, so the socket-to-group mapping it does not
keep has to be kept somewhere machine-wide. A row is keyed by `AmiSocket *`, and
sockets are per-base, so the ownership is right.

The window was not. `bsd_mcast_join()` picked a free row *inside* the
`bsd_nx_enter()` bracket and only wrote `bm_Sock` **after** `bsd_nx_leave()`. A
row is free precisely because `bm_Sock` is NULL, so a second base joining in that
window took the same row. The loser's membership then existed in NetX Duo and
nowhere else: `bsd_mcast_close()` never found it, so it was never left on close,
and the winner's close dropped a group it did not own. `bsd_mcast6_join()` had it
identically.

Both now claim the row inside the bracket and give it back if the join fails.

`tests/tools/run-mcastrace.sh` is the test. One process cannot reach this -- a
base serialises itself on its own nesting counter -- so `McastRace` runs two,
with a base each. The window is a handful of instructions and is aimed at rather
than waited for: `bsd_nx_leave()` pokes the ThreadX scheduler Task, which runs at
Exec priority 1 and preempts a priority-0 caller *at that instruction*, so a
second Process parked on the baton is dispatched inside the window. The assertion
is the only thing the loser can see, which is its own `IP_DROP_MEMBERSHIP` refused
with `EADDRNOTAVAIL` on a group it is holding. Moving the three stores back below
`bsd_nx_leave()` takes it from 0 lost memberships in 200 to 63.

Receiving a datagram is deliberately *not* the assertion. NetX Duo refcounts
membership per `NX_IP`, so two joins for one group leave the count at 2 and one
spurious leave still leaves the group live for the other opener. What the defect
destroys is this library's own socket-to-group mapping.

`bsd_mcast_close()` walks the tables with no bracket of its own; that is fine.
Its only caller is `bsd_socket_destroy()` (`socket.c:856`), and every path into
that -- `bsd_socket_release`, `bsd_close_all`, `bsd_closing_sweep`,
`bsd_closing_drain`, the listen-spare teardown -- is already inside one.

### `tcp_started` could not be cleared, so TCP: could not come back (FIXED)

`tcp_started` was set once and never cleared. `ACTION_DIE` is accepted with
openers still holding the library (it only requires `tcp_sessions == 0`), and
the expunge that would reload the segment and reset the static runs only after
the last close. Between the two, every `OpenLibrary()` reached
`bsd_tcp_handler_start()`, found the latch set, and returned -- a library with
no TCP: device and no way to get one.

Cleared now on `ACTION_DIE` inside the existing `Forbid()`, so no opener can see
`tcp_started` and `tcp_ctrl_port` disagree; and on the two launch failures
(`tcp_ctrl_publish()` returning FALSE, `CreateNewProc()` returning NULL), which
left the same latch set with no handler behind it.

### The address-allocation hand-over slot had a Permit()-sized hole (FIXED)

`bsd_aam_boot` and `bsd_aam_boot_parent` were claimed under `Forbid()`, but the
launcher has to `Permit()` before it can `Wait()`, and the worker collects the
slot after that. A second `BeginInterfaceConfig()` -- legal, on another
interface -- could enter that window, take the `Forbid()`, and overwrite both.
The first launcher then waited for a `SIGF_SINGLE` its worker had sent to the
second launcher's task.

The parent task moves into `BsdAamJob`, where it is per-request and cannot be
overwritten by anyone. `bsd_aam_boot` is now claimed under the same `Forbid()`
as the interface row, and a launch that finds it taken is answered `AAMR_Busy`
-- an error the API already defines. The window it can be refused in is bounded
by the worker's first instructions, which do nothing that can block.

The per-interface half of that is tested. `AamProbe` forks a Process with a
bsdsocket.library base of its own and has it ask for the same interface while the
first allocation is in flight -- the case `AAMR_Busy` exists for, and one no
single caller can reach, since the caller holding the job is the one that would
ask again. `tests/tools/run-ifquery.sh` asserts it.

The result code alone would not have caught it. `AAMR_Busy` is answered twice
over: at the door by `bsd_aam_jobs[index]`, and -- if that guard is gone and the
worker starts anyway -- by `netstack_interface_dhcp_start()` refusing a second
DHCP client on one interface, which `bsd_aam_worker()` also reports as
`AAMR_Busy`. Deleting the guard still produces an 11, measured. What changes is
*when*: a refusal is replied inside `BeginInterfaceConfig()`, the other answer
costs a `CreateNewProc()` and a worker's first DHCP call and arrived ten ticks
later. So the probe asserts the message beat the call's return.

### `bsd_raw_close()` with the stack already down (FIXED)

The registry is a file-scope static and outlives any `NX_IP`, but the
`ip == NULL` branch skipped the unlink entirely: the `AmiSocket` was then freed
and left on `bsd_raw_list`, and `bsd_raw_installed` never returned to zero, which
is the filter never being installed again. Unlinked under `Forbid()` now, since
`nx_ip_protection` lives inside the `NX_IP` that has gone.

### The netmonitor registry -- shared by contract, not fixed

`bsd_mon_list[]` holds `struct Hook *` from any opener, and nothing removes a
base's hooks when it closes. That is the documented contract, not an oversight:
"It must be called before the library is closed, or the library will stay in
memory indefinitely." `bsd_netmon_busy()` declines the expunge for exactly that
reason. The `MinNode` is embedded in the caller's own `Hook`, so there is nowhere
to record an owner without a side table, and a program that exits without
removing leaves a dangling hook the library will keep calling. Recorded, matching
the API.

---

## src/config

| Object | Verdict | Guard |
|---|---|---|
| `config_text.c:14` `ami_cfg_empty` | constant | never written here, but handed out as a mutable `char *` at `:393`. Should be `const`; the ABI of `ami_cfg_next_pair()` is what stops it |
| `config_text.c:101` `ami_cfg_reporter` | **misplaced** | per-caller, and dormant. See below |
| `config_text.c:102` `ami_cfg_reporter_user` | **misplaced** | as above |
| `config_text.c:103` `ami_cfg_current_file` | unguarded | per-parse state; live in the library, serialised only by `master->sb_Lock` around the first open. See below |
| `netdb.c:48` `ami_netdb[4]` | singleton | loaded once from `bsd_lib_open()` under `master->sb_Lock`, read-only afterwards; freed by `bsd_lib_expunge()` |
| `netdb.c:49` `ami_netdb_loaded` | singleton | as above |

`ami_config_set_reporter()` is a per-program callback plus a per-program `user`
pointer, in two file-scope statics written as two separate unguarded stores. It
is **not reachable inside the shipped library**: nothing in `src/bsdsocket`,
`src/netstack` or `src/config` calls it, and the two real installers
(`tool_diag.c:425`, `checknetconfig.c:915`) are separate executables with private
copies. It is one call from being a bug -- the moment any in-library code sets
it, two openers clobber each other's callback and can pair one program's
function with another's `user` pointer. Recorded, not moved: moving it means
inventing an owner it does not have.

`ami_cfg_current_file` *is* live in the library -- `ami_config_load()` writes it
ten times from `config_file.c` -- but every reachable caller sits inside
`bsd_lib_open()`'s `master->sb_Lock`, so two loads cannot interleave. It also
holds a pointer to a stack-local `path[]` between `config_file.c:143` and `:145`;
the code comments that and clears it, and there is no return between the two.

The netdb was the subject of two separate defects on the same day
(`docs/ALLOCATIONS.md`) and neither was a reentrancy defect: both were leaks.
The concurrency is sound. `ami_netdb_load()` sets `ami_netdb_loaded` *before* it
parses, which would let a second caller read half-built tables -- but the only
caller inside the library holds `master->sb_Lock`, and the lazy path in
`netdb_table()` cannot fire because `bsd_lib_open()` has already loaded.

---

## src/netstack

| Object | Verdict | Guard |
|---|---|---|
| `netstack.c:49` `ami_ns_lock` | singleton | `InitSemaphore` under `Forbid()`; every obtain is preceded by `ami_ns_lock_init()` |
| `netstack.c:50` `ami_ns_lock_ready` | singleton | the test and the set are both inside one `Forbid()`, and `InitSemaphore()` does not `Wait()`, so the lazy init is **not** racy |
| `netstack.c:51` `ami_ns` | unguarded | writes under `ami_ns_lock`, reads not. See below |
| `netstack.c:52` `ami_ns_system_initialised` | singleton | written under `ami_ns_lock`. Deliberately persists: it is the "has NetX Duo's global init run in this segment" latch |
| `netstack_baton.c:68` `ami_baton_slot[16]` | singleton | `Forbid()` on every touch. Keyed by `struct Task *`, which is the right key. Slot leak below |
| `netstack_baton.c:72` `ami_baton_stats` | singleton | writes under the callers' `Forbid()`; the seven-field snapshot in `netstatus.c:845` is read without one, so it can be internally inconsistent. Diagnostics only |
| `netstack_baton.c:82` `ami_health_mark` | singleton | field writes are outside the `Forbid()` that covers `FindSemaphore`+`AddSemaphore`, but the only caller is `netstack.c:1352` under `ami_ns_lock` |
| `netstack_baton.c:83` `ami_health_name` | constant | never written; `char[]` because `ln_Name` is `STRPTR` and it goes on Exec's public semaphore list |
| `netstack_baton.c:84` `ami_health_up` | singleton | writes under `Forbid()`, early-out reads under `ami_ns_lock` |
| `netstack_baton.c:87` `ami_baton_sampler` | singleton | write under `Forbid()`; cleared at shutdown before the stack goes |
| `netstack_rexx.c:51` `ami_rx_rexxbase` | singleton | task-confined: only the host Process ever touches it |
| `netstack_rexx.c:55` `ami_rx_port_name` | constant | never written; `char[]` because it goes on Exec's public port list |
| `netstack_rexx.c:92` `ami_rx_port` | **unguarded -- FIXED** | `AddPort()` and the record are one step now |
| `netstack_rexx.c:93` `ami_rx_proc` | singleton | unguarded locally; serialised by `ami_ns_lock` at both call sites |
| `netstack_rexx.c:94` `ami_rx_boot` | singleton | as above; the pointer targets the starter's stack and both early returns precede the Process existing |
| `netstack_rexx.c:113` `ami_rx_gone` | singleton | `volatile`, one-shot, set under a deliberately unmatched `Forbid()` in the exiting process |
| `netstack_rexx.c:114` `ami_rx_stopper` | singleton | `Forbid()`, taken together with the `Signal()` so the host cannot finish and find nobody |
| `netstack_rexx.c:115` `ami_rx_stop_sig` | singleton | as above |

### `ami_ns` -- readers versus teardown, recorded and not fixed

Every write to `ami_ns` holds `ami_ns_lock`; not one of the eighteen reads does.
A pointer load is atomic on m68k, so there is no torn value -- but there is no
reference on the read path either. `netstack_shutdown()` NULLs it at
`netstack.c:1428` and `ami_ns_destroy()` frees it at `:1445`, so a reader that
loaded the pointer a moment before uses freed memory. `netstack_ip()`,
`netstack_pool()` and every `bsd_*` caller that starts with one of them are on
that path.

Not fixed here. It needs a reference count on the read side, which means every
`netstack_ip()` caller acquires and releases, and that is a different change from
an audit. The exposure is bounded in practice: `netstack_shutdown()` runs only
when `master->sb_StackRefs` reaches zero, which means no base is open, which
means no application task is inside a vector.

### The baton slots leak on a task that dies mid-bracket

A slot is claimed by `ami_netstack_baton_release()` and freed by the matching
`ami_netstack_baton_acquire()`. Nothing sweeps. A Task that dies between the two
holds its slot for ever, and there are sixteen. Exhaustion is handled honestly
(`bs_Full++` and a warning at `netstack_baton.c:239`) but the consequence is that
threads then block *holding* the baton, which is the stall this file exists to
prevent. Worse, Exec recycles `struct Task` addresses, so a later Task at the
same address inherits a slot with a stale `TX_THREAD` and a non-zero nesting
count.

Recorded, not fixed: a reaper needs a liveness test for a `struct Task *` that
Exec does not offer cheaply, and getting it wrong is worse than the leak.

---

## src/sana2

| Object | Verdict | Guard |
|---|---|---|
| `sana2_device.c:22` `ami_raw_allowed` | **unsettled** | see below |
| `sana2_device.c:31` `ami_block_enter` | singleton | no local guard; installed before `tx_amiga_kernel_start()` and cleared after `ami_ns_destroy()`, both under `ami_ns_lock`. Safe by ordering |
| `sana2_device.c:32` `ami_block_leave` | singleton | as above |
| `sana2_device.c:280` `ami_raw_probe_slot` | **misplaced -- FIXED** | now a stack local in `ami_sana2_probe_raw()` |
| `sana2_driver.c:32` `ami_sana2_bindings[]` | singleton | `Forbid()` on attach and unbind; `ami_sana2_lookup()` reads it lock-free from the IP thread |

`ami_raw_probe_slot` was a file-scope `AmiRxSlot` handed to a driver as
`ios2_Data` for one probe that begins and ends inside one function. It was safe
only by absence -- the copy hook refuses anything aimed at it because
`packet == NULL`, and the two callers happen to sit under `ami_ns_lock`. Neither
is a property of the slot. A driver that writes `ios2_Data` directly instead of
calling the copy hooks (documented iComp behaviour, `netstack_rexx.c:578`) writes
into shared library data. On the stack it is per-probe and the question does not
arise.

`ami_sana2_lookup()`'s lock-free read is correct **by store ordering, not by a
lock**: attach writes `ip`, `index`, then `iface` last, and the reader tests
`iface` first; unbind clears `iface` first. Both orders are right and m68k word
stores are atomic. Left alone, and flagged because a reordering of those three
lines would break it silently.

**Could not settle: `ami_raw_allowed`.** `ami_sana2_set_raw_allowed()` is public
API (`include/aminetxduo/sana2.h:168`) with no caller anywhere in the tree, so
today it is a compile-time default and the missing guard is unreachable. Whether
it is meant as machine policy or per-opener policy is genuinely undetermined by
the code: framing is a property of the shared device, which argues machine-wide;
the API shape lets any one opener change what interfaces a *different* opener
later brings up, which argues otherwise. Left as it is rather than guessing.

---

## src/bpf

| Object | Verdict | Guard |
|---|---|---|
| `bpf_channel.c:40` `ami_bpf_chan[]` | singleton | `Forbid()`. Owner is the **child** base (`src/bsdsocket/bpf.c:84`), and `bsd_child_destroy()` closes them, so channels die with their opener |
| `bpf_channel.c:41` `ami_bpf_bound_channels` | singleton | `Forbid()` on every update, decrements predicated on `> 0`; read lock-free as a fast gate and re-validated per channel under the lock |
| `bpf_tap.c:23` `ami_bpf_iface[]` | **unguarded -- FIXED** | see below |
| `bpf_tap.c:70` `ami_bpf_addr_hook` | singleton | no local guard; installed and cleared under `ami_ns_lock`, cleared before the interfaces are detached |

`ami_bpf_init()` zeroed the channel table and not the interface table, although
both are file-scope and both survive a stack teardown. A detach the last teardown
missed left a row with `used` set and a cookie into the `AmiSana2If` that went
away with the old stack, which the next bring-up's `ami_bpf_iface_by_cookie()`
would hand to an injector. Both are zeroed now.

The channel keying is correct and worth stating, because it is the one place in
the tree that gets per-opener ownership of a machine-wide table right:
`bsd_bpf_close_all()` passes the child `SocketBase` as the owner, and
`bsd_child_destroy()` calls it, so a program's capture channels go when the
program does. What remains is a shared-resource limit, not a correctness
problem: one program can exhaust the pool and every other opener gets `EBUSY`.

---

## src/common

| Object | Verdict | Guard |
|---|---|---|
| `compat.c:30` `ami_mem` | singleton | `Forbid()` inside compat.c. `netstack.c:1436` and `netstack_pool_sample()` write the same struct through `ami_mem_stats()` **without** it; individual counters are atomic, the snapshot is not |
| `compat.c:175` `TimerBase` | **unguarded -- FIXED** | see below |
| `compat.c:177` `ami_timer_req` | **unguarded -- FIXED** | as above. Never `DoIO`'d -- `ReadEClock()` bypasses it -- so the only exposure was the double open |
| `compat.c:178` `ami_timer_port` | **unguarded -- FIXED** | as above. `mp_SigTask` records whichever task opened first, and is inert only because the port is `PA_IGNORE` and nothing is ever queued |
| `compat.c:179` `ami_eclock_hz` | **unguarded -- FIXED** | init only; steady-state reads are under the `ami_millis()` `Forbid()` |
| `compat.c:181-184` `ami_eclock_last/ms/rem/carry` | singleton | the read-modify-write in `ami_millis()` was already fully bracketed by `Forbid()`. Only the init was open, and that is now closed |
| `compat.c:296` `ami_sana2_quiesce` | singleton | no local guard; set and cleared under `ami_ns_lock` at `netstack.c:1353`/`:1432` |
| `compat.c:297` `ami_sana2_restore` | singleton | as above |
| `ami_random.c:239-243` `pool_key`, `pool_out`, `pool_out_used`, `pool_counter`, `pool_bits` | singleton | `Forbid()` per block, correctly. One CSPRNG per machine is the right design and this is the cleanest guard discipline in the tree |
| `ami_random.c:244` `pool_started` | **unguarded -- FIXED** | see below |
| `ami_random.c` `pool_gathering` | singleton | new; the latch that closes the above |
| `ami_random.c:579` `random_sample` | **unguarded -- FIXED** | 800 bytes at file scope to keep it off a small stack, which is what created the sharing. Closed by the same latch |
| `crashguard.c:33-38, 41-45, 253` | **misplaced -- not fixed** | see below |

### The two timer opens were the same bug twice (FIXED)

`ami_timer_init()` (`compat.c`) and `ami_tls_timer_open()` (`tls_amiga.c`) were
both `if (base != NULL) return TRUE;` followed by an unprotected open. Two tasks
reaching either for the first time together both fell through and both
`OpenDevice()`d the same static `struct timerequest` -- Exec's "reuse of an
active IORequest" -- and in compat.c the second re-zeroed `ami_eclock_ms` under
the first, which a caller computing `ami_millis() - start` sees as an unsigned
wrap of about 49 days.

Both re-test under `Forbid()` now. `timer.device`'s Open is a table lookup and
does not `Wait()`, so the Forbid holds across it, and no other task can observe
the half-built state inside it. `TimerBase` is still assigned immediately after
`OpenDevice()` and before `ReadEClock()`, which is a `proto/timer.h` inline and
calls through that base -- publishing it later looks tidier and hangs the first
`OpenLibrary()`, which is what `run-cycledrill.sh` caught.

`ami_millis()` is not a theoretical concurrent caller: it is reached from the
SANA-II receive path through `ami_bpf_now()` (`bpf_channel.c:432`), from
`ami_random.c`, and from any opener's task.

### The CSPRNG collection could run twice at once (FIXED)

`ami_random_add_entropy()` and `ami_random_bytes()` both do
`if (!pool_started) ami_random_init();`, and `ami_random_init()` set
`pool_started` *before* the 22 ms `random_gather()`. Two tasks could both pass
the test and both run the gather, which writes the 800-byte file-scope
`random_sample` from end to end -- two machine fingerprints interleaved into one
buffer.

One collection at a time now, latched under `Forbid()`. Repeat calls still add,
which is what `include/aminetxduo/random.h:48` promises and what
`tools/smoke/randtest.c:240` measures.

**Residual, recorded and not fixed:** a second caller that finds a collection in
flight returns rather than waits, so it can draw a block from a pool the first
collection has not mixed yet. Making it wait means holding the scheduler off for
those 22 ms, which a shared library must not do. It does not arise in the shipped
library: `bsd_runtime_open()` seeds from `InitResident()`, before any opener
exists, exactly so the lazy path never runs.

### crashguard is per-task state in globals, and is not in the library

`ami_crash_task`, `ami_crash_jmp`, `ami_crash_old_trap`, `ami_crash_ref` and the
five `ami_crash_saved_*` globals are all properties of the one task that
installed the guard, held at file scope with no guard of any kind. Two installers
is unrecoverable: task B's `setjmp()` overwrites `ami_crash_jmp` with B's stack
frame, and a crash in A then `longjmp()`s A onto B's stack. `ami_alert_old`
(`:253`) has an install-once test read outside the `Forbid()` that does the
`SetFunction()`, so two installers can leave the trampoline tail-jumping to
itself inside Exec's `Alert()`.

**Not fixed, because none of it is in a shared library.** `aminetxduo_common` is a
static archive and nothing under `src/` references any `ami_crash_*` entry point
-- the only callers are `tools/smoke/{crashtest,lifecycle,kernelstop,gurutest}.c`,
which are separate single-task executables, so `crashguard.o` is never extracted
into `bsdsocket.library`. The file already documents the single-task resume as a
"KNOWN LIMITATION"; the multi-task case is strictly worse and is recorded here
rather than in that comment because it would only matter if this were ever linked
into the library.

---

## src/tls, src/tlslib

`tls.library` hands every opener the same base (`tls_library.c:8`), so nothing
here *can* be per-opener. Where a static is per-connection, the correct home is
the `TLSConnection` or the nx_secure per-session metadata area that
`_nx_secure_tls_metadata_size_calculate()` already sizes.

| Object | Verdict | Guard |
|---|---|---|
| `ami_tls_crypto.c:36` `ami_arithmetic` | constant | the setter has no LVO; write-never in the shipped library |
| `ami_tls_crypto.c:37` `ami_crt_enabled` | constant | as above |
| `ami_tls_crypto.c:56` `ami_counters[2]` | **misplaced -- not fixed** | per-opener instrumentation; inert, see below |
| `ami_tls_crypto.c:57` `ami_client_thread` | **misplaced -- not fixed** | as above |
| `ami_tls_crypto.c:128` `ami_p256_curve` | singleton | two lazy inits that do not exclude each other, but every write is byte-identical. See below |
| `ami_tls_crypto.c:129` `ami_p256_ready` | singleton | set last, so a reader either re-inits or sees a complete curve |
| `ami_tls_crypto.c:296` `ami_rsa_keys[4]` | **misplaced -- not fixed** | unreachable in the shipped library, see below |
| `ami_tls_crypto.c:297` `ami_rsa_key_count` | **misplaced -- not fixed** | as above |
| `ami_tls_crypto.c:1528` `ami_x509_cipher_table` | constant | no writer; non-`const` only because `NX_SECURE_TLS_CRYPTO` takes a mutable pointer |
| `ami_tls_crypto.c:1565` `ami_ciphersuite_table` | constant | as above |
| `tls_amiga.c:30` `ami_tls_timer_base` | **unguarded -- FIXED** | the second of the two timer opens |
| `tls_amiga.c:32` `ami_tls_req` | **unguarded -- FIXED** | as above |
| `tls_amiga.c:33` `ami_tls_port` | **unguarded -- FIXED** | as above |
| `tls_amiga.c:34` `ami_tls_hz` | **unguarded -- FIXED** | as above |
| `rfc7905/..._decrypt.c:38` `save_iv[20]` | constant | dead storage, see below |
| `rfc7905/..._encrypt.c:42` `_nx_secure_tls_record_block_buffer` | **misplaced -- not fixed** | the most serious finding here, see below |
| `tls_library.c:27` `SysBase` | singleton | written once at `LibInit` |
| `tls_library.c:36` `tls_lib_name` | constant | `rt_Name` is `char *` |
| `tls_library.c:37` `tls_lib_id` | constant | `rt_IdString` |
| `tls_netx.c:40` `tls_ctx` | singleton | write-once under `TLSBase->tb_Lock`, every reader NULL-checks |
| `tls_runtime.c:21` `DOSBase` | singleton | opened at `LibInit`, closed at expunge |
| `tls_store.c:448` `tls_registry[8]` | **unguarded -- FIXED** | see below |

### `_nx_secure_tls_record_block_buffer` -- per-record scratch in a global

One `NX_SECURE_TLS_MAX_CIPHER_BLOCK_SIZE` buffer for the whole machine, used as
the CBC padding source, the AEAD ICV destination, and both the input and the
output of the record decrypt. **The baton does not cover it**, because it stays
live across exactly the calls that release the baton:

- encrypt `:731` -- `MEMSET` the padding in, then
  `nx_packet_data_append(..., NX_WAIT_FOREVER)`. On an empty packet pool that
  suspends, and another adopted task can `MEMSET` different padding into the same
  bytes before the first append copies them out.
- decrypt `:816` -- fill the buffer as `input`, then
  `nx_packet_allocate(..., wait_option)` at `:860` before decrypting from it at
  `:911`.
- decrypt `:876` -- decrypt into the buffer, then
  `nx_packet_data_append(..., wait_option)` at `:922` with plaintext sitting in
  it.

So yes: **two simultaneous TLS connections can corrupt each other through this
buffer, and nothing serialises them.** The window needs packet-pool exhaustion,
so it is narrow rather than routine, and the CBC path at `:293-303` is the worst
outcome because a corrupted buffer becomes the next record's IV.

**Not fixed here.** It is vendored NetX Duo shape carried into our RFC 7905
files, the correct fix is to move it into the per-session metadata area, and that
is a change to crypto code with no unit test between it and a silently wrong
handshake. It is written down here rather than attempted at the end of an audit.

`save_iv[20]` in the decrypt file is the same shape and is **dead**: the only two
references in the file are the definition and a `sizeof()`. The TLS 1.0 body that
used it is gone from this copy. If TLS 1.0 is ever re-enabled it becomes the same
bug.

### `tls_registry[]` -- claim guarded, scan not (FIXED)

Eight `{store, connection}` pairs, the back-pointer nx_secure's store-only verify
hook needs. `tls_registry_add()` and `tls_registry_remove()` both take `Forbid()`;
`tls_conn_for_store()` and `tls_conn_for_session()` did not.
`tls_conn_for_session()` dereferences `rs_Conn` to reach `tc_Session`, so a
`TLSClose()` on another task that clears the slot and frees the `TLSConnection`
mid-scan was a use-after-free. Both scans take the same `Forbid()` now.

Two things remain, recorded: eight slots is a machine-wide limit, so the ninth
concurrent connection loses certificate verification (it fails closed, at
`tls_store.c:639`) and resumption; and a program that exits without `TLSClose()`
leaves a slot holding a dangling `TLSConnection *`.

### The two that are inert

`ami_rsa_keys[]` is a four-entry registry of private-key primes matched **by
modulus bytes**, not by session, holding borrowed pointers into the caller's DER.
Any opener holding the same modulus would pick up another's `p`/`q`, nothing
reclaims a slot when the certificate is freed, and the fifth certificate silently
loses CRT acceleration for ever. It is unreachable in the shipped library: tls.library
is client-only and registers no local certificate, and no LVO reaches
`ami_tls_local_certificate_add()`. The only callers are in `tests/`.

`ami_counters[2]`/`ami_client_thread` are a two-bucket profiling split: one
thread is nominated and everyone else shares bank 1. With three concurrent TLS
users it is not meaningful, `counters_reset()` zeroes everyone's numbers, and
`set_client_thread()` is last-writer-wins. Nothing in the shipped library calls
the setter, so `ami_client_thread` is NULL and every opener shares bank 1.
Instrumentation only; no handshake depends on it.

### The P-256 curve init is racy and benign

`ami_tls_crypto_initialize()` is called from two paths that do not exclude each
other -- `tls_conn.c:307` under `TLSBase->tb_Lock`, and
`ami_crypto_method_ec_secp256_operation()` at `:243` under the baton. Both can
run the init. Every write is byte-identical: the same struct copy from the same
const curve and the same function pointer. `ami_p256_ready` is set last, so a
reader either sees zero and re-inits or sees a complete curve. Under
`AMINETXDUO_TLS_CRYPTO68K_SELFCHECK` the failure branch swaps
`nx_crypto_ec_multiple` back to the reference implementation, which is still a
valid pointer and still correct, just slower. Left alone.

---

## src/usergroup

usergroup.library gets this right. Cursors, credentials, result records and
scratch buffers are on the per-opener child base
(`usergroup_internal.h:175-189`); the parsed tables and the lock are in the
heap-allocated shared `UgGlobal` (`:152-158`). **None of the `ug_*` statics needs
to move.**

| Object | Verdict | Guard |
|---|---|---|
| `ug_library.c:41` `SysBase` | singleton | written once at `LibInit` |
| `ug_library.c:42` `DOSBase` | singleton | the write in `ug_dos()` is under `g->lock` behind the `dos_tried` one-shot; the clear in expunge relies on `lib_OpenCnt == 0` |
| `ug_library.c:47` `ug_lib_name` | constant | `rt_Name` |
| `ug_library.c:48` `ug_lib_id` | constant | `rt_IdString` |
| `ug_db.c:31-35` `ug_def_name`, `_empty`, `_gecos`, `_dir`, `_shell` | constant | never written; `char[]` because `struct ug_passwd` fields are `char *` |
| `ug_db.c:37` `ug_def_members` | constant | never written; `char *[]` because `gr_mem` is `char **` |

The one thing worth naming: those defaults are handed straight out to callers in
`struct ug_passwd`/`struct ug_group` on the no-file path, so a client that writes
through `pw->pw_dir` or edits `gr_mem` in place corrupts the default for every
program on the machine, permanently. `ug_def_empty` is the most exposed --
`ug_field()` returns it for every missing field of every parsed line. That is the
BSD ABI's fault, not this code's, and copying on return is a different change.

---

## The constants

Thirty-eight file-scope `static const` objects across the nine directories: the
errno and status maps (`errno.c`), the TCP state names (`netstats.c`), the
built-in netdb text (`config/netdb.c`), the interface and address-type keyword
tables (`config_parse.c`), the ARexx variable definitions
(`netstack_rexx_vars.c`), the SANA-II reader types, depths and names
(`sana2_rx.c`), the SHA-256 round constants (`ami_random.c`), the vector and init
tables, and `netstack_capture.c:39`'s `ami_ns_lo_cookie`, whose *address* is the
loopback identity and whose contents are never read. None is written; all are
read-only from every opener; nothing to do.

Eleven more are function-local `static const` -- log prefixes, hex digit strings,
boolean word lists, the mDNS `.local` suffix. Same verdict.

The writable objects that are constants in practice are named in the tables
above. Ten of them cannot be `const` because an Exec or NetX Duo struct field is
typed `char *`: the three library names, the three ID strings, the two socket
names, `ami_health_name` and `ami_rx_port_name`. `bsd_no_aliases`,
`ug_def_members` and the five `ug_def_*` strings are the same story with the BSD
ABI. `ami_cfg_empty` is the one that could genuinely be `const` if
`ami_cfg_next_pair()`'s out-parameter were.

---

## What was fixed

| Where | Was |
|---|---|
| `mcast.c` `bsd_mcast_join`, `bsd_mcast6_join` | row claimed inside the bracket, marked outside it; two bases could take one row |
| `tcp_handler.c` `tcp_started` | never cleared; TCP: could not be restarted in a resident segment |
| `addralloc.c` `bsd_aam_boot`, `bsd_aam_boot_parent` | hand-over slot overwritable across the launcher's `Permit()`/`Wait()` |
| `raw.c` `bsd_raw_close` | the stack-down branch left a freed socket on the registry |
| `compat.c` `ami_timer_init` | unserialised device open + accumulator reset |
| `tls_amiga.c` `ami_tls_timer_open` | the same, again |
| `ami_random.c` `ami_random_init` | two concurrent 22 ms collections into one buffer |
| `tls_store.c` `tls_conn_for_store`, `tls_conn_for_session` | registry scanned without the `Forbid()` the claim takes |
| `sana2_device.c` `ami_raw_probe_slot` | per-probe scratch at file scope; now on the stack |
| `bpf_channel.c` `ami_bpf_init` | the interface table was not zeroed at bring-up |
| `netstack_rexx.c` `ami_rx_port` | port on Exec's list before the global recorded it |

## What was not

- **`_nx_secure_tls_record_block_buffer`** -- two concurrent TLS connections can
  corrupt each other. Needs the buffer moved into the per-session metadata area;
  a crypto change, not an audit change.
- **`ami_ns` readers versus teardown** -- needs a reference count on
  `netstack_ip()`/`netstack_pool()`.
- **`ami_baton_slot[]` leaking on a task that dies mid-bracket** -- needs a
  liveness test for a `struct Task *`.
- **`ami_rsa_keys[]`, `ami_counters[]`, `ami_client_thread`** -- per-session and
  per-opener state in globals, all unreachable through any LVO in the shipped
  library.
- **`ami_cfg_reporter`/`ami_cfg_reporter_user`** -- per-caller callback, dormant
  in the library.
- **crashguard** -- per-task state in globals, not linked into any shared library.
- **The netmonitor hook list** -- shared by documented contract; there is nowhere
  to record an owner.
- **The `ug_def_*` defaults handed out as mutable `char *`** -- BSD ABI.
- **The CSPRNG's second caller during a collection** -- fixing it means a 22 ms
  Forbid.

## What could not be settled

- **`ami_raw_allowed`** (`sana2_device.c:22`). Public setter, no caller in the
  tree, and the code does not say whether framing policy is per-machine or
  per-opener. Left as it is.
- **Whether skipping the second `nx_system_initialize()`** is safe.
  `ami_ns_system_initialised` deliberately makes NetX Duo's system init run once
  per segment load while ThreadX *is* fully restarted by
  `tx_amiga_kernel_stop()`/`_start()`. The asymmetry is visible and intentional;
  nothing in-tree explains it, and `third_party/netxduo` is a submodule that is
  not checked out here, so the function could not be read.
- **`ami_cfg_empty`** -- handed out as a mutable `char *` to four call sites in
  `config_parse.c`. A write through it would corrupt one shared byte for every
  later parse. Whether any of those four can write a non-NUL byte was not
  established; the type permits it.

---

## The instrument

`tests/tools/run-cycledrill.sh` opens the library many times over, nested and
cycled, which is the closest existing test to any of this. It opens from **one
task**, so it exercises the persistence half of the problem -- what a static
carries from one program into the next -- and not the concurrency half. Nothing
in the tree today puts two tasks inside the library at once on purpose. That is
the gap this document leaves behind.
