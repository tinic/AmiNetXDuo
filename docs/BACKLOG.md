# Backlog
What is outstanding. Nothing else — finished work leaves a commit message and a
comment beside the code, not an entry here.
**HARD CAP: 250 LINES.** Over it, delete rows. Never relocate them; git has it.
| Item | Why it is open | Cite |
|---|---|---|
| One lost ARP broadcast fails a TCP arm | `NX_ARP_UPDATE_RATE` is NetX Duo's default 10 s while `iperf` gives up at 5 s, so an unanswered first ARP is never retried in time. BSD retries once per second. Intermittent, hits every card. Changing the rate moves bring-up timing, which is a tracked metric, so it needs its own measurement | `tests/tools/run-cardsweep.sh` |
| ~20 guest test programs write junk to `stdout.txt` | `t_flush()` does `Write(out, (APTR)t_log_buffer, t_log_used)` — right length, address 0, so the file is the 68000 exception vector table. `t_put()` fills the buffer correctly and the serial log is fine, which is why nobody noticed | `tests/*/…` sharing the `t_flush()` pattern |
