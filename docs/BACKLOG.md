# Backlog
What is outstanding. Nothing else — finished work leaves a commit message and a
comment beside the code, not an entry here.
**HARD CAP: 250 LINES.** Over it, delete rows. Never relocate them; git has it.
| Item | Why it is open | Cite |
|---|---|---|
| `fetch` tries one address and gives up | It takes the first answer from the resolver and connects to that alone, so a name whose first address is unreachable costs a 191 s timeout and then fails, with the other family's working address never tried | `src/tools/fetch.c:554-583`, `tool_sock_resolve_af` |
| A clean-link transfer aborts mid-stream, rarely | `Read() gave 0 of 32768`, no connection lost. 2 arms on `c3e4e806`-based builds, base and cap alike; 0 in ~30 arms before it. Confounded: another agent's unscoped `pkill -f fitz-serve` on the shared peer produces the same signature. Needs a quiet rig to separate | `run-fitzbench.sh` `RESULT read FAILED` |
| No sender-side fast path below 3 segments in flight | RTO floor is a flat 1000 ms, fast retransmit needs 3 dupacks, and there is no Limited Transmit or TLP. An HTTP request is 2 segments, so 3 dupacks cannot be elicited and every lost outbound segment is a 1 s stall | `nx_tcp.h:262`, `nx_tcp_socket_state_ack_check.c:291` |
| `run-lossgate.sh` loses in one direction only | The u32 filter bands only frames addressed to the guest, so outbound retransmits are 0 in every run and the whole send-side recovery path ships unmeasured | `tests/perf/run-lossgate.sh` |
| The emulator repairs partial checksums on IPv4 only, two cards | `ethernet.cpp:181` returns early unless ethertype is 0x0800, so every off-LAN IPv6 segment arrives unfilled and is dropped; only a2065 and ne2000 are covered at all. Any off-LAN measurement on another card carries the artifact | `~/amiberry/src/ethernet.cpp:172-181` |
