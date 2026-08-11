# Exec resources

Signal bits, message ports and I/O requests, for the constraints that are not
visible at the call site. A `Task` has 32 signal bits and no way to recover one
except `FreeSignal()` on that same Task, and a `MsgPort` names the Task whose
bit it holds.

| Constraint | Where | Effect |
|---|---|---|
| A signal bit taken for a child base comes out of the opening task and must go back in `bsd_child_destroy()` | `src/bsdsocket/library.c:891`, `:903` | `sb_TimerSignal` and `sb_EventSignal` are both freed there. A per-base signal added without a matching free costs its opener one of 32 bits per open/close cycle, permanently; `tests/tools/run-cycledrill.sh` reads `tc_SigAlloc` because that is the only place it shows |
| `sb_TimerPort` is a cross-task port waiting to happen | `src/bsdsocket/bsdsocket_internal.h:596` | Its `mp_SigBit` is allocated by the task that called `WaitSelect()`, its `mp_SigTask` is the task that called `OpenLibrary()`. They are the same task only because `sb_CanShareBases` is `FALSE` and nothing acts on it. If `SBTC_CAN_SHARE_LIBRARY_BASES` is ever honoured, `timer.device` signals task A on a bit owned by task B and B's timeout never fires |
| `bsd_child_destroy()` is only correct on the owning task | `src/bsdsocket/library.c:349` | A foreign caller must not destroy the base: `ami_signal_free()` would free a bit out of the wrong Task |
| `tcp_session_main()` stops draining its port on `ACTION_END` | `src/bsdsocket/tcp_handler.c:866` | The inner loop breaks without emptying the queue and `DeleteMsgPort()` follows at `:906`. Nothing sends a packet to a file handle after `Close()`, so it is not live; a second task using the same handle would be |
| `tx_amiga_adopt_thread()` discards `_tx_amiga_thread_park()`'s answer | `port/threadx-amiga/src/tx_amiga_adopt.c:298` | It returns `TX_SUCCESS` even when park reports the thread was torn down, unlike `tx_amiga_adopt_resume()` (`:453`), which checks it |
| `_tx_initialize_low_level()`'s `AllocSignal` is never freed in the `tx_kernel_enter()` shape | `port/threadx-amiga/inc/tx_amiga.h:35` | A standalone program may call `tx_kernel_enter()` from `main()` instead of `tx_amiga_kernel_start()`; then the bit is allocated in the application's own Task and `_tx_amiga_kernel_task_entry()` never runs to free it. Nothing in this project uses that shape |
