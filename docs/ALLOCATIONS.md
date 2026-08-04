# Allocations

Every allocation call site in the tree and what frees it, in two parts:
`ami_alloc()`/`ami_alloc_flags()` first, then the raw
`AllocMem`/`AllocVec`/`AllocSignal` sites that bypass it.

`ami_alloc()` is `AllocVec()` and `ami_free()` is `FreeVec()`
(`src/common/compat.c`). **AmigaOS has no MMU and does not reclaim `AllocVec`
memory when a process exits.** A Shell command that allocates and returns leaks
until reboot; a library allocation leaks until expunge. The supported floor is a
1 MB machine, so a 12 KB per-run leak is about seventy runs from exhaustion,
which is what `ami_netdb_load()` was doing in `ping`, `ShowNetStatus` and
`AddNetRoute` until commit 4df5315.

`NETSTATUS_HEALTH` reports `nsl_AllocLive`/`nsl_AllocPeak`. Those count blocks
that went through `ami_alloc()` only: a raw `AllocMem` or a `CreateNewProc` stack
is invisible to them, and `AvailMem()` is the only instrument that sees one.
The raw sites are the second half of this file.

46 `ami_alloc` call sites, plus 2 in host-only test code. Six leaked; all six
are fixed here. 95 raw sites. Sixteen leaked; all sixteen are fixed here.

**`grep` needs `LC_ALL=C grep -a`** in this tree, the NDK headers are Latin-1
and a plain `grep` reads them as binary and silently returns nothing.

---

## Shell commands, leak until reboot

| Site | What | Freed by | Verdict |
|---|---|---|---|
| `src/tools/netsetup.c:1130` | `Blob` file-build scratch | `netsetup.c:1140/1148/1161/1173/1193/1217` | clean, all six returns after the alloc free it |
| `src/tools/shownetstatus.c:1400` | `AmiConfig` read from disk | `shownetstatus.c:1444` | clean, the only earlier return is its own alloc-failure guard |
| `src/tools/tool_diag.c:73` | `FileInfoBlock` for a directory scan | `tool_diag.c:113` | clean, the `Examine`/`ExNext` block has no return |
| `src/tools/tool_diag.c:288` | `IOSana2Req` for a device probe | `tool_diag.c:311` | clean |
| `tools/smoke/lifecycle.c:216` | worker thread stack | `lifecycle.c:233` | clean, paired inside the round loop |
| `tools/smoke/lifecycle.c:243` | blocker thread stack | `lifecycle.c:261` | clean |
| `tools/smoke/lifecycle.c:289` | victim Task stack | `lifecycle.c:330` | clean |
| `tools/smoke/lifecycle.c:348` | stuck thread stack | `lifecycle.c:394` | clean, freed only after the zombie is confirmed gone |
| `tools/smoke/kernelstop.c:189` | worker thread stacks | `kernelstop.c:210` (`work_stop`) | clean, `work_start`/`work_stop` are paired at both call sites, no return between |
| `tools/smoke/kernelstop.c:376` | stuck thread stack | `kernelstop.c:417` | clean |

### The netdb: four more commands leaked it, FIXED

`ami_netdb_load()` builds twelve blocks out of `DEVS:Internet`, 12,616 bytes on
a stock install, and holds them in file-scope statics. Only `ami_netdb_free()`
releases them.

The trap is that **`ami_config_load()` calls `ami_netdb_load()`**
(`src/config/config_file.c:423`), so every caller of `ami_config_load` inherits
the obligation without naming it. Four commands did:

| Caller | Was | Now |
|---|---|---|
| `src/tools/shownetstatus.c:1410` | leaked on **every** run | `atexit` at `:1420` |
| `src/tools/netstat.c:505` | leaked on every run | `atexit` at `:518` |
| `src/tools/nslookup.c:772` | leaked whenever no server was named on the command line | `atexit` at `:781` |
| `src/tools/checknetconfig.c:914` | leaked on every run | `atexit` at `:926` |

`ShowNetStatus` was the worst of the four and looked covered: `names_prepare()`
registers the `atexit` from commit 4df5315, but only under `NAMES`. The
`ami_config_load()` at `:1410` sits behind `if (netstack_config() == NULL)`, and
`netstack_config()` in a command is the weak stub in
`src/tools/netstack_weak.c:45`, which always returns NULL, so the branch is
taken every time and the default invocation leaked the full netdb.

`atexit()` rather than a free before each return, for the reason 4df5315 gives:
these commands leave `main()` from many places and a leak that depends on
covering all of them is one edit away from returning. `ami_netdb_free()` is
idempotent (`netdb_free_one()` zeroes the table at `src/config/netdb.c:330`) and
`ami_free(NULL)` is a no-op, so a duplicate registration and a registration on a
path that never loaded are both harmless.

