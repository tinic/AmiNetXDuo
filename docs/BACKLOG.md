# Backlog
What is outstanding. Nothing else — finished work leaves a commit message and a
comment beside the code, not an entry here.
**HARD CAP: 250 LINES.** Over it, delete rows. Never relocate them; git has it.
| Item | Why it is open | Cite |
|---|---|---|
| `tools/smoke/lifecycle` faults on both CPUs | Reports 18/18 and exits 0, then an Exec task executes `Illegal instruction: 0008 at 0021AAF8` outside ROM. Suspect: phase e's `stuck_worker()` returns into the port's thread-exit path after its `TX_THREAD` was deleted, and `main()` frees the zombie's stack 500 ms later — which `tx_thread_schedule.c:136` forbids. The SANA-II reader case that comment names has the same shape | `tools/smoke/lifecycle.c`, `port/threadx-amiga/src/tx_thread_schedule.c:136` |
| One lost ARP broadcast fails a TCP arm | `NX_ARP_UPDATE_RATE` is NetX Duo's default 10 s while `iperf` gives up at 5 s, so an unanswered first ARP is never retried in time. BSD retries once per second. Intermittent, hits every card. Changing the rate moves bring-up timing, which is a tracked metric, so it needs its own measurement | `tests/tools/run-cardsweep.sh` |
