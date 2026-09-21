# Forbid regions

Nothing inside a `Forbid()` may block. Exec resumes multitasking for the
duration of any block, restores `tc_TDNestCnt` when the task is redispatched,
and the code returns believing it was never interrupted, so the damage surfaces
later and somewhere else.

| Constraint | Where | Effect |
|---|---|---|
| `ami_mbuf_lock()` is `Forbid()` and `ami_mbuf_unlock()` is `Permit()` | `src/mbuf/mbuf_amiga.c:21` | The lock drop across `ami_alloc()` in `ami_mbuf_grow()` (`src/mbuf/mbuf_alloc.c:188`) only re-enables switching at nest 1. Calling `ami_mbuf_raw_get()` from inside another mbuf region turns that `AllocVec()` into an allocation under Forbid, silently. One caller today, and it has no path that already holds the lock |
| `bsd_lib_open()` must do substantial work from an Open vector that Exec enters under its library-list `Forbid()` | `src/bsdsocket/library.c` | It increments `lib_OpenCnt` first, temporarily `Permit()`s while it takes `sb_Lock`, reads `DEVS:Internet`, starts the loopback-only stack and publishes the optional TCP: handler, then restores Exec's nesting with `Forbid()` on every return. The reference pins the resident while LibList is mutable. A caller's own outer `Forbid()` is deliberately not unwound; calling `OpenLibrary()` from such a region remains invalid. |
| `ami_netstack_baton_release()` releases the ThreadX baton and nothing else | `src/netstack/netstack_baton.c` | It does not unwind an outstanding `TX_DISABLE` nesting. A driver entry called from inside a vendored critical section would still take its `DoIO()`'s `Wait()` with `TDNestCnt` raised; the safety rests on there being no such call site, not on the hook |
| `Disable()`, not `Forbid()`, on the SANA-II TX reply port | `src/sana2/sana2_tx.c:134`, `:151` | A device may `ReplyMsg()` from its own interrupt, and Exec's `PutMsg` reads `mp_SigTask`/`mp_SigBit`/`mp_Flags` as one `Disable()`d unit. `Forbid()` does not stop interrupts and would not be sufficient |

The `anxgenet.device` data path is deliberately absent from this table.  Its
hardware interrupt only acknowledges and masks the source, then signals a
dedicated task.  DMA-ring walks, cache operations, client buffer callbacks,
PHY access and transmit construction all run with scheduling enabled; short
`Disable()` regions protect only queue and ownership transitions.  Detach
still brackets the non-blocking `RemTask()` and pointer retirement with
`Forbid()` before freeing the private task allocation.