Every other netdb path is accounted for: `src/bsdsocket/library.c:422` is
released by `bsd_lib_expunge` at `library.c:581` (commit caecc37);
`ping.c:357`, `addnetroute.c:261` and `shownetstatus.c:138` by 4df5315;
`netstack.c:1218` by the library expunge, since netstack only ships inside
`bsdsocket.library` and shares its segment lifetime. The lazy load in
`netdb_table()` (`netdb.c:369`) is reachable from the three tool lookups, and
each is preceded by an explicit load plus its `atexit`.

---

## Library, leaks until expunge

### bsdsocket

| Site | What | Freed by | Verdict |
|---|---|---|---|
| `handoff.c:169` | `BsdHandoff` registry node | `handoff.c:186` (duplicate id), `:341` (ObtainSocket), `:247` (`bsd_handoff_flush`, from `library.c:486` on the last close) | clean |
| `routing.c:733` | `BsdRouteTable` | `routing.c:742` (bracket failed), `:777` (`bsd_FreeRouteInfo`) | clean, application-owned after return, no internal callers |
| `netstatus.c:1099` | `AmiMdnsService` rows | `netstatus.c:1144` | clean |
| `addrinfo.c:196` | `BsdAddrInfoNode` | `addrinfo.c:538` (`bsd_freeaddrinfo`) | clean, every `return EAI_*` in `bsd_getaddrinfo` runs while `head` is still NULL |
| `roadshow.c:149` | `BsdDnsList` | `roadshow.c:281` (`bsd_ReleaseDomainNameServerList`) | clean, no close-time sweep for an application that forgets, which is the documented Roadshow contract |
| `socket.c:329`, `:359`, `:379` | descriptor table `sb_Table` | `library.c:284` (`bsd_child_destroy`); the superseded table at `socket.c:387` | clean, `bsd_lib_open` only ever returns child bases, so the master never allocates one, which is why the expunge path has no `sb_Table` free |
| `socket.c:432` | `AmiSocket` | `socket.c:518` (`bsd_socket_dispose`) | **was a leak, fixed, see below** |
| `interfaces.c:370` | `BsdIfList` | `interfaces.c:411` | clean, no exit between alloc and return |
| `interfaces.c:1785` | `BsdIfNameIndex` | `interfaces.c:1829` | clean |
| `addralloc.c:313` | carved `AddressAllocationMessage` | `addralloc.c:449` | clean, no failure return after the alloc; the cookie is cleared before the free, so a double delete is a no-op |
| `addralloc.c:797` | `BsdAamJob` | `addralloc.c:819`, `:857` (launch failures), `:716` (worker) | clean, the hand-off slot is Forbid()-guarded across `CreateNewProc` + `Wait` |

#### `socket.c:432`, the closing list was never drained (FIXED)

`bsd_tcp_close_start()` parks a socket with a FIN in flight on the file-static
`bsd_closing_head` and returns FALSE, so the caller does not dispose it. Only
`bsd_closing_sweep()` frees these, and it collects a socket only once the close
has finished or the deadline has passed.

On the **last** `CloseLibrary()`, `bsd_close_all()` parks this program's sockets
and sweeps in the same breath, the FINs went out microseconds ago, so nothing
is collected. `bsd_child_destroy`, `bsd_lib_close`, `bsd_lib_expunge` and
`netstack_shutdown` then run without sweeping again. Any program that exits with
a live TCP connection lost `sizeof(AmiSocket)` per connection.

The second half is worse than the leak: `netstack_shutdown()` →
`ami_ns_destroy()` `ami_free()`s the `AmiNetStack` at `netstack.c:455`, and the
`NX_IP` is embedded in it. Every parked socket is left holding an
`nx_tcp_socket_ip_ptr` into freed memory, and if the library stays resident the
*next* program's `socket()` runs the sweep through that pointer.

Fixed with `bsd_closing_drain()` (`socket.c:757`), called from the end of
`bsd_close_all()` under the same `sb_StackRefs <= 1` test `bsd_lib_close()`
already uses to decide `bsd_handoff_flush()`, and inside the same
`bsd_nx_enter()` bracket. It aborts and disposes everything still parked.
Terminating by construction: `ASF_CLOSING` is set on everything on the list, and
`bsd_socket_destroy()` skips `bsd_tcp_close_start()` when it sees that flag, so
nothing can be re-parked underneath the loop.

Not fixed, and deliberate: the `ASF_ORPHANED` path (`socket.c:920`) keeps a
block NetX Duo refuses to release, and logs it. Not fixed, ENOMEM-only:
`bsd_ObtainSocket` (`handoff.c:343`) re-parks via `bsd_handoff_park`, which
allocates, if *that* allocation fails the `AmiSocket` is dropped with no
reference held. Recorded rather than fixed: it needs an error path that cannot
allocate, which is a different change.

### config, usergroup

