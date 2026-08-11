# Forbid regions

Nothing inside a `Forbid()` may block. Exec resumes multitasking for the
duration of any block, restores `tc_TDNestCnt` when the task is redispatched,
and the code returns believing it was never interrupted, so the damage surfaces
later and somewhere else.

| Constraint | Where | Effect |
|---|---|---|
| `ami_mbuf_lock()` is `Forbid()` and `ami_mbuf_unlock()` is `Permit()` | `src/mbuf/mbuf_amiga.c:21` | The lock drop across `ami_alloc()` in `ami_mbuf_grow()` (`src/mbuf/mbuf_alloc.c:188`) only re-enables switching at nest 1. Calling `ami_mbuf_raw_get()` from inside another mbuf region turns that `AllocVec()` into an allocation under Forbid, silently. One caller today, and it has no path that already holds the lock |
| `ami_bpf_capture()` runs the filter and the record copy under the channel lock | `src/bpf/bpf_channel.c:453-500` | The filter is up to `BPF_MAXINSNS` 512 interpreted instructions and `blen` is client-settable to `BPF_MAXBUFSIZE` 0x8000 through `BIOCSBLEN`, so this is the longest Forbid region in `src/`: tens of milliseconds on a 68000. The lock is taken inside the per-channel loop, so four channels give four regions with a scheduling point between them |
| `bsd_lib_open()` blocks inside the library-list `Forbid()` Exec holds across the Open vector | `src/bsdsocket/library.c:1034` | `ObtainSemaphore` at `:1055`, `ami_netdb_load()` at `:1067` (which reads `DEVS:Internet`), `bsd_netstack_bringup()` at `:1073` and `bsd_tcp_handler_start()` at `:1110` all break it. Deliberate, and what every real `bsdsocket.library` does; `lib_OpenCnt++` at `:1053` is what holds the reference across the block |
| `bsd_netmon_dispatch()` calls an application hook with switching off | `src/bsdsocket/netmonitor.c:280` | The `Forbid()` is what makes the list walk safe against a concurrent `RemoveNetMonitorHook()`, so it cannot go, and snapshotting the list would mean allocating on the packet path. A hook that calls `Delay`, `OpenLibrary` or `ObtainSemaphore` breaks both. A trust boundary, not something the library can enforce |
| `ami_netstack_baton_release()` releases the ThreadX baton and nothing else | `src/netstack/netstack_baton.c` | It does not unwind an outstanding `TX_DISABLE` nesting. A driver entry called from inside a vendored critical section would still take its `DoIO()`'s `Wait()` with `TDNestCnt` raised; the safety rests on there being no such call site, not on the hook |
| `Disable()`, not `Forbid()`, on the SANA-II TX reply port | `src/sana2/sana2_tx.c:134`, `:151` | A device may `ReplyMsg()` from its own interrupt, and Exec's `PutMsg` reads `mp_SigTask`/`mp_SigBit`/`mp_Flags` as one `Disable()`d unit. `Forbid()` does not stop interrupts and would not be sufficient |
