# Backlog
What is outstanding. Nothing else — finished work leaves a commit message and a
comment beside the code, not an entry here.
**HARD CAP: 250 LINES.** Over it, delete rows. Never relocate them; git has it.
| Item | Why it is open | Cite |
|---|---|---|
| mDNS ignores a legacy unicast query | The vendored responder rejects any query not sourced from port 5353, so RFC 6762 §6.7 queries go unanswered and `dig @224.0.0.251 -p 5353` gets nothing. Real responders always source from 5353, so normal use is unaffected | `third_party/netxduo` `nxd_mdns.c` source check |
| Read collapses at 0.5% packet loss | 4173 → 554 KB/s on a2065, write flat; ACK delay p50 6.9 ms → 195 ms, max 1009 ms. `nx_tcp_socket_ack_n_packet_counter` only ratchets up, pins above the read chunk, so every ACK waits for the 200 ms delayed-ACK timer | `nx_tcp_socket_state_data_check.c:1246-1293` |