| Site | What | Freed by | Verdict |
|---|---|---|---|
| `config_file.c:63` | file text from `ami_cfg_read_file()` | caller's; six callers all balanced (`:147`, `:272`, `:307`, `:330`, `:368`, `:402`), plus `netdb.c:307/318/329`; internal failure at `config_file.c:77` | clean |
| `config_file.c:203` | `FileInfoBlock` for the NetInterfaces scan | `config_file.c:245` | clean |
| `netdb.c:177` | netdb entry array | `netdb.c:327`, rollback at `:183` | clean inside the library; see the four tools above |
| `netdb.c:179` | netdb alias pool | `netdb.c:328`, rollback at `:184` | as above |
| `netdb.c:310` | built-in fallback text, becomes `table->buffer` | `netdb.c:329`, parse failure at `:318` | as above |
| `ug_db.c:186` | passwd/group file text | `ug_library.c:158`/`:163` (`ug_db_free`, from expunge at `:283`); short read at `ug_db.c:198` | clean |
| `ug_db.c:338` | `gr_members` pointer pool | `ug_library.c:168` | clean, `gr_loaded` is set before the read, so it cannot be overwritten while live |
| `ug_library.c:200` | per-opener child base | `ug_library.c:247`, as `base - lib_NegSize`, the same pointer `ami_alloc` returned | clean |
| `ug_library.c:386` | the single `UgGlobal` | `ug_library.c:291`, after `ug_db_free` | clean |

### netstack, sana2, mbuf, bpf

| Site | What | Freed by | Verdict |
|---|---|---|---|
| `netstack.c:1208` | `AmiNetStack` | `netstack.c:1220`, `:1229`, and `:455` via `ami_ns_destroy()` on the five later failure paths and on `netstack_shutdown()` | clean |
| `netstack.c:1247/1248/1249` | packet pool memory, IP thread stack, ARP cache | one combined NULL test at `:1251` sends any failure to `ami_ns_destroy()`, which frees each under its own guard (`:435`, `:451`, `:446`) plus `ns` at `:455` | clean, the third-of-four failure does free the first two |
| `netstack.c:760` | AutoIP thread stack | `netstack.c:773` on create failure (with NULL-out), `:441` in destroy | clean |
| `netstack_mdns.c:229` | mDNS thread stack | `netstack_mdns.c:256` on create failure, `:328` in `mdns_stop`, reached from `ami_ns_destroy()` at `netstack.c:379` | clean, deliberately not in destroy's own free block, and `MdnsCreated == FALSE` with a live stack is unreachable |
| `netstack_mdns.c:736` | `AmiMdnsScratch` | `netstack_mdns.c:746`, `:834` | clean, the collect loop has only `break`/`continue` |
| `sana2_device.c:520` | `AmiSana2If` | `sana2_device.c:559`, `:579`, `:676` (`ami_sana2_close`), and every caller path in `netstack.c` | clean; the two early returns in `ami_sana2_close` are the documented `rx_orphaned` case |
| `sana2_rx.c:699` | reader thread stack | `sana2_rx.c:861` | **was a leak, fixed, see below** |
| `mbuf_alloc.c:189` | mbuf slab | `mbuf_alloc.c:109` (`ami_mbuf_cleanup`) | correct, but see the note below. The freed pointer is exactly the allocated one: the alignment slack is consumed *after* the header, and `slab->raw` keeps the original |
| `mbuf_alloc.c:318` | `AmiCluster` | `mbuf_alloc.c:117` | correct; clusters are by design returned to a free list rather than freed individually |
| `bpf_channel.c:760` | capture buffer (store + hold) | `bpf_channel.c:214` (`ami_bpf_chan_release`) | **raced, fixed, see below** |
| `bpf_channel.c:809` | filter program copy | `bpf_channel.c:824` (validation failed), `:839` (superseded), `:215` (channel close) | clean, the swap is under the lock, the free of the old program outside it |

#### `sana2_rx.c:699`, the reader stack leaked when the reader never started (FIXED)

`rx->started` is set at `sana2_rx.c:726`, *after* thread creation; the 4 KB stack
is allocated at `:699`, *before*. `ami_sana2_rx_stop()` gated its whole body on
`rx->started`, so both intermediate failures, semaphore creation at `:707`,
thread creation at `:715`, unwound by calling `rx_stop`, which skipped the
reader and lost its stack.

Repeatable, not one-shot: `ami_sana2_rx_start()` runs from
`sana2_driver.c:223` on every `NX_LINK_ENABLE`, and assigns `:699` over
`rx->stack` without testing it. An Online/Offline loop against a driver that
fails thread creation lost 4 KB per cycle per reader.

Fixed by moving the stack free outside the `started` gate in
`ami_sana2_rx_stop()`. The orphan path (`:828`) still `continue`s before it: a
thread that would not stop is running on that stack and it must stay.

The same `||` also left `ready` created but never deleted when `exited` failed.
Its `TX_SEMAPHORE` lives inside the `ami_alloc`ed `AmiSana2If`, so
`ami_sana2_close()` would free memory ThreadX's created-semaphore list still
links. The two creates are now separate, with the matching deletes.

