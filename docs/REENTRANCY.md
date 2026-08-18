# Reentrancy

An AmigaOS library has one data segment shared by every opener at once, and a
file-scope object keeps its value until the segment is unloaded, so the next
unrelated program sees what the last one left. Per-opener state belongs on the
child base (`src/bsdsocket/bsdsocket_internal.h`,
`src/usergroup/usergroup_internal.h`); `tls.library` hands every opener the same
base (`src/tlslib/tls_library.c:8`), so nothing in `src/tls` or `src/tlslib` can
be per-opener.

Writable file-scope state, and what serialises it. `static const` is omitted.

| Object | Guard | Effect |
|---|---|---|
| `src/bsdsocket/library.c:944` `bsd_net_boot` | `master->sb_Lock`, held across `bsd_netstack_bringup()` | |
| `src/bsdsocket/netmonitor.c:77` `bsd_mon_list[]` | `Forbid()` on install, remove and dispatch | Nothing removes a base's hooks when it closes; a program that exits without `RemoveNetMonitorHook()` leaves a hook the library keeps calling and an expunge it keeps declining. The `MinNode` is in the caller's own `Hook`, so there is nowhere to record an owner |
| `src/bsdsocket/netmonitor.c:78` `bsd_mon_ready`, `:79` `bsd_mon_count` | `Forbid()` | |
| `src/bsdsocket/addralloc.c:513` `bsd_aam_jobs[]` | `Forbid()` on claim and release | One row per interface; a second `BeginInterfaceConfig()` on the same interface is answered `AAMR_Busy` |
| `src/bsdsocket/addralloc.c:514` `bsd_aam_workers` | `Forbid()` | |
| `src/bsdsocket/oob.c:128` `bsd_oob_mark` | the ThreadX baton | A second OOB send that finds it armed sends its byte unmarked rather than racing |
| `src/bsdsocket/raw.c:110` `bsd_raw_list`, `:111` `bsd_raw_installed` | `ip->nx_ip_protection`, which the IP thread holds for its whole event loop; `Forbid()` on the stack-down path (`:386`) where that mutex has gone with the `NX_IP` | |
| `src/bsdsocket/socket.c:614` `bsd_closing_head` | the baton | Sockets with a FIN in flight, drained on the last close |
| `src/bsdsocket/socket.c:192` `last_budget` | none; function-local | Log de-duplicator, a torn read costs one repeated line |
| `src/bsdsocket/mcast.c:72` `bsd_mcast_table[]`, `:519` `bsd_mcast6_table[]` | the baton, claimed and released inside the bracket | Machine-wide, keyed by `AmiSocket *`: it is the socket-to-group mapping NetX Duo does not keep, since membership there is per `NX_IP` and refcounted |
| `src/bsdsocket/tcp_handler.c:125-129` `tcp_ctrl_port`, `tcp_node`, `tcp_boot`, `tcp_sessions`, `tcp_started` | written by the handler Process; `tcp_sessions` under `Forbid()`, the `tcp_started` latch under `master->sb_Lock` | `tcp_started` is cleared on `ACTION_DIE` and on both launch failures, or `TCP:` cannot be restarted in a resident segment |
| `src/config/config_text.c:101` `ami_cfg_reporter`, `:102` `ami_cfg_reporter_user` | none | Per-caller callback and cookie in two unguarded stores. No in-library caller sets them (`tool_diag.c` and `checknetconfig.c` are separate executables with private copies); the moment anything in the library does, two openers can pair one program's function with another's cookie |
| `src/config/config_text.c:103` `ami_cfg_current_file` | `master->sb_Lock` at every reachable caller | Per-parse state; also holds a pointer to a stack-local `path[]` between `config_file.c:143` and `:145` |
| `src/config/netdb.c:48` `ami_netdb[4]`, `:49` `ami_netdb_loaded` | loaded once from `bsd_lib_open()` under `master->sb_Lock`, read-only after, freed by the expunge | `ami_netdb_load()` sets `loaded` before it parses, so a caller that reached it without the lock would read half-built tables |
| `src/netstack/netstack.c:57` `ami_ns_lock` | `InitSemaphore` under `Forbid()`, both the test and the set inside one | |
| `src/netstack/netstack.c:59` `ami_ns` | writes under `ami_ns_lock`; none of the reads take it | `netstack_ip()` and `netstack_pool()` hand out a pointer that `netstack_shutdown()` NULLs and `ami_ns_destroy()` frees. Bounded in practice: shutdown runs only when `sb_StackRefs` is zero. A fix needs a reference count on the read side |
| `src/netstack/netstack.c:60` `ami_ns_system_initialised` | `ami_ns_lock` | NetX Duo's system init runs once per segment load, while ThreadX is fully restarted by `tx_amiga_kernel_stop()`/`_start()`. The asymmetry is deliberate |
| `src/netstack/netstack_dns.c` `ami_ns->ns_Config.resolver` | the ThreadX baton serialises live writers; short `Forbid()` sections cover every stored mutation and coherent copy | No Exec wait is taken inside the baton. Resolver searches copy the suffix they are about to use and Roadshow reports take a snapshot, so no pointer into a mutable list survives the section |
| `src/netstack/netstack_baton.c:73` `ami_baton_slot[16]` | `Forbid()` on every touch, keyed by `struct Task *` | Nothing sweeps: a Task that dies between acquire and release holds its slot for the life of the segment, and Exec recycles `struct Task` addresses, so a later Task at the same address inherits a stale `TX_THREAD` and nesting count. Exhaustion makes threads block holding the baton |
| `src/netstack/netstack_baton.c:77` `ami_baton_stats` | writes under the callers' `Forbid()` | The snapshot in `netstatus.c` is read without one and can be internally inconsistent. Diagnostics only |
| `src/netstack/netstack_baton.c:87` `ami_health_mark`, `:92` `ami_baton_sampler` | `Forbid()` around `FindSemaphore`/`AddSemaphore`; the field writes under `ami_ns_lock` | |
| `src/netstack/netstack_rexx.c:51` `ami_rx_rexxbase`, `:93` `ami_rx_proc`, `:94` `ami_rx_boot` | task-confined to the ARexx host Process, or serialised by `ami_ns_lock` at both call sites | |
| `src/netstack/netstack_rexx.c:92` `ami_rx_port` | `Forbid()`, taken with `AddPort()`/`RemPort()` as one step | |
| `src/netstack/netstack_rexx.c:113` `ami_rx_gone`, `:114` `ami_rx_stopper`, `:115` `ami_rx_stop_sig` | `Forbid()`, taken together with the `Signal()` | |
| `src/sana2/sana2_device.c:22` `ami_raw_allowed` | none | `ami_sana2_set_raw_allowed()` (`include/aminetxduo/sana2.h`) has no caller in the tree, so this is a compile-time default and the missing guard is unreachable. Whether framing policy is per-machine or per-opener is not settled by the code |
| `src/sana2/sana2_device.c:31` `ami_block_enter`, `:32` `ami_block_leave` | none locally; installed before `tx_amiga_kernel_start()` and cleared after `ami_ns_destroy()`, both under `ami_ns_lock` | |
| `src/sana2/sana2_driver.c:41` `ami_sana2_bindings[]` | `Forbid()` on attach and unbind | `ami_sana2_lookup()` reads it lock-free from the IP thread and is correct by store order only: attach writes `iface` last, unbind clears it first. Reordering those lines breaks it silently |
| `src/bpf/bpf_channel.c:40` `ami_bpf_chan[]` | `ami_bpf_lock()` (`Forbid()`) | Owner is the child base, and `bsd_child_destroy()` closes them, so capture channels die with their opener. One program can still exhaust the pool and every other opener gets `EBUSY` |
| `src/bpf/bpf_channel.c:41` `ami_bpf_bound_channels` | the lock on every update; read lock-free as a fast gate and re-validated per channel under the lock | |
| `src/bpf/bpf_tap.c:23` `ami_bpf_iface[]` | the lock; zeroed at bring-up by `ami_bpf_init()` alongside the channel table | A row surviving a teardown holds a cookie into an `AmiSana2If` that went with the old stack |
| `src/bpf/bpf_tap.c:70` `ami_bpf_addr_hook` | none locally; installed and cleared under `ami_ns_lock`, before the interfaces are detached | |
| `src/common/compat.c:36` `ami_mem` | `Forbid()` per counter | Individual counters are atomic, the snapshot `ami_mem_stats()` copies is not |
| `src/common/compat.c:189` `TimerBase`, `ami_timer_req`, `ami_timer_port`, `ami_eclock_hz` | `ami_timer_lock`, a semaphore rather than `Forbid()` because `OpenDevice()` may `Wait()`; a separate ready flag is set last | `TimerBase` cannot double as the ready flag: the NDK's `ReadEClock()` resolves the library base through it, so it must be set before the rate can be read |
| `src/common/compat.c:195` `ami_eclock_last`, `_ms`, `_rem`, `_carry` | `Forbid()` around the read-modify-write in `ami_millis()` | `ami_millis()` is reached from SANA-II reader threads through `ami_bpf_now()` as well as from any opener's task |
| `src/common/compat.c:395` `ami_sana2_quiesce`, `:396` `ami_sana2_restore` | none locally; set and cleared under `ami_ns_lock` | |
| `src/common/ami_random.c:239-243` `pool_key`, `pool_out`, `pool_out_used`, `pool_counter`, `pool_bits` | `Forbid()` per block | One CSPRNG per machine is the design |
| `src/common/ami_random.c:244` `pool_started`, `:606` `random_sample` | one collection at a time, latched under `Forbid()` | A second caller that finds a collection in flight returns rather than waits, so it can draw a block from a pool that collection has not mixed yet. Making it wait means holding the scheduler off for the 22 ms gather. `bsd_runtime_open()` seeds from `InitResident()`, before any opener exists, so the lazy path does not run in the shipped library |
| `src/common/crashguard.c:33` `ami_crash` | none | Per-task state at file scope: two installers is unrecoverable, since task B's `setjmp()` overwrites the frame and a crash in A `longjmp()`s A onto B's stack. Not linked into any shared library, only `tools/smoke/` and `tests/` reference it |
| `src/tls/ami_tls_crypto.c:56` `ami_counters[2]`, `:57` `ami_client_thread` | none | Two-bucket profiling split, last-writer-wins, and `counters_reset()` zeroes everyone's. No setter caller in the shipped library, so every opener shares bank 1. Instrumentation only |
| `src/tls/ami_tls_crypto.c:128` `ami_p256_curve`, `:129` `ami_p256_ready` | none; two init paths that do not exclude each other | Every write is byte-identical and `ready` is set last, so a reader either re-inits or sees a complete curve |
| `src/tls/ami_tls_crypto.c:296` `ami_rsa_keys[4]`, `:297` `ami_rsa_key_count` | none | Private-key primes matched by modulus bytes rather than by session, holding borrowed pointers into the caller's DER; no slot is reclaimed when a certificate is freed and the fifth loses CRT acceleration permanently. Unreachable in the shipped library: no LVO reaches `ami_tls_local_certificate_add()`, only `tests/` calls it |
| `src/tls/tls_amiga.c:31` `ami_tls_timer_base`, `ami_tls_req`, `ami_tls_port`, `ami_tls_hz` | the same semaphore-plus-ready-flag shape as `compat.c`, and `ami_tls_timer_close()` from `tls_runtime_close()` | Reached from `ami_tls_eclock*()` on whatever task is doing crypto |
| `src/tlslib/tls_netx.c:42` `tls_ctx` | write-once under `TLSBase->tb_Lock`, every reader NULL-checks | |
| `src/tlslib/tls_store.c:448` `tls_registry[8]` | `Forbid()` on claim, remove and both scans | Eight slots is a machine-wide limit: the ninth concurrent connection loses certificate verification (fails closed at `tls_store.c:639`) and resumption. A program that exits without `TLSClose()` leaves a slot holding a dangling `TLSConnection *` |
| `src/usergroup/ug_parse.c:19-25` `ug_def_name`, `_empty`, `_gecos`, `_dir`, `_shell`, `_members` | none | Handed straight out in `struct ug_passwd`/`struct ug_group` on the no-file path, so a client that writes through `pw_dir` or edits `gr_mem` in place corrupts the default for every program on the machine. `ug_def_empty` is the most exposed: `ug_field()` returns it for every missing field of every parsed line. The BSD ABI is what stops these being `const` |
