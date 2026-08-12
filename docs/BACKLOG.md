# Backlog
What is outstanding. Nothing else — finished work leaves a commit message and a
comment beside the code, not an entry here.
**HARD CAP: 250 LINES.** Over it, delete rows. Never relocate them; git has it.
| Item | Why it is open | Cite |
|---|---|---|
| The timer wheel runs 100 ms late on real hardware | Peak skew 5 ticks on an X-Surf-100, nothing lost or deferred. The delayed-ACK timer is 200 ms, so timers can fire half a period late. Only non-zero number in an otherwise clean health block | `netstat -s` `nsl_TickSkewPeak`, `include/aminetxduo/netstatus.h:510` |
| TCP discards packets that arrived intact | 6 `dropped on receipt` in a 10 MB transfer on real hardware, with 0 receive errors, 0 overruns and 0 checksum errors, so the card delivered them and the stack threw them away. Reason unknown | `nsx_TcpReceiveDropped`, `src/bsdsocket/netstatus.c:815` |
| The lab emulator has no IPv6 repair built in yet | `4c4cff0a` merged the IPv6 partial-checksum fix, but `~/amiberry/build/amiberry` is still the 03:38 binary because measurements were running against it. Every off-LAN IPv6 figure until it is rebuilt carries the old artifact | `~/amiberry` on playhouse3 |
| `fetch` tries one address and gives up | It takes the first answer from the resolver and connects to that alone, so a name whose first address is unreachable costs a 191 s timeout and then fails, with the other family's working address never tried | `src/tools/fetch.c:554-583`, `tool_sock_resolve_af` |
| A clean-link transfer aborts mid-stream, rarely | `Read() gave 0 of 32768`, no connection lost. 2 arms on `c3e4e806`-based builds, base and cap alike; 0 in ~30 arms before it. Confounded: another agent's unscoped `pkill -f fitz-serve` on the shared peer produces the same signature. Needs a quiet rig to separate | `run-fitzbench.sh` `RESULT read FAILED` |
| The X-Surf family has no working IPv6 | X-Surf, X-Surf-100 Z2 and Z3 take a DHCP lease and answer 20/20 pings, and an RA gives them a global address, but every IPv6 TCP connect runs to the 191 s ceiling and fails. Identical with the emulator checksum repair on and off, so it is not that. Three shipping cards | emulator or stack, not separated |
| `uaenet.device` never finishes coming up | With `sana2=true` on a bridged unit the pcap thread starts and the device initialises, then `AddNetInterface` never returns | emulator or stack, not separated |