#### `bpf_channel.c:760`, a racing `BIOCSETIF` dropped a buffer (FIXED)

`ami_bpf_ioctl_setif()` tested `ch->bufbase == NULL` outside the lock and stored
inside it, `ami_alloc()` is not something to call under Forbid(). Two tasks
sharing a base and issuing `BIOCSETIF` on the same channel could both see NULL,
both allocate up to `2 * BPF_MAXBUFSIZE`, and the second store would drop the
first block with no reference left. The test is now repeated inside the lock and
the loser is freed after the unlock.

---

## Host-test-only sites

Not shipped and not on m68k, listed for completeness. `src/config/test/`,
`src/mbuf/test/`, `src/bpf/test/` and `tests/fuzz/` each define their own
malloc-backed `ami_alloc`/`ami_free`/`ami_alloc_count` stubs; those definitions
are not call sites. The two real ones are the file-read hooks,
`src/config/test/test_config.c:122` and `tests/fuzz/fuzz_config.c:93`, whose
buffers are freed by the code under test, which several of those tests assert by
checking `ami_alloc_count() == 0` at the end.

---

## `ami_free()`, the mirror check

Every `ami_free()` in `src/` traces to an `ami_alloc()`/`ami_alloc_flags()` in
the same ownership chain. No free of memory that came from somewhere else, and
no double free: each site either NULLs its slot afterwards or destroys the
container immediately.

Two places free memory that did *not* come from `ami_alloc` and correctly use
the matching call instead: the master `bsdsocket` base (`library.c:586`,
`FreeMem`) and the master `usergroup` base (`ug_library.c:294`, `FreeMem`), both
of which come from Exec.

`ami_bpf_chan_release()` reads the pointers, zeroes the channel under Forbid,
and only then frees, so a concurrent release of the same channel frees NULL.

---

## Not routed through `ami_alloc`, invisible to `nsl_AllocLive`

Checked and balanced:

- `src/tools/fetch.c:1197`, `StackSwap` stack, `FreeMem` at `:1206`.
- `src/tools/nettrace.c:480`, capture buffer, `FreeVec` at `:540` in
  `nt_cap_stop()`. The `nt_out_open` failure at `:487` returns without freeing,
  but `cap->open` is already TRUE, so the caller's `nt_cap_stop()` at `:985`
  collects it.
- `src/netstack/netstack_rexx_vars.c:261/733/953/1154`, ARexx reply growth and
  the three query scratch tables; all freed on every return, and
  `ami_rx_reply_init`/`_done` are paired with no return between.
- `src/bsdsocket/library.c:159`, child base, `FreeMem` at `:245` (signal
  allocation failed) and `:302` (`bsd_child_destroy`).
- `src/netstack/netstack.c:153`, `AmiNetCaller`, `FreeMem` at `:161` and
  `:174`. Every `ami_netstack_enter_alloc()` in `src/netstack/` reaches a
  `ami_netstack_leave_free()`; the returns that look like escapes are all the
  `caller == NULL` guard or are preceded by the free.

The five entries above are the spot check that pass made. The full accounting
is the next section.

---

# Raw allocations

Every `AllocMem`, `AllocVec`, `AllocSignal`, `MakeLibrary` and `CreateNewProc`
site in the tree that does not go through `ami_alloc()`. `AllocPooled`,
`CreatePool`, `AllocEntry` and `AllocDosObject` have **no** call sites anywhere
outside `third_party/`, only prose mentions.

**95 sites**: 7 in `clients/`, 10 in `port/threadx-amiga/`, 20 in `src/`, 7 in
`tools/smoke/`, 50 in `tests/`, 1 in `install/test/`. Sixteen leaked; all
sixteen are fixed here.

None of these are visible to `NETSTATUS_HEALTH`'s `nsl_AllocLive`, which counts
only what went through `ami_alloc()`. `AvailMem()` is the sole instrument.

**Every `CreateNewProc`/`CreateNewProcTags` in the tree passes `NP_StackSize`
and none passes `NP_Stack`**, so every Process stack is DOS-allocated and
DOS-freed at process exit. There is no `CreateNewProc` stack leak to find.
Checked: `library.c:372`, `addralloc.c:847`, `tcp_handler.c:930`/`:1186`,
`netstack_rexx.c:517`, `amiga_dropbear.c:1104`/`:1872`, `clientrun.c:156`/
`:168`/`:179`/`:190`, and eight sites in `tests/`.

## Shell commands, leak until reboot

