# Backlog
What is outstanding. Nothing else — finished work leaves a commit message and a
comment beside the code, not an entry here.
**HARD CAP: 250 LINES.** Over it, delete rows. Never relocate them; git has it.
| Item | Why it is open | Cite |
|---|---|---|
| One lost ARP broadcast fails a TCP arm | `NX_ARP_UPDATE_RATE` is NetX Duo's default 10 s while `iperf` gives up at 5 s, so an unanswered first ARP is never retried in time. BSD retries once per second. Intermittent, hits every card. Changing the rate moves bring-up timing, which is a tracked metric, so it needs its own measurement | `tests/tools/run-cardsweep.sh` |
| Eight guest test programs start ThreadX and never stop it | The VERTB server's `struct Interrupt` and `is_Code` live in the program's hunk; AmigaDOS frees it on exit and the next VBlank runs whatever replaced it. Only `soak_test` calls `tx_amiga_kernel_stop()`. `ram_driver_test`, `mbuf_bpf_test`, `tls_handshake`, `bracket_test` (both), `perf_test`, `tcpprof`, `ipv6_test` survive by luck; four are in the emulator tier. Each needs its ThreadX objects deleted before `stop` will accept | `tools/smoke/kernelstop.c` is the model |
