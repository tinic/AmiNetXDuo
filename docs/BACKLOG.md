# Backlog

What is outstanding. Nothing else — finished work leaves a commit message and a
comment beside the code, not an entry here.

**HARD CAP: 250 LINES.** Over it, delete rows. Never relocate them; git has it.

| Item | Why it is open | Cite |
|---|---|---|
| `bsd_accept()` EMFILE arm stored into a freed socket | Fixed by inspection, not by a test — it needs fd-table exhaustion coinciding with a refused relisten, and its symptom is corruption, not a wire event | `socket.c` |
| fs-uae is gone but the tree still routes through it | `tools/fsuae-run.sh` is invoked by nothing yet ~10 comments send manual work through it and it still shells out to the `fs-uae` binary; `tests/conformance/run-fsuae.sh:120` writes an fs-uae config while driving Amiberry; `netpeer.py:5` documents an fs-uae SLIRP workflow | `tools/fsuae-run.sh` |
| `alloc-census-known.txt:44` can never match | The gate key is `library.c:636`; `AMI_CENSUS_ADD` stamps `library.c:761`, so the child-base block is unaccounted in the census | `tools/alloc-census-known.txt:44` |
| `tools/smoke/lifecycle` faults on both CPUs | Reports 18/18 and exits 0, then an Exec task executes `Illegal instruction: 0008 at 0021AAF8` outside ROM. Suspect: phase e's `stuck_worker()` returns into the port's thread-exit path after its `TX_THREAD` was deleted, and `main()` frees the zombie's stack 500 ms later — which `tx_thread_schedule.c:136` forbids. The SANA-II reader case that comment names has the same shape | `tools/smoke/lifecycle.c`, `port/threadx-amiga/src/tx_thread_schedule.c:136` |
| `eb920` carries nothing | Comes online with a DHCP lease, then hangs on its first TCP send and hits the 300 s ceiling with zero bytes either way. Two independent sweeps | `tests/tools/run-cardsweep.sh` |
| Three conformance failures | `accept(): EWOULDBLOCK when non-blocking`, `recv(MSG_OOB)`, `WaitSelect(): exceptfds detects OOB`. 127/142 pass. Two may already be closed by `ecbb29b0` and `7446c5be`, which landed after that run | `tests/conformance/` |