| Site | What | Freed by | Verdict |
|---|---|---|---|
| `clients/compat/amiga_argv.c:242` | 256 KB `StackSwap` stack for every ported client | `amiga_argv.c:261` | **was the worst leak in the tree, fixed, see below** |
| `clients/dropbear/amiga_dropbear.c:1070` | `con_data_bit` | `:1113` (`con_reader_start` failure), `:1127` (`con_reader_stop`) | **was a leak, fixed, see below** |
| `clients/dropbear/amiga_dropbear.c:1071` | `con_done_bit` | as above | as above |
| `clients/dropbear/amiga_dropbear.c:1075` | `ConReader` ring + the child that fills it | `:1107` (spawn failed), `:1125` (`con_reader_stop`) | as above |
| `clients/dropbear/amiga_dropbear.c:756` | `InfoData` for `ACTION_DISK_INFO` | `:766` | clean, the only returns after the alloc are past the free |
| `clients/dropbear/clientrun.c:341` | `InfoData` for the console `Window` | `:349` | clean, freed before every sanity check that can return NULL |
| `src/tools/fetch.c:1197` | 64 KB `StackSwap` stack | `:1206` | clean, `fetch_run()` returns rather than exits, and nothing in `src/` calls `exit()` |
| `src/tools/nettrace.c:480` | capture buffer | `:540` (`nt_cap_stop`) | clean, see the spot-check note above |
| `tools/smoke/randtest.c:207/256` | scratch and block buffers | `:213`, `:350` | clean |
| `tools/smoke/lifecycle.c:97/101` | `MemList`, `Task` | `:104`/`:126`/`:127`, else `RemTask(NULL)`'s `FreeEntry` | clean, the MemList is kept out of the block it describes, which is what stops `AN_FreeTwice` |
| `tools/smoke/kernelstop.c:484` | 8 x 64 KB poison chunks | `:500` (`poison_release`) | clean |
| `tools/smoke/gurutest.c:20` | `AllocVec(256)`, freed at `:21` **and** `:22` |, | **deliberate**, the double free is the test: it provokes `AN_FreeTwice` 0x01000009 |
| `tools/smoke/memprobe.c:66` | `AllocMem(0x7F000000)` | `:69`, guarded | **deliberate**, an impossible size, to force Exec's low-memory expunge |
| `tests/crypto68k/c68k_bulk_bench.c:269` | 4 KB timing table | nothing | **was a leak, fixed**: `FreeMem` at the end of `b_bench_kernels()`. 4 KB per run, and the file had no `FreeMem` at all |
| `tests/soak/fitz_soak.c:1411` | `SoakState` | nothing | **was a leak, fixed**: `atexit(s_release)` |
| `tests/endurance/endurance.c:2390` | `EndState` | `:2405`, `:2415` only | **was a leak, fixed**: `atexit(end_release)`; the two explicit frees are gone, or the atexit would be a double free |
| `tests/endurance/endurance.c` x9 | every worker `w_Buf` (`es_MaxIo`, 4 KB, 8 KB, 16 KB) | nothing | **was a leak, fixed**: allocated through `end_worker_buf()`, which records the size, and released by `end_release()` |
| `tests/soak/fitz_soak.c:725` | filer `f_Buf` | `:743` | clean, the filer allocates and frees its own |
| `tests/tcpdrill/tapdev.c:765` + `MakeLibrary` at `:760` | `TapFrame` ring, device block | `:769`/`:837`, `:839` | clean |
| `tests/bracket/bracket_test.c:171/175/184` | task stack, `MemList`, `Task` | `:179`/`:188`/`:212`, `:187`/`:211`, `:210`; else `bt_reap()`/`RemTask` | clean for memory; see the timeout hazard below |
| `tests/soak/soak_test.c:417/424` | `MemList`, `Task` | `:427`/`:450`, `:449` on failure; on success **nothing** | **deliberate and default-on**: `S_NO_REMTASK 1` (`:165`) compiles out the `RemTask(NULL)` at `:1093` and parks the Task in `Wait(0)` instead, documented at `:153-163` as the workaround for a free-list Guru that would destroy the verdict |
| `tests/perf/fitzbench.c:254/381/382/383`, `tests/perf/cpucal.c:466/467`, `tests/perf/bracket_test.c:354` | benchmark buffers and a signal | `:263`/`:284`, the `out:` label at `:436-440`, `:507`/`:511`, `:357` | clean |
| `tests/stress/fitzstress.c:646/648/649`, `:786/788`, `:850/852`, `:1104` | `FileInfoBlock` + path buffers, worker I/O buffer | `:759-761` (`done:`), `:811-812`, `:912-913`, `:1122` | clean, every `goto done` and the fallthrough reach the label |
| `tests/concurrent/concurrent_test.c:457`, `:561/562` | chunk buffers | `:469`/`:485`/`:493`/`:505`/`:547`, `:566-567`/`:575`/`:588`/`:658-659` | clean, all exits covered, and the partial-allocation case is guarded |
| `tests/tls/tls_resume.c:451`, `:592` | tamper buffer, 64 KB `StackSwap` stack | `:458`/`:467`/`:485`/`:491`, `:602` | clean |
| `tests/libraries/library_test.c:536`, `tests/conformance/conf_probe.c:334`, `tests/mbuf_bpf/mbuf_bpf_test.c:840`, `tests/crypto68k/c68k_amissl_bench.c:1808` | datagram buffer, oversize buffer, signal, signal | `:562`, `:370`, `:864`, `:1835`/`:1841` | clean |
| `install/test/installdrive.c:269` | synthetic `IntuiMessage` | `:254` (`drain_replies`), from `:295` and `:491` | clean in the normal case; a message the Installer never replies to inside 100 ticks is never collected. Bounded and deliberate, getting it back is the proof the Installer consumed it |

