# Allocations

Every `ami_alloc()` and `ami_alloc_flags()` call site in the tree, and what frees
it.

`ami_alloc()` is `AllocVec()` and `ami_free()` is `FreeVec()`
(`src/common/compat.c`). **AmigaOS has no MMU and does not reclaim `AllocVec`
memory when a process exits.** A Shell command that allocates and returns leaks
until reboot; a library allocation leaks until expunge. The supported floor is a
1 MB machine, so a 12 KB per-run leak is about seventy runs from exhaustion --
which is what `ami_netdb_load()` was doing in `ping`, `ShowNetStatus` and
`AddNetRoute` until commit 4df5315.

`NETSTATUS_HEALTH` reports `nsl_AllocLive`/`nsl_AllocPeak`. Those count blocks
that went through `ami_alloc()` only: a raw `AllocMem` or a `CreateNewProc` stack
is invisible to them. The raw ones are listed at the bottom.

46 shipping call sites, plus 2 in host-only test code. Six leaked; all six are
fixed here.

**`grep` needs `LC_ALL=C grep -a`** in this tree -- the NDK headers are Latin-1
and a plain `grep` reads them as binary and silently returns nothing.

---

## Shell commands -- leak until reboot

| Site | What | Freed by | Verdict |
|---|---|---|---|
| `src/tools/netsetup.c:1130` | `Blob` file-build scratch | `netsetup.c:1140/1148/1161/1173/1193/1217` | clean -- all six returns after the alloc free it |
| `src/tools/shownetstatus.c:1400` | `AmiConfig` read from disk | `shownetstatus.c:1444` | clean -- the only earlier return is its own alloc-failure guard |
| `src/tools/tool_diag.c:73` | `FileInfoBlock` for a directory scan | `tool_diag.c:113` | clean -- the `Examine`/`ExNext` block has no return |
| `src/tools/tool_diag.c:288` | `IOSana2Req` for a device probe | `tool_diag.c:311` | clean |
| `tools/smoke/lifecycle.c:216` | worker thread stack | `lifecycle.c:233` | clean -- paired inside the round loop |
| `tools/smoke/lifecycle.c:243` | blocker thread stack | `lifecycle.c:261` | clean |
| `tools/smoke/lifecycle.c:289` | victim Task stack | `lifecycle.c:330` | clean |
| `tools/smoke/lifecycle.c:348` | stuck thread stack | `lifecycle.c:394` | clean -- freed only after the zombie is confirmed gone |
| `tools/smoke/kernelstop.c:189` | worker thread stacks | `kernelstop.c:210` (`work_stop`) | clean -- `work_start`/`work_stop` are paired at both call sites, no return between |
| `tools/smoke/kernelstop.c:376` | stuck thread stack | `kernelstop.c:417` | clean |

### The netdb: four more commands leaked it -- FIXED

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
`src/tools/netstack_weak.c:45`, which always returns NULL -- so the branch is
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

## Library -- leaks until expunge

### bsdsocket

