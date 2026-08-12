# Backlog
What is outstanding. Nothing else — finished work leaves a commit message and a
comment beside the code, not an entry here.
**HARD CAP: 250 LINES.** Over it, delete rows. Never relocate them; git has it.
| Item | Why it is open | Cite |
|---|---|---|
| The release e2e depends on `ftp.gnu.org` | It rate-limits the lab and refuses v4 and v6 for a while, so the arm's verdict is partly someone else's policy. The host was chosen for its 240 s handshake budget, which a replacement must also hold | `install/test/run-workbench.sh` |
| No sender-side fast path below 3 segments in flight | RTO floor is a flat 1000 ms, fast retransmit needs 3 dupacks, and there is no Limited Transmit or TLP. An HTTP request is 2 segments, so 3 dupacks cannot be elicited and every lost outbound segment is a 1 s stall | `nx_tcp.h:262`, `nx_tcp_socket_state_ack_check.c:291` |
| `run-lossgate.sh` loses in one direction only | The u32 filter bands only frames addressed to the guest, so outbound retransmits are 0 in every run and the whole send-side recovery path ships unmeasured | `tests/perf/run-lossgate.sh` |
| A socket goes silent for 127 s with nothing surfaced | RTO ladder 1,2,4,8,16,32,64 s then ECONNRESET; 191 s on connect. `recv()` blocks forever by default and nothing reports the retries. This is the "hangs, then restarts" users describe | `nx_tcp_socket_retransmit.c:245`, `nx_user.h:244` |
| A stray SYN rewrites MSS and SACK mid-connection | The block is inside `if (SYN)` but not state-guarded, unlike the timestamp block below it; one late SYN drops MSS to 536 and turns SACK off on an ESTABLISHED connection | `nx_tcp_packet_process.c:449-483` |
| IPv6 RA timer values truncate to zero | `time_val * NX_IP_FAST_TIMER_RATE / 1000` is 0 for any RetransTimer under 100 ms, and ReachableTime under 1000 ms likewise; ND resolution then re-arms to 0 ticks | `nx_icmpv6_process_ra.c:196,217` |
| 4 TCP checksum errors per e2e run, unexplained | Neither known cause applies: the peer's TX offload is already off, and the emulator has the receive-side fill. So these are either real corruption or a defect | `netstat -s` in the e2e guest |
| mDNS ignores a legacy unicast query | The vendored responder rejects any query not sourced from port 5353, so RFC 6762 §6.7 queries go unanswered and `dig @224.0.0.251 -p 5353` gets nothing. Real responders always source from 5353, so normal use is unaffected | `third_party/netxduo` `nxd_mdns.c` source check |
| Read collapses at 0.5% packet loss | 4173 → 554 KB/s on a2065, write flat; ACK delay p50 6.9 ms → 195 ms, max 1009 ms. `nx_tcp_socket_ack_n_packet_counter` only ratchets up, pins above the read chunk, so every ACK waits for the 200 ms delayed-ACK timer | `nx_tcp_socket_state_data_check.c:1246-1293` |