### `amiga_argv.c:242`, 256 KB per client invocation (FIXED)

The shim that gives ported clients a real `argv[]` also brings them a 256 KB
stack, because a Shell gives 4 KB and Dropbear's key exchange and curl's TLS
both run deep. `argv_run_on_stack()` swaps onto it, calls `__real_main()`, swaps
back; `__wrap_main()` then `FreeMem`s it.

The file's own comment at `:81` says Dropbear ends by calling `exit()`, always,
and curl on some paths. That never returns to `argv_run_on_stack()`, so the
second `StackSwap()` does not run, which the file already knew and worked
around by restoring `tc_SPLower`/`tc_SPUpper` by hand from `__wrap_exit()`. What
it did not do is free the stack: the `FreeMem` sits after the call that never
returns. **256 KB per invocation of ssh, dbclient and curl, gone until reboot,
on a machine whose floor is 1 MB.** Four runs of `ssh` fill it.

Fixed with `setjmp()` in `__wrap_main()` and a `longjmp()` from `__wrap__exit()`.
`_exit` rather than `exit`: by the time newlib calls it the `atexit()` handlers
have run and stdio has been flushed, all of it on the big stack, which is the
reason for having one. Only the return to DOS is left, and it happens back on
the caller's stack with the block already given back. `argv_stack` is a file
static because a local written before `setjmp()` is not guaranteed to survive
the jump. The jump is gated on `FindTask(NULL)` matching the task that set it,
so a child Process that exits does not jump into somebody else's frame.

Not affected: `src/tools/fetch.c` does the same `StackSwap` dance, but nothing
in `src/` calls `exit()`, `fetch_run()` returns and the `FreeMem` is reached.

### `amiga_dropbear.c:1070-1075`, the console reader outlived the session (FIXED)

`con_reader_start()`/`con_reader_stop()` are driven from `tcsetattr()`: raw mode
starts the child that reads the console, cooked mode stops it. Dropbear's
`cli_tty_cleanup()` is the cooked call, and a session that ends through
`dropbear_exit()` before the tty was ever restored, a refused connection, a
dropped link, a Ctrl-C, never makes it.

The leak is the `ConReader` and both signal bits. The larger problem is the
child: it is still running, still holding `cr`, and its last act is
`Signal(cr->cr_Parent, ...)` to a Task that has exited.

Fixed with one `atexit(con_reader_stop)`, registered on the first
`con_reader_start()` and guarded by a static so a raw/cooked cycle does not
register it again. `con_reader_stop()` returns immediately when `con_reader` is
NULL, so it is safe on every path, including the ones that never opened a
console.

## Library, leaks until expunge