| Site | What | Freed by | Verdict |
|---|---|---|---|
| `handoff.c:169` | `BsdHandoff` registry node | `handoff.c:186` (duplicate id), `:341` (ObtainSocket), `:247` (`bsd_handoff_flush`, from `library.c:486` on the last close) | clean |
| `routing.c:733` | `BsdRouteTable` | `routing.c:742` (bracket failed), `:777` (`bsd_FreeRouteInfo`) | clean -- application-owned after return, no internal callers |
| `netstatus.c:1099` | `AmiMdnsService` rows | `netstatus.c:1144` | clean |
| `addrinfo.c:196` | `BsdAddrInfoNode` | `addrinfo.c:538` (`bsd_freeaddrinfo`) | clean -- every `return EAI_*` in `bsd_getaddrinfo` runs while `head` is still NULL |
| `roadshow.c:149` | `BsdDnsList` | `roadshow.c:281` (`bsd_ReleaseDomainNameServerList`) | clean -- no close-time sweep for an application that forgets, which is the documented Roadshow contract |
| `socket.c:329`, `:359`, `:379` | descriptor table `sb_Table` | `library.c:284` (`bsd_child_destroy`); the superseded table at `socket.c:387` | clean -- `bsd_lib_open` only ever returns child bases, so the master never allocates one, which is why the expunge path has no `sb_Table` free |
| `socket.c:432` | `AmiSocket` | `socket.c:518` (`bsd_socket_dispose`) | **was a leak -- fixed, see below** |
| `interfaces.c:370` | `BsdIfList` | `interfaces.c:411` | clean -- no exit between alloc and return |
| `interfaces.c:1785` | `BsdIfNameIndex` | `interfaces.c:1829` | clean |
| `addralloc.c:313` | carved `AddressAllocationMessage` | `addralloc.c:449` | clean -- no failure return after the alloc; the cookie is cleared before the free, so a double delete is a no-op |
| `addralloc.c:797` | `BsdAamJob` | `addralloc.c:819`, `:857` (launch failures), `:716` (worker) | clean -- the hand-off slot is Forbid()-guarded across `CreateNewProc` + `Wait` |

#### `socket.c:432` -- the closing list was never drained (FIXED)

`bsd_tcp_close_start()` parks a socket with a FIN in flight on the file-static
`bsd_closing_head` and returns FALSE, so the caller does not dispose it. Only
`bsd_closing_sweep()` frees these, and it collects a socket only once the close
has finished or the deadline has passed.

On the **last** `CloseLibrary()`, `bsd_close_all()` parks this program's sockets
and sweeps in the same breath -- the FINs went out microseconds ago, so nothing
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
allocates -- if *that* allocation fails the `AmiSocket` is dropped with no
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
| `ug_db.c:338` | `gr_members` pointer pool | `ug_library.c:168` | clean -- `gr_loaded` is set before the read, so it cannot be overwritten while live |
| `ug_library.c:200` | per-opener child base | `ug_library.c:247`, as `base - lib_NegSize` -- the same pointer `ami_alloc` returned | clean |
| `ug_library.c:386` | the single `UgGlobal` | `ug_library.c:291`, after `ug_db_free` | clean |

### netstack, sana2, mbuf, bpf

| Site | What | Freed by | Verdict |
|---|---|---|---|
| `netstack.c:1208` | `AmiNetStack` | `netstack.c:1220`, `:1229`, and `:455` via `ami_ns_destroy()` on the five later failure paths and on `netstack_shutdown()` | clean |
| `netstack.c:1247/1248/1249` | packet pool memory, IP thread stack, ARP cache | one combined NULL test at `:1251` sends any failure to `ami_ns_destroy()`, which frees each under its own guard (`:435`, `:451`, `:446`) plus `ns` at `:455` | clean -- the third-of-four failure does free the first two |
| `netstack.c:760` | AutoIP thread stack | `netstack.c:773` on create failure (with NULL-out), `:441` in destroy | clean |
| `netstack_mdns.c:229` | mDNS thread stack | `netstack_mdns.c:256` on create failure, `:328` in `mdns_stop`, reached from `ami_ns_destroy()` at `netstack.c:379` | clean -- deliberately not in destroy's own free block, and `MdnsCreated == FALSE` with a live stack is unreachable |
| `netstack_mdns.c:736` | `AmiMdnsScratch` | `netstack_mdns.c:746`, `:834` | clean -- the collect loop has only `break`/`continue` |
| `sana2_device.c:520` | `AmiSana2If` | `sana2_device.c:559`, `:579`, `:676` (`ami_sana2_close`), and every caller path in `netstack.c` | clean; the two early returns in `ami_sana2_close` are the documented `rx_orphaned` case |
| `sana2_rx.c:699` | reader thread stack | `sana2_rx.c:861` | **was a leak -- fixed, see below** |
| `mbuf_alloc.c:189` | mbuf slab | `mbuf_alloc.c:109` (`ami_mbuf_cleanup`) | correct, but see the note below. The freed pointer is exactly the allocated one: the alignment slack is consumed *after* the header, and `slab->raw` keeps the original |
| `mbuf_alloc.c:318` | `AmiCluster` | `mbuf_alloc.c:117` | correct; clusters are by design returned to a free list rather than freed individually |
| `bpf_channel.c:760` | capture buffer (store + hold) | `bpf_channel.c:214` (`ami_bpf_chan_release`) | **raced -- fixed, see below** |
| `bpf_channel.c:809` | filter program copy | `bpf_channel.c:824` (validation failed), `:839` (superseded), `:215` (channel close) | clean -- the swap is under the lock, the free of the old program outside it |

