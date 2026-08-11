# Allocations

AmigaOS has no MMU and reclaims neither `AllocVec` nor `AllocMem` memory when a
process exits, so a Shell command that allocates and returns leaks until reboot
and a library allocation leaks until expunge. The supported floor is a 1 MB
machine.

| Rule or hazard | Where | Effect |
|---|---|---|
| `nsl_AllocLive`/`nsl_AllocPeak` count only blocks that went through `ami_alloc()` | `include/aminetxduo/netstatus.h:530` | A raw `AllocMem`, an `AllocVec` or a `CreateNewProc` stack is invisible to `NETSTATUS_HEALTH`. `AvailMem()` is the only instrument that sees one |
| Every `CreateNewProc`/`CreateNewProcTags` in the tree passes `NP_StackSize` and none passes `NP_Stack` | tree-wide | Every Process stack is DOS-allocated and DOS-freed at process exit; there is no `CreateNewProc` stack to leak |
| The two master library bases come from Exec's `MakeLibrary`, not `ami_alloc` | `src/bsdsocket/library.c`, `src/usergroup/ug_library.c` | They are released with `FreeMem` at `base - lib_NegSize`; `ami_free()` on either is a free of memory that came from somewhere else |
| A command that calls `ami_netdb_load()` owns `ami_netdb_free()` | `src/config/netdb.c` | The twelve blocks it builds out of `DEVS:Internet` are held in file-scope statics and only `ami_netdb_free()` releases them. Commands register it with `atexit()` rather than freeing before each return, because `main()` is left from many places; the call is idempotent and safe on a path that never loaded |
| `ami_mbuf_cleanup()` has no production caller | `src/mbuf/mbuf_alloc.c:79` | Only `src/mbuf/test/` and `tests/mbuf_bpf/` reach it, and every `mbuf_*` LVO is a `bsd_enosys` stub (`src/bsdsocket/bsdsocket_vectors.c:132-142`), so nothing leaks today. The moment one of those vectors is implemented every slab and cluster leaks for the life of the library: neither `netstack_shutdown()` nor the expunge calls cleanup |
| `ASF_ORPHANED` keeps a block NetX Duo refused to release | `src/bsdsocket/socket.c:1005` | Deliberate and logged, not a defect to chase |
| `bsd_ObtainSocket` re-parks through an allocation | `src/bsdsocket/handoff.c:347` | If that allocation fails the `AmiSocket` is dropped with no reference held. Fixing it needs an error path that cannot allocate |
| A `TLSConnection` is application-owned | `src/tlslib/tls_conn.c` | Nothing sweeps live connections at `tls_lib_close`/`tls_lib_expunge`; a program that forgets `TLSClose()` leaks the connection and its six buffers. `bsd_ReleaseDomainNameServerList` (`src/bsdsocket/roadshow.c`) is the same documented Roadshow contract |
| A 60-second timeout frees a live Task's stack | `tests/bracket/bracket_test.c:640`, `:714` | The wait loop breaks and `bt_reap()` runs regardless of `bt_Done`, which is the use-after-free the file documents at `:228` for the non-timeout path. `tools/smoke/lifecycle.c` has the same shape with a `Delay` as the only mitigation |
| `tests/soak/soak_test.c` leaks two Tasks and two MemLists per run by default | `:165` `S_NO_REMTASK 1` | Deliberate: it compiles out `RemTask(NULL)` and parks the Task in `Wait(0)` instead, because the free-list Guru it avoids would destroy the verdict |