| Site | What | Freed by | Verdict |
|---|---|---|---|
| `src/tlslib/tls_runtime.c:45` | the `tls_alloc()` wrapper, `AllocVec` | `tls_free()` at `:51` | see the per-caller rows below |
| `tls_conn.c:341` + `:414`/`:460`/`:462`/`:464`/`:465`/`:466` | `TLSConnection` and its six buffers | `tls_conn_free()` at `:247-262`, reached from the `fail:` and `fail_session:` labels and from `TLSClose` | clean, every `goto fail` after the alloc lands on it, and `tls_conn_free` also runs `tls_store_detach()`, so the eight-slot registry cannot be left pointing at freed memory |
| `tls_store.c:278` | trust-store index | `:288` (short read), `:231` (`tls_store_close`) | clean, `tls_store_open` calls `tls_store_close` first, so the field cannot be overwritten while live |
| `tls_resume.c:338` | session cache table | `tls_library.c:154`, in the expunge, zeroed first because it holds master secrets | clean, assigned only when NULL |
| `tls_resume.c:670`, `:714` | on-disk record scratch | `:692`/`:745`, and `:721` on the `Open` failure | clean |
| `src/tlslib/tls_library.c` (RTF_AUTOINIT) | the library base, from Exec's `MakeLibrary` | `FreeMem` at `:161` as `TLSBase - lib_NegSize` | clean, the correct mirror; a NULL return from `tls_lib_init` is Exec's to clean up |
| `port/.../tx_initialize_low_level.c:180` | `MemList` for a port Task | `:191`/`:207`/`:232` on failure; else `RemTask`'s `FreeEntry` | clean |
| `port/.../tx_initialize_low_level.c:187` | `_tx_amiga_ctrl` (the `struct Task` lives inside it) | `:206`/`:231` on failure; else `FreeEntry` through `tc_MemEntry` | clean, and deliberately a separate allocation from the MemList, or `FreeEntry` would free the same address twice |
| `port/.../tx_initialize_low_level.c:311` | scheduler baton signal | never | clean, it belongs to the master Task, which `RemTask`s itself; a signal bit dies with its Task |
| `port/.../tx_initialize_low_level.c:329` | `TX_AMIGA_MEMORY_SIZE` kernel region | `:1668` under `_tx_amiga_memory_owned` | **was a `FreeMem(NULL, size)`, fixed, see below** |
| `port/.../tx_initialize_low_level.c:349` | tick task stack | `:1661` | clean, `_tx_amiga_kernel_memory` is guarded by `== 0` and the stop path NULLs both, so a start/stop/start cycle cannot overwrite a live pointer |
| `port/.../tx_initialize_low_level.c:1215/1221` | kernel-start handshake signal, master Task stack | `:1224`, `:1240`/`:1241` on failure; `:1253` and `:1654` on success | clean, the stack is handed to `_tx_amiga_master_stack` only after the Task exists, and the master cannot free the stack it stands on |
| `port/.../tx_initialize_low_level.c:1452` | kernel-stop handshake signal | `:1527` (every refusal) and `:1633` (every outcome) | clean |
| `port/.../tx_thread_schedule.c:198` | reap handshake signal | `_tx_amiga_reap_cleanup()` at `:159`, from all three exits of `_tx_amiga_reap()` | clean |
| `port/.../tx_amiga_adopt.c:241` | per-adoption run signal | `:274` (create failed), `:548`/`:585` (`tx_amiga_orphan_thread`) | clean when orphaned; see "known and not fixed" for the case where it is not |
| `src/bsdsocket/library.c:159` | per-opener child base | `:245`, `:302` | clean |
| `src/bsdsocket/library.c:357` | netstack bring-up handshake signal | `:378` (spawn failed), `:384` | clean; the `sig < 0` path falls back to the caller's stack and allocates nothing |
| `src/netstack/netstack_rexx.c:549` | ARexx host stop signal | `:573` | clean, and the `mask == 0` fallback polls with `Delay` rather than allocating |
| `src/sana2/sana2_rx.c:508` | TX-reap signal, on the reader thread | `:582`, in the same function's tail | clean, there is no return between the two |
| `src/common/compat.c:161` | `ami_signal_alloc()` | `ami_signal_free()` at `:167` | wrapper only |
| `src/common/ami_random.c:425` | four entropy probe blocks | `:442` | clean |
| `src/netstack/netstack.c:153`, `netstack_rexx_vars.c:261/733/953/1154`, `src/tools/*` |, |, | as in the spot check above |

### `tx_initialize_low_level.c:329`, `FreeMem(NULL, 262144)` on the way down (FIXED)

`_tx_amiga_memory_owned` was set `TX_TRUE` whether or not the `AllocMem`
succeeded. On a machine too short of memory to give ThreadX its region, the
kernel came up with `_tx_initialize_unused_memory == NULL` and
`tx_amiga_kernel_stop()` then called `FreeMem(NULL, TX_AMIGA_MEMORY_SIZE)`,
which is not a no-op on Exec, it is a free-list corruption. The flag is now set
only on success, and the free is guarded on the pointer as well.

Not a leak, and the mirror of one: a free of something never allocated.

## Mirror problems

- **Free of something never allocated.** One real instance, above. Two
  deliberate ones: `tests/tools/aamprobe.c:771` calls `DeleteAddrAllocMessage`
  on a stack-resident `AddressAllocationMessage` and `:757` calls it on NULL,
  both to probe the autodoc's "will not work with anything else" contract, which
  the file states at `:12-15`.
- **Double free.** One, `tools/smoke/gurutest.c:21-22`, and it is the test.
  Nowhere else.
- **Freed on one path and not another.** `tests/endurance/endurance.c` `ES`:
  two of five exits freed it. Fixed. `tests/tcpdrill/tapdev.c`: the timer
  `MsgPort` and the open `timer.device` were released only by `tap_remove()`,
  which returns at once while `tap_dev` is NULL, so both `return -1` paths in
  `tap_install()` leaked them permanently. Fixed with a `tap_timer_close()`
  helper on both paths, also used by `tap_remove()`.
- **A pointer overwritten before it is freed**, the `sana2_rx.c` shape that
  cost 4 KB per Online cycle. **No further instance found.** The `started`-flag
  pattern recurs (`concurrent_test.c:499`/`:593`, `soak_test.c:1020`/`:1132`),
  but in each the buffer is a local of a body that runs once per spawned
  Process, so a re-entry gets a fresh local. `con_data_bit`/`con_done_bit` in
  Dropbear are file statics assigned on every `con_reader_start()`, but the
  function returns early while `con_reader != NULL` and the failure path resets
  both to -1, so neither can be overwritten while live. `_tx_amiga_timer_stack`
  and `_tx_amiga_kernel_memory` are both guarded. Stated as a negative finding.