#### `sana2_rx.c:699` -- the reader stack leaked when the reader never started (FIXED)

`rx->started` is set at `sana2_rx.c:726`, *after* thread creation; the 4 KB stack
is allocated at `:699`, *before*. `ami_sana2_rx_stop()` gated its whole body on
`rx->started`, so both intermediate failures -- semaphore creation at `:707`,
thread creation at `:715` -- unwound by calling `rx_stop`, which skipped the
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

#### `bpf_channel.c:760` -- a racing `BIOCSETIF` dropped a buffer (FIXED)

`ami_bpf_ioctl_setif()` tested `ch->bufbase == NULL` outside the lock and stored
inside it -- `ami_alloc()` is not something to call under Forbid(). Two tasks
sharing a base and issuing `BIOCSETIF` on the same channel could both see NULL,
both allocate up to `2 * BPF_MAXBUFSIZE`, and the second store would drop the
first block with no reference left. The test is now repeated inside the lock and
the loser is freed after the unlock.

---

## Host-test-only sites

Not shipped and not on m68k, listed for completeness. `src/config/test/`,
`src/mbuf/test/`, `src/bpf/test/` and `tests/fuzz/` each define their own
malloc-backed `ami_alloc`/`ami_free`/`ami_alloc_count` stubs; those definitions
are not call sites. The two real ones are the file-read hooks --
`src/config/test/test_config.c:122` and `tests/fuzz/fuzz_config.c:93` -- whose
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

## Not routed through `ami_alloc` -- invisible to `nsl_AllocLive`

Checked and balanced:

- `src/tools/fetch.c:1197` -- `StackSwap` stack, `FreeMem` at `:1206`.
- `src/tools/nettrace.c:480` -- capture buffer, `FreeVec` at `:540` in
  `nt_cap_stop()`. The `nt_out_open` failure at `:487` returns without freeing,
  but `cap->open` is already TRUE, so the caller's `nt_cap_stop()` at `:985`
  collects it.
- `src/netstack/netstack_rexx_vars.c:261/733/953/1154` -- ARexx reply growth and
  the three query scratch tables; all freed on every return, and
  `ami_rx_reply_init`/`_done` are paired with no return between.
- `src/bsdsocket/library.c:159` -- child base, `FreeMem` at `:245` (signal
  allocation failed) and `:302` (`bsd_child_destroy`).
- `src/netstack/netstack.c:153` -- `AmiNetCaller`, `FreeMem` at `:161` and
  `:174`. Every `ami_netstack_enter_alloc()` in `src/netstack/` reaches a
  `ami_netstack_leave_free()`; the returns that look like escapes are all the
  `caller == NULL` guard or are preceded by the free.

**Not audited:** `clients/` (dropbear and the argv shim),
`port/threadx-amiga/`, and `src/tlslib/`. Those have their own raw
`AllocMem`/`AllocVec` sites and were out of scope here.

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
  Safe as called -- `netstack_capture.c:164` runs at bring-up and the matching
  `ami_bpf_cleanup()` at `:252` runs from the top of `ami_ns_destroy()` -- but it
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