## Known and not fixed, raw sites

- **An adopted ThreadX thread that never orphans keeps its signal bit.**
  `tx_amiga_adopt.c:241` allocates on the caller's own Task because only its
  owner may free it; if the caller never calls `tx_amiga_orphan_thread()` the
  bit is gone for that Task's life. Already documented at `tx_amiga.h:288`.
  Not a memory leak, and unfixable from the port's side.
- **`TLSConnection` is application-owned.** Nothing sweeps live connections at
  `tls_lib_close`/`tls_lib_expunge`; a program that forgets `TLSClose()` leaks
  the connection and its six buffers. Same contract `roadshow.c:149` documents
  for the DNS list, and the expunge cannot run while a base is open anyway.
- **`tests/bracket/bracket_test.c:664`/`:734` free a live Task's stack on the
  60-second timeout.** The wait loop breaks and `bt_reap()` runs regardless of
  `bt_Done`. The file documents exactly this failure at `:222-235` ("100%
  reproducible on Kickstart 3.1") for the non-timeout path, which it then fixed;
  the timeout path reopens it. A use-after-free, not a leak.
  `tools/smoke/lifecycle.c:329-330` and `:392-394` have the same shape, with a
  `Delay` as the only mitigation.
- **`tests/soak/soak_test.c` leaks two Tasks and two MemLists per run by
  default.** `S_NO_REMTASK 1` is deliberate and documented; the Tasks are left
  parked in `Wait(0)` with entry points in a hunk that is about to be unloaded,
  which the file acknowledges at `:2416-2422`.

## Not accounted for

- **`tests/soak/soak_test.c:525` `memblock`**, recorded and logged, never
  freed. It reads as `RemTask`'s to own, but with `S_NO_REMTASK 1` nothing frees
  it at all, and whether the field is meant as a fallback owner is not
  determinable from the source.
- **`tests/soak/soak_test.c:1633` and `:2053`** both hand `s_worker_stack[i]`
, the same static BSS array, to `tx_thread_create` and to `s_spawn_task`.
  No allocation either way, so nothing leaks, but whether the two uses are
  disjoint in time needs someone who knows the phase ordering.

---

## Known and not fixed

- **`ami_mbuf_cleanup()` has no production caller.** The slab and cluster frees
  at `mbuf_alloc.c:109`/`:117` are only reached from
  `tests/mbuf_bpf/mbuf_bpf_test.c` and `src/mbuf/test/test_mbuf.c`. Nothing
  outside `src/mbuf/` calls any `ami_mbuf_*` function today and the `mbuf_*` ABI
  vectors are all `bsd_enosys` stubs
  (`src/bsdsocket/bsdsocket_vectors.c:132-142`), so the pool is unreachable from
  the shipped library and nothing leaks. The moment one of those vectors is
  implemented, every slab and cluster leaks for the life of the library: neither
  `netstack_shutdown()` nor the expunge calls cleanup. Latent, not live.
- **`ami_bpf_close_owner()` does not check `ch->reading`.** `ami_bpf_close()`
  returns EBUSY for a channel with a read in flight (`bpf_channel.c:314`);
  `close_owner`, called from `src/bsdsocket/bpf.c:77` on library close, does not.
  `ami_bpf_read()` drops the lock while copying out of `ch->hold`, so a close
  from a second task in that window frees the buffer under the reader. A
  use-after-free, not a leak, so it is recorded rather than fixed here.
  `ami_bpf_cleanup()` has the same shape and documents it as deliberate.
- **`ami_bpf_init()` zeroes every channel without freeing `bufbase`/`filter`.**
  Safe as called, `netstack_capture.c:164` runs at bring-up and the matching
  `ami_bpf_cleanup()` at `:252` runs from the top of `ami_ns_destroy()`, but it
  would leak for any future caller that skipped the cleanup.
- **`ug_LibOpen` has no child-to-master redirect**, unlike `bsd_lib_open`
  (`library.c:404`). Opening a child base would leave the master's `lib_OpenCnt`
  un-incremented. Not reachable through a normal
  `OpenLibrary("usergroup.library")`, and an expunge-while-live hazard rather
  than a leak.
- **`netstack.c:434` calls `nx_packet_pool_delete()` whenever `ns_PoolMemory`
  is set**, including when the pool was never created. Resolved as safe:
  `NX_DISABLE_ERROR_CHECKING` is deliberately not set
  (`port/netxduo-amiga/inc/nx_user.h:688`), so the call maps to
  `_nxe_packet_pool_delete`, which rejects a zeroed pool with `NX_PTR_ERROR`
  before touching the created list. `ns` comes from `ami_alloc` and is cleared.
